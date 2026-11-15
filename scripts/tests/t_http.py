#!/usr/bin/env python3
"""ThreadOS smoke test: TCP and HTTP from ring 3, onto the disk.

On the other side stands a real stack - an HTTP server in Python on the
host - and not a loopback of our own. So the client has to reach
agreement with something we did not write: negotiate a connection, read
the response, follow a 302 redirect and assemble a body sent in pieces.

The second thing this is about is the path network -> disk. The files
fetched land on TFS, and their sizes are chosen so as to go through all
three levels of an inode's addressing: direct, indirect and doubly
indirect. They are read back through the kernel, and once the machine
is off the disk image is read on the host as well and compared byte for
byte with what the server sent. At the end the machine comes up a
second time on the same disk - the largest files are to have their full
size after the restart.

Everything is read from the serial port: GRUB gives the kernel a
framebuffer, so the VGA character memory says nothing.

Run on its own or through scripts/run_tests.py, which starts all the
groups in parallel.
"""

import os
import re
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, start_http_server, TfsImage

PROMPT = "thread64>"


def main():
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.write(b"\0" * DISK_SIZE)
    slog = tempfile.mktemp(suffix=".log")

    q = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE, serial=slog)
    try:
        q.wait_serial(PROMPT, timeout=90)
        q.wait_serial("[net] DHCP", timeout=30)
        time.sleep(0.5)

        def get(args, timeout=60):
            return q.command("run httpget 10.0.2.2 %d %s" % (http_port, args),
                             "[httpget] done, exit code", timeout=timeout)

        def size_in_fls(name):
            m = re.search(r"%s  \((\d+) B\)" % re.escape(name),
                          q.command("fls", "Used:", timeout=20))
            return int(m.group(1)) if m else None

        http_port, http_stop = start_http_server()
        try:
            page = get("/")
            check("TCP: a handshake and an HTTP 200 answer",
                  "[httpget] HTTP 200" in page)
            check("TCP: the page's content arrived whole",
                  "ThreadOS TCP works" in page)
            check("httpget ended without an error",
                  "[httpget] done, exit code 0" in page)

            # Network -> disk: an HTTP body written to TFS and read back
            get("/ page.htm")
            saved = q.command("cat page.htm", PROMPT)
            check("content from the network written to the TFS and read "
                  "off the disk", "<html>ThreadOS TCP works</html>" in saved)
            check("only the content went into the file, without the HTTP "
                  "headers", "Content-Length" not in saved
                  and "HTTP/1" not in saved)

            # A file well above what the direct blocks hold: it reaches
            # into the indirect block
            big = get("/big big.htm", timeout=120)
            got = re.search(r"content (\d+) B", big)
            check("a big page fetched whole (%d B of content)"
                  % len(BIG_PAGE),
                  got is not None and int(got.group(1)) == len(BIG_PAGE))
            check("a file past the indirect block's start written at its "
                  "full size", size_in_fls("big.htm") == len(BIG_PAGE))
            usage = q.command("fls", "Used:", timeout=20)
            check("the listing shows the disk usage in blocks",
                  re.search(r"Used: \d+/\d+ blocks of 1024 B", usage)
                  is not None)

            # A file above the old ceiling of 268 KiB: the data reach
            # the doubly indirect block
            huge = get("/huge huge.htm", timeout=240)
            check("a file past 268 KiB fetched whole (%d B)"
                  % len(HUGE_PAGE),
                  "content %d B" % len(HUGE_PAGE) in huge)
            check("written to the TFS at its full size",
                  size_in_fls("huge.htm") == len(HUGE_PAGE))

            # Read back through the kernel by another program, which
            # counts what it got
            wc = q.command("run core wc huge.htm", "[core] done",
                           timeout=180).replace("\r", "")
            m = re.search(r"^\s*(\d+)\s+(\d+)\s+(\d+) huge\.htm\s*$", wc,
                          re.M)
            check("reading through a doubly indirect block gives every "
                  "line and byte back (%s)" % (m.group(0).strip() if m
                                               else "no count"),
                  m is not None and int(m.group(1)) == 6500
                  and int(m.group(3)) == len(HUGE_PAGE))

            # HTTP/1.1: a 302 redirect and a body in pieces (chunked) -
            # that is how most real servers answer
            hop = get("/old")
            check("the client follows a 302 redirect",
                  "[httpget] redirect -> /chunks" in hop)
            check("content in chunks was put together correctly",
                  "HTTP 200 (chunked), content %d B" % len(PAGE) in hop)
            check("the content is whole once put together",
                  "ThreadOS TCP works" in hop)
        finally:
            http_stop()
    finally:
        q.close()

    # The large files are to survive a restart with their full size:
    # their inodes reach into the indirect blocks, and the allocation
    # bitmap comes back from the disk
    print("--- a second boot (the same disk) ---")
    slog2 = tempfile.mktemp(suffix=".log")
    q2 = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE, serial=slog2)
    try:
        q2.wait_serial(PROMPT, timeout=90)
        q2.wait_serial("[net] DHCP", timeout=30)
        time.sleep(0.5)
        listing = q2.command("fls", "Used:", timeout=20)
        check("a big file (>100 KiB) survived the restart at its full size",
              "big.htm  (%d B)" % len(BIG_PAGE) in listing)
        check("and the one past 268 KiB too",
              "huge.htm  (%d B)" % len(HUGE_PAGE) in listing)
        m = re.search(r"Used: (\d+)/\d+ blocks", listing)
        need = (len(BIG_PAGE) + len(HUGE_PAGE)) // 1024
        check("the disk usage was rebuilt from the bitmap after the restart"
              + (f" ({m.group(1)} blocks)" if m else ""),
              m is not None and int(m.group(1)) > need)
    finally:
        q2.close()

    # The image read on the host, by code that knows the layout and not
    # the kernel's code: every byte as the server sent it
    try:
        img = TfsImage(disk)
        check("on the disk image: the big file is what the server sent",
              img.read("/big.htm") == BIG_PAGE)
        check("on the disk image: the file past 268 KiB too, byte for byte",
              img.read("/huge.htm") == HUGE_PAGE)
        taken, past, free_held = img.leaks()
        check(f"on the disk image: every block taken belongs to a file "
              f"({len(taken)} taken but not held, {len(past)} past a "
              f"file's end, {len(free_held)} held but free)",
              not taken and not past and not free_held)
    finally:
        for f in (disk, slog, slog2):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
