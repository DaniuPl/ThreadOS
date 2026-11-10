#!/usr/bin/env python3
"""ThreadOS smoke test: a TCP server in ring 3, and gweb, the small
graphical browser.

tcpserv (user/tcpserv.c) is the passive open seen from a program: it
listens on a port, and the host's own TCP stack connects to it through
QEMU's port forwarding, gets a greeting and has what it sends echoed
back. Then the server is killed with Ctrl+C while it waits for the next
client: the one started after it must get the same port, and once that
one is killed too, a connection from the host must find nobody there
and leave nothing in the kernel's table (netstat).

gweb (user/gweb.c) is started from the kernel's shell on the desktop.
It looks a host up by name through a DNS server the test puts up,
fetches a page from the test's HTTP server and draws its text; PgDn
scrolls it, a link stands out in the accent colour, and a click on the
link fetches the second page. What it fetched last it saves to the
disk as page.htm, which a second boot of the same disk, at the text
shell, reads back.

The DNS server has to sit on port 53, which an unprivileged user may
not bind. Without it gweb is given the server's address instead of its
name: everything after the lookup is still checked, the lookup itself
is not, and the run says so with a SKIPPED line at its start and at its
end.

Run on its own or through scripts/run_tests.py, which starts all the
groups in parallel.
"""

import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, no64, DNS_HINT

PROMPT = "thread64>"
SRV_PORT = 5555                  # user/tcpserv.c listens on this one
# What tcpserv says first: it has the port, or it could not get it
SERVER_START = re.compile(r"\[tcpserv\] (listening on|I cannot listen on) port")
# The shell's whole line once tcpserv has ended, killed or not - up to
# its closing parenthesis, so that nothing is read off half a line
SERVER_ENDED = re.compile(r"\[tcpserv\] done, exit code -?\d+ \([^)\n]*\)")


def recv_until(sock, want, timeout=15):
    """Reads from the socket until 'want' has arrived, the other side
    closed or the time ran out - a stream may hand a reply over in more
    than one piece. Returns everything read."""
    got = b""
    end = time.time() + timeout
    while want not in got and time.time() < end:
        sock.settimeout(max(0.1, end - time.time()))
        try:
            part = sock.recv(200)
        except socket.timeout:
            break
        if not part:
            break
        got += part
    return got


def fresh_until(q, start, pattern, timeout):
    """What arrived on the serial port since 'start', once 'pattern'
    matches it (or when the time is up)."""
    end = time.time() + timeout
    while time.time() < end:
        fresh = q.serial()[start:]
        if pattern.search(fresh):
            return fresh
        time.sleep(0.3)
    return q.serial()[start:]


def knock(port, timeout=20):
    """Connects to the forwarded port and waits until the connection is
    closed from the other end (or the time runs out). QEMU takes the
    host's connection itself and only then sends a SYN to the guest, so
    the guest's refusal shows as a connection that opens and ends."""
    try:
        sock = socket.create_connection(("127.0.0.1", port), timeout=timeout)
    except OSError:
        return
    try:
        end = time.time() + timeout
        while time.time() < end:
            sock.settimeout(max(0.1, end - time.time()))
            try:
                if not sock.recv(200):
                    return
            except socket.timeout:
                return
            except OSError:
                return                   # reset: closed all the same
    finally:
        sock.close()


def tcp_server():
    """ThreadOS as a TCP server: a passive open from ring 3."""
    print("--- the network: a TCP server in ring 3 ---")
    q = Qemu(KERNEL, INITRD, hostfwd_port=SRV_PORT, append=TEXT_MODE)
    try:
        q.wait_serial(PROMPT, timeout=60)
        # Without an address nobody can reach it, forwarding or not
        q.wait_serial("[net] DHCP: granted", timeout=40)
        view = q.command("run tcpserv", "listening on port %d" % SRV_PORT,
                         timeout=30)
        check("ring 3 listens on a TCP port",
              "[tcpserv] listening on port %d" % SRV_PORT in view)

        # The host's real stack connects, through QEMU's hostfwd
        sock = socket.create_connection(("127.0.0.1", SRV_PORT), timeout=15)
        try:
            greeting = recv_until(sock, b"ThreadOS TCP ready")
            check("the host connected and got a greeting from ThreadOS",
                  b"ThreadOS TCP ready" in greeting)

            sock.sendall(b"hello from the host\n")
            echoed = recv_until(sock, b"hello from the host")
            check("the host's data came back echoed by the ring 3 server",
                  b"hello from the host" in echoed)
        finally:
            sock.close()

        heard = "[tcpserv] from the client: hello from the host"
        said = ""
        end = time.time() + 15
        while time.time() < end and heard not in said:
            said = q.serial()
            time.sleep(0.3)
        check("the server saw the client's data", heard in said)

        # A listening socket must not outlive its program. The server is
        # back in accept by now, waiting for its second client: Ctrl+C
        # kills it there, with the port still open, and the next server
        # asks for the same port at once.
        seen = len(q.serial())
        q.sendkey("ctrl-c")
        ended = fresh_until(q, seen, SERVER_ENDED, 30)
        check("Ctrl+C kills the server while it waits for a client",
              "(tcpserv) killed: a signal" in ended)

        seen = len(q.serial())
        q.type_line("run tcpserv")
        again = fresh_until(q, seen, SERVER_START, 30)
        check("the port of a killed server is free for the next one",
              "[tcpserv] listening on port %d" % SRV_PORT in again)
        sock = socket.create_connection(("127.0.0.1", SRV_PORT), timeout=15)
        try:
            greeting = recv_until(sock, b"ThreadOS TCP ready")
            check("the host reaches the server started in its place",
                  b"ThreadOS TCP ready" in greeting)
        finally:
            sock.close()

        # And with nobody started in its place, the port of a killed
        # server is a port nobody listens on. The kernel used to take the
        # dead listener's own table entry for the new connection, which
        # then stayed there for good - so after the host has knocked,
        # the table has to be empty.
        seen = len(q.serial())
        q.sendkey("ctrl-c")
        ended = fresh_until(q, seen, SERVER_ENDED, 30)
        knock(SRV_PORT)
        table = q.command("netstat", "no TCP connections", timeout=15)
        check("a knock on a killed server's port leaves no connection behind",
              "(tcpserv) killed: a signal" in ended and
              "no TCP connections" in table)
    finally:
        q.close()


def main():
    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return summary()

    tcp_server()

    # --- The browser in a desktop window (network -> GUI -> disk) ---
    print("--- the desktop: the browser (gweb) ---")
    wdisk = tempfile.mktemp(suffix=".img")
    with open(wdisk, "wb") as f:
        f.write(b"\0" * DISK_SIZE)
    http_port, http_stop = start_http_server()
    dns_stop, dns_ok = start_dns_server()
    if dns_ok:
        host = "threados.test"
    else:
        # The browser looks the page up by NAME when a name server of
        # ours can be put up - and port 53 is out of an unprivileged
        # user's reach. Everything after the lookup (the fetch, the
        # drawing, scrolling, the link, the page saved to the disk)
        # does not depend on it, so it is checked by address then.
        print("SKIPPED (the browser's name lookup): " + DNS_HINT)
        print("      gweb is given the address instead - nothing below "
              "checks DNS")
        host = "10.0.2.2"
    # The serial port says when the lease came and what the shell did
    slog = tempfile.mktemp(suffix=".log")
    q_web = Qemu(KERNEL, INITRD, wdisk, serial=slog)
    try:
        q_web.wait_desktop()
        # The lease first: it brings a name server of its own, which
        # would replace the one set below
        q_web.wait_serial("[net] DHCP: granted", timeout=30)

        # The host's DNS server: the browser will look the address up by
        # name. The command goes to the kernel's shell - in graphics mode
        # it listens to the keyboard as long as no window has focus.
        if dns_ok:
            q_web.command("dns 10.0.2.2", "DNS server set to 10.0.2.2",
                          timeout=10)
        # The shell waits for the browser to end; the keys go to its
        # window from here on
        q_web.type_line("run gweb")
        time.sleep(1.5)

        ink = theme("dark")["app_text"]

        def page_white():
            """How much text is on the page - under the toolbar and the
            status line: pixels in the theme's text colour, or nearly"""
            _, _, a = q_web.screen_raw()
            return sum(1 for y in range(140, 390) for x in range(96, 520)
                       if near(a(x, y), ink, 40))

        before = page_white()

        # Calibrating the cursor - clicks on links have to land to the pixel,
        # and mouse deltas count from the current position
        q_web.mouse_home()

        # A click in the window gives it keyboard focus; then the cursor goes
# aside
        q_web.mouse_goto(150, 120)
        q_web.mouse_click()
        q_web.mouse_goto(400, 350)
        time.sleep(0.3)

        for _ in range(20):              # clear the default address
            q_web.sendkey("backspace")
        for ch in "%s:%d/" % (host, http_port):   # an address by name
            q_web.sendkey(Qemu.KEYNAMES.get(ch, ch))
        q_web.sendkey("ret")

        shown = False
        for _ in range(25):
            if page_white() > before + 50:
                shown = True
                break
            time.sleep(0.4)
        check("the browser resolved the name, fetched the page and "
              "drew the text" if dns_ok else
              "the browser fetched the page by address and drew the text",
              shown)

        # --- scrolling and links ---
        for _ in range(40):              # a new address: a page with a link
            q_web.sendkey("backspace")
        for ch in "%s:%d/links" % (host, http_port):
            q_web.sendkey(Qemu.KEYNAMES.get(ch, ch))
        q_web.sendkey("ret")
        time.sleep(5)

        link_ink = theme("dark")["accent"]

        def link_pixels():
            """Links are drawn in the accent - which is what a link has
            been since there were links - and underlined"""
            _, _, a = q_web.screen_raw()
            return [(x, y) for y in range(140, 390) for x in range(96, 520)
                    if near(a(x, y), link_ink, 20)]

        first_view = page_white()
        q_web.sendkey("pgdn")
        time.sleep(1.0)
        check("PgDn scrolls the content in the window", page_white() != first_view)

        found = []
        for _ in range(8):               # scroll to the end, where the link is
            found = link_pixels()
            if found:
                break
            q_web.sendkey("pgdn")
            time.sleep(0.8)
        check("the link stands out in colour in the text", bool(found))

        if found:
            # A link can occupy two lines - the centre of gravity of all the
            # pixels would then fall in the gap between them. We take the
            # densest line and its centre.
            rows = {}
            for x, y in found:
                rows.setdefault(y, []).append(x)
            best_y = max(rows, key=lambda y: len(rows[y]))
            xs = sorted(rows[best_y])
            cx, cy = xs[len(xs) // 2], best_y
            q_web.mouse_goto(cx, cy)
            q_web.mouse_click()
            q_web.mouse_goto(560, 400)
            time.sleep(5)
    finally:
        q_web.close()
        http_stop()
        dns_stop()
        if os.path.exists(slog):
            os.unlink(slog)

    # The same disk in text mode: content from the world survived the
    # restart. The browser wrote where it was started: the home folder
    # the signed-in session's shell stands in - which is the account's
    # own, so the text shell signs in to it too, and lands there.
    q_web2 = Qemu(KERNEL, INITRD, wdisk, append=TEXT_MODE)
    try:
        q_web2.wait_for("thread64>")
        q_web2.type_line("login admin")
        q_web2.command("thread", "logged in as", timeout=15)
        saved = q_web2.command("cat page.htm", "The second page",
                               timeout=15)
        if "The second page" not in saved:
            print("      the shell said: %r" % saved[-300:])
        check("the clicked link fetched the second page (it shows on the disk)",
              "The link worked" in saved)
        check("the page the browser saved survived the restart",
              "<html><body><h1>The second page</h1>" in saved)
    finally:
        q_web2.close()
        if os.path.exists(wdisk):
            os.unlink(wdisk)

    if not dns_ok:
        # Said again at the end, where a reader of the result looks:
        # the checks above passing is not a word about DNS
        print("SKIPPED (the browser's name lookup, by address instead): "
              + DNS_HINT)
    return summary()


if __name__ == "__main__":
    sys.exit(main())
