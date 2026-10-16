#!/usr/bin/env python3
"""ThreadOS smoke test: TCP and HTTP in the 32-bit kernel.

On the other side stands a real stack - an HTTP server in Python on the
host - and not a loopback of our own. So the client has to reach
agreement with something we did not write: negotiate a connection, read
the response, follow a 302 redirect and assemble a body sent in pieces.

The second thing this is about is the path network -> disk. The files
fetched land on TFS, and their sizes are chosen so as to go through all
three levels of an inode's addressing: direct, indirect and doubly
indirect. At the end the machine comes up a second time on the same
disk - the largest file is to have its full size after the restart.

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
from ptest import check, summary, start_http_server


def main():
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.write(b"\0" * DISK_SIZE)

    q = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE)
    try:
        q.wait_for("ThreadOS shell", timeout=40)
        q.wait_for("[net] DHCP", timeout=30)
        time.sleep(0.5)

        http_port, http_stop = start_http_server()
        try:
            q.type_line("clear")
            q.type_line("run httpget 10.0.2.2 %d /" % http_port)
            page = q.wait_for("content ", timeout=30)
            check("TCP: a handshake and an HTTP 200 answer",
                  "[httpget] HTTP 200" in page)
            check("TCP: the page's content arrived whole",
                  "ThreadOS TCP works" in page)
            check("httpget ended without an error",
                  "process \'httpget\' has ended (code 0)" in page)

            # Network -> disk: an HTTP body written to TFS and read back from it
            q.type_line("clear")
            q.type_line("run httpget 10.0.2.2 %d / page.htm" % http_port)
            q.wait_for("content ", timeout=30)
            q.type_line("cat page.htm")
            saved = q.wait_for("ThreadOS TCP works", timeout=10)
            check("content from the network written to the TFS and read off the disk",
                  "<html>ThreadOS TCP works</html>" in saved)
            check("only the content went into the file, without the HTTP headers",
                  "Content-Length" not in saved)

            # TFS3: a file well above the old limit of 16 KiB per file
            q.type_line("clear")
            q.type_line("run httpget 10.0.2.2 %d /big big.htm" % http_port)
            big = q.wait_for("content ", timeout=90)
            # httpget gives the length of the body alone (without the headers)
            got = re.search(r"content (\d+) B", big)
            check("a big page fetched whole (%d B of content)"
                  % len(BIG_PAGE),
                  got is not None and int(got.group(1)) == len(BIG_PAGE))
            q.type_line("fls")
            listing = q.wait_for("big.htm", timeout=10)
            check("a file past the old 16 KiB limit written to the TFS",
                  "big.htm  (%d B)" % len(BIG_PAGE) in listing)
            q.type_line("df")
            usage = q.wait_for("disk: ", timeout=10)
            check("df shows the disk usage in blocks",
                  "blocks of 1024 B" in usage and "used" in usage)

            # A file above the old ceiling of 268 KiB: the data reach the
            # doubly indirect block, and then we read them back
            q.type_line("clear")
            q.type_line("run httpget 10.0.2.2 %d /huge huge.htm"
                        % http_port)
            huge = q.wait_for("content ", timeout=180)
            check("a file past 268 KiB fetched whole (%d B)"
                  % len(HUGE_PAGE),
                  "content %d B" % len(HUGE_PAGE) in huge)

            q.type_line("fls")
            listing = q.wait_for("huge.htm", timeout=15)
            check("written to the TFS at its full size",
                  "huge.htm  (%d B)" % len(HUGE_PAGE) in listing)

            q.type_line("run sh")
            q.wait_for("ush:/", timeout=20)
            q.type_line("cat huge.htm | wc")
            back = q.wait_for("bytes=", timeout=180)
            check("reading through a doubly indirect block gives the same data back",
                  "lines=6500 bytes=%d" % len(HUGE_PAGE) in back)
            q.type_line("exit")
            q.wait_for("ush: pa!", timeout=20)

            # HTTP/1.1: a 302 redirect and a body in pieces (chunked) - that
            # is how most real servers answer
            q.type_line("clear")
            q.type_line("run httpget 10.0.2.2 %d /old" % http_port)
            hop = q.wait_for("content ", timeout=30)
            check("the client follows a 302 redirect",
                  "redirect -> /chunks" in hop)
            check("content in chunks was put together correctly",
                  "HTTP 200 (chunked), content %d B" % len(PAGE) in hop)
            check("the content is whole once put together",
                  "ThreadOS TCP works" in hop)
        finally:
            http_stop()

        q.type_line("netstat")
        check("netstat: after the close no connections hang about",
              "no TCP connections" in q.wait_for("connections", timeout=10))
    finally:
        q.close()

    # The large file is to survive a restart with its full size: its inode
    # reaches into an indirect block, and the allocation bitmap comes back
    # from the disk
    print("--- a second boot (the same disk) ---")
    q2 = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE)
    try:
        q2.wait_for("ThreadOS shell", timeout=40)
        q2.type_line("fls")
        listing = q2.wait_for("big.htm", timeout=10)
        check("a big file (>100 KiB) survived the restart at its full size",
              "big.htm  (%d B)" % len(BIG_PAGE) in listing)
        q2.type_line("df")
        usage = q2.wait_for("disk: ", timeout=10)
        check("the disk usage was rebuilt from the bitmap after the restart",
              "used" in usage and "used 0 KiB" not in usage)
    finally:
        q2.close()
        if os.path.exists(disk):
            os.unlink(disk)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
