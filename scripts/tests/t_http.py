#!/usr/bin/env python3
"""ThreadOS smoke test: HTTP from ring 3, and the path from the network
to the disk.

On the other side stands a real stack - an HTTP server in Python on the
host - and not a loopback of our own. So the client has to reach
agreement with something we did not write: follow a 302 redirect and
assemble a body sent in pieces (chunked), the way most real servers
answer. The plain fetch and HTTPS are in t_tls64.py.

The second thing this is about is the path network -> disk. The files
fetched land on TFS, and their sizes are chosen so as to go through all
three levels of an inode's addressing: direct, indirect and doubly
indirect. At the end the machine comes up a second time on the same
disk - the large file is to have its full size after the restart, and
the disk usage is to be rebuilt from the allocation bitmap.

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
from ptest import check, summary, no64, start_http_server

PROMPT = "thread64>"
DONE = "[httpget] done, exit code"
# The shell's whole line once the program has ended - up to its closing
# parenthesis, so that the exit code is never read off half a line
DONE_LINE = re.compile(re.escape(DONE) + r" (-?\d+) \([^)\n]*\)")


def fetch(q, cmd, timeout):
    """Runs httpget from the shell and hands back what arrived on the
    serial port, waiting for the shell's complete 'done' line (or the
    timeout). Everything the program printed comes before that line."""
    start = len(q.serial())
    q.type_line(cmd)
    end = time.time() + timeout
    while time.time() < end:
        fresh = q.serial()[start:]
        if DONE_LINE.search(fresh):
            return fresh
        time.sleep(0.3)
    return q.serial()[start:]


def exit_code(out):
    m = DONE_LINE.search(out)
    return int(m.group(1)) if m else None


def main():
    print("--- HTTP from ring 3, and the network -> disk path ---")
    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return summary()
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.write(b"\0" * DISK_SIZE)

    q = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE)
    try:
        q.wait_serial(PROMPT, timeout=60)
        # The address comes from slirp's DHCP server only after the
        # shell has started - without waiting for it every fetch would
        # be a race with the lease
        q.wait_serial("[net] DHCP: granted", timeout=40)

        http_port, http_stop = start_http_server()
        try:
            # Network -> disk: an HTTP body written to TFS and read back
            # from it
            got = fetch(q, "run httpget 10.0.2.2 %d / page.htm" % http_port,
                        timeout=60)
            check("httpget ended without an error (exit code %s)"
                  % exit_code(got), exit_code(got) == 0)
            saved = q.command("cat page.htm", PROMPT, timeout=20)
            check("content from the network written to the TFS and read "
                  "off the disk",
                  "<html>ThreadOS TCP works</html>" in saved)
            check("only the content went into the file, without the HTTP "
                  "headers", "Content-Length" not in saved)

            # TFS3: a file well above the old limit of 16 KiB per file
            big = fetch(q, "run httpget 10.0.2.2 %d /big big.htm"
                        % http_port, timeout=120)
            # httpget gives the length of the body alone (without the
            # headers)
            m = re.search(r"content (\d+) B", big)
            check("a big page fetched whole (%d B of content)"
                  % len(BIG_PAGE),
                  m is not None and int(m.group(1)) == len(BIG_PAGE))
            listing = q.command("fls", PROMPT, timeout=20)
            check("a file past the old 16 KiB limit written to the TFS",
                  "big.htm  (%d B)" % len(BIG_PAGE) in listing)
            usage = q.command("df", PROMPT, timeout=20)
            check("df shows the disk usage in blocks",
                  re.search(r"disk: \d+ KiB, used \d+ KiB, free \d+ KiB "
                            r"\(blocks of 1024 B\)", usage) is not None)

            # A file above the old ceiling of 268 KiB: the data reach the
            # doubly indirect block, and then we read them back
            huge = fetch(q, "run httpget 10.0.2.2 %d /huge huge.htm"
                         % http_port, timeout=240)
            check("a file past 268 KiB fetched whole (%d B)"
                  % len(HUGE_PAGE),
                  "content %d B" % len(HUGE_PAGE) in huge)
            listing = q.command("fls", PROMPT, timeout=20)
            check("written to the TFS at its full size",
                  "huge.htm  (%d B)" % len(HUGE_PAGE) in listing)

            # Counted by a program reading the file through the system
            # calls, and compared with the same counts made here on the
            # bytes the server sent
            back = q.command("run core wc huge.htm", "[core] done, exit code",
                             timeout=240)
            m = re.search(r"^\s*(\d+)\s+(\d+)\s+(\d+) huge\.htm", back, re.M)
            want = (HUGE_PAGE.count(b"\n"), len(HUGE_PAGE.split()),
                    len(HUGE_PAGE))
            check("reading through a doubly indirect block gives the same "
                  "data back (lines, words, bytes %s, want %s)"
                  % (m.groups() if m else None, want),
                  m is not None and
                  tuple(int(v) for v in m.groups()) == want)

            # HTTP/1.1: a 302 redirect and a body in pieces (chunked) -
            # that is how most real servers answer
            hop = fetch(q, "run httpget 10.0.2.2 %d /old" % http_port,
                        timeout=60)
            check("the client follows a 302 redirect",
                  "[httpget] redirect -> /chunks" in hop)
            check("content in chunks was put together correctly",
                  "HTTP 200 (chunked), content %d B" % len(PAGE) in hop)
            check("the content is whole once put together",
                  "ThreadOS TCP works" in hop)
        finally:
            http_stop()

        out = q.command("netstat", PROMPT, timeout=20)
        check("netstat: after the close no connections hang about",
              "no TCP connections" in out)
    finally:
        q.close()

    # The large files are to survive a restart with their full size:
    # their inodes reach into indirect blocks, and the allocation bitmap
    # comes back from the disk
    print("--- a second boot (the same disk) ---")
    q2 = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE)
    try:
        q2.wait_serial(PROMPT, timeout=60)
        listing = q2.command("fls", PROMPT, timeout=20)
        check("a big file (>100 KiB) survived the restart at its full size",
              "big.htm  (%d B)" % len(BIG_PAGE) in listing)
        usage = q2.command("df", PROMPT, timeout=20)
        m = re.search(r"used (\d+) KiB", usage)
        # The data of the files listed take at least this many blocks
        # (their index blocks come on top). swap.sys is not among them:
        # 'fls' keeps it from a plain user. It is made anew at every
        # start, AFTER the bitmap has been read, so it is in the usage
        # even when the bitmap came back empty - which is why its size,
        # as the boot reported it, is taken off before comparing. What
        # is left is what the bitmap brought back from the disk, and an
        # empty bitmap brings back next to nothing: the swap file's own
        # index blocks.
        # (the serial port ends a line with "\n\r")
        files = re.findall(r"^  (\S+)  \((\d+) B\)",
                           listing.replace("\r", ""), re.M)
        least = sum((int(size) + 1023) // 1024 for name, size in files
                    if not name.endswith("/") and name != "swap.sys")
        sw = re.search(r"\[ OK \] Page swapping: (\d+) KiB", q2.serial())
        rest = int(m.group(1)) - int(sw.group(1)) if m and sw else None
        check("the disk usage was rebuilt from the bitmap after the restart "
              "(%s KiB used, %s of them swap made at this boot; the %d "
              "files listed hold %d KiB)"
              % (m.group(1) if m else None, sw.group(1) if sw else None,
                 len(files), least),
              rest is not None and
              least >= (len(BIG_PAGE) + len(HUGE_PAGE)) // 1024 and
              rest >= least)
    finally:
        q2.close()
        if os.path.exists(disk):
            os.unlink(disk)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
