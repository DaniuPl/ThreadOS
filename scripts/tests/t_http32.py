#!/usr/bin/env python3
"""ProtonOS smoke test: TCP and HTTP in the 32-bit kernel.

On the other side stands a real stack - an HTTP server in Python on the
host - and not a loopback of our own. So the client has to reach
agreement with something we did not write: negotiate a connection, read
the response, follow a 302 redirect and assemble a body sent in pieces.

The second thing this is about is the path network -> disk. The files
fetched land on PFS, and their sizes are chosen so as to go through all
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
        q.wait_for("ProtonOS shell", timeout=40)
        q.wait_for("[siec] DHCP", timeout=30)
        time.sleep(0.5)

        http_port, http_stop = start_http_server()
        try:
            q.type_line("clear")
            q.type_line("run httpget 10.0.2.2 %d /" % http_port)
            page = q.wait_for("tresc ", timeout=30)
            check("TCP: handshake i odpowiedz HTTP 200",
                  "[httpget] HTTP 200" in page)
            check("TCP: tresc strony dotarla w calosci",
                  "ProtonOS TCP dziala" in page)
            check("httpget zakonczyl sie bez bledu",
                  "proces 'httpget' zakonczyl sie (kod 0)" in page)

            # Network -> disk: an HTTP body written to PFS and read back from it
            q.type_line("clear")
            q.type_line("run httpget 10.0.2.2 %d / strona.htm" % http_port)
            q.wait_for("tresc ", timeout=30)
            q.type_line("cat strona.htm")
            saved = q.wait_for("ProtonOS TCP dziala", timeout=10)
            check("tresc z sieci zapisana na PFS i odczytana z dysku",
                  "<html>ProtonOS TCP dziala</html>" in saved)
            check("do pliku poszla sama tresc, bez naglowkow HTTP",
                  "Content-Length" not in saved)

            # PFS3: a file well above the old limit of 16 KiB per file
            q.type_line("clear")
            q.type_line("run httpget 10.0.2.2 %d /duza duza.htm" % http_port)
            big = q.wait_for("tresc ", timeout=90)
            # httpget gives the length of the body alone (without the headers)
            got = re.search(r"tresc (\d+) B", big)
            check("duza strona pobrana w calosci (%d B tresci)"
                  % len(BIG_PAGE),
                  got is not None and int(got.group(1)) == len(BIG_PAGE))
            q.type_line("fls")
            listing = q.wait_for("duza.htm", timeout=10)
            check("plik ponad stary limit 16 KiB zapisany na PFS",
                  "duza.htm  (%d B)" % len(BIG_PAGE) in listing)
            q.type_line("df")
            usage = q.wait_for("dysk: ", timeout=10)
            check("df pokazuje zajetosc dysku w blokach",
                  "bloki po 1024 B" in usage and "zajete" in usage)

            # A file above the old ceiling of 268 KiB: the data reach the
            # doubly indirect block, and then we read them back
            q.type_line("clear")
            q.type_line("run httpget 10.0.2.2 %d /ogromna ogromna.htm"
                        % http_port)
            huge = q.wait_for("tresc ", timeout=180)
            check("plik ponad 268 KiB pobrany w calosci (%d B)"
                  % len(HUGE_PAGE),
                  "tresc %d B" % len(HUGE_PAGE) in huge)

            q.type_line("fls")
            listing = q.wait_for("ogromna.htm", timeout=15)
            check("zapisany na PFS z pelnym rozmiarem",
                  "ogromna.htm  (%d B)" % len(HUGE_PAGE) in listing)

            q.type_line("run sh")
            q.wait_for("ush:/", timeout=20)
            q.type_line("cat ogromna.htm | wc")
            back = q.wait_for("bajtow=", timeout=180)
            check("odczyt przez blok podwojnie posredni oddaje te same dane",
                  "wierszy=6500 bajtow=%d" % len(HUGE_PAGE) in back)
            q.type_line("exit")
            q.wait_for("ush: pa!", timeout=20)

            # HTTP/1.1: a 302 redirect and a body in pieces (chunked) - that
            # is how most real servers answer
            q.type_line("clear")
            q.type_line("run httpget 10.0.2.2 %d /stara" % http_port)
            hop = q.wait_for("tresc ", timeout=30)
            check("klient idzie za przekierowaniem 302",
                  "przekierowanie -> /kawalki" in hop)
            check("tresc w kawalkach (chunked) zlozona poprawnie",
                  "HTTP 200 (chunked), tresc %d B" % len(PAGE) in hop)
            check("tresc po zlozeniu jest cala",
                  "ProtonOS TCP dziala" in hop)
        finally:
            http_stop()

        q.type_line("netstat")
        check("netstat: po zamknieciu nie ma wiszacych polaczen",
              "brak polaczen TCP" in q.wait_for("polacze", timeout=10))
    finally:
        q.close()

    # The large file is to survive a restart with its full size: its inode
    # reaches into an indirect block, and the allocation bitmap comes back
    # from the disk
    print("--- drugi boot (ten sam dysk) ---")
    q2 = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE)
    try:
        q2.wait_for("ProtonOS shell", timeout=40)
        q2.type_line("fls")
        listing = q2.wait_for("duza.htm", timeout=10)
        check("duzy plik (>100 KiB) przetrwal restart z pelnym rozmiarem",
              "duza.htm  (%d B)" % len(BIG_PAGE) in listing)
        q2.type_line("df")
        usage = q2.wait_for("dysk: ", timeout=10)
        check("zajetosc dysku odtworzona z bitmapy po restarcie",
              "zajete" in usage and "zajete 0 KiB" not in usage)
    finally:
        q2.close()
        if os.path.exists(disk):
            os.unlink(disk)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
