#!/usr/bin/env python3
"""ProtonOS smoke test: shared memory in the 64-bit kernel.

Until now every process saw only its own memory and that was intended -
that is the point of separate address spaces. Shared memory is an
exception made deliberately: the same physical frames enter several
spaces at once, because both processes asked for the same key.

Each sees them at ITS OWN address - a virtual address is a matter of the
space, not of the memory - and that is exactly what we check first,
because if the addresses had to match, this would be no sharing at all
but a coincidence.

The road goes both ways: the writer writes a pattern, the reader reads
it and writes back into the same place, and the writer sees the answer.
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, make_iso64, no64


def main():
    print("--- pamiec wspoldzielona (jadro 64-bitowe) ---")

    reason = no64()
    if reason:
        print("POMINIETE: " + reason)
        return 0

    d = tempfile.mkdtemp()
    iso = tempfile.mktemp(suffix=".iso")
    slog = tempfile.mktemp(suffix=".log")
    try:
        check("plyta z jadrem 64-bitowym buduje sie", make_iso64(d, iso))
        if not os.path.exists(iso):
            return summary()

        q = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog,
                 qemu="qemu-system-x86_64")
        try:
            q.wait_serial("proton64>", timeout=60)
            out = q.command("run shmwrite64", "koniec, kod wyjscia",
                              timeout=40)

            check("pisarz dostal region pamieci wspoldzielonej",
                  "[shmwrite64] zapisalem 12345" in out)
            check("czytelnik - osobny proces - widzi te sama wartosc",
                  "[shmread64] wartosc=12345" in out)
            check("czytelnik potwierdza, ze to ta sama pamiec",
                  "dwa procesy, jedna pamiec" in out)
            check("odpowiedz czytelnika wraca do pisarza ta sama droga",
                  "czytelnik odpisal przez te sama pamiec" in out)
            check("oba procesy skonczyly sie bez bledu",
                  "koniec, kod wyjscia 0" in out and "ZLE" not in out)

            # The region lies in the mapping area (from 0x50000000
            # upwards), not in the heap or on the stack - and that can be
            # seen from the address
            adresy = re.findall(r"region pod adresem (\d+)", out)
            check("oba procesy dostaly adres z obszaru mapowan",
                  len(adresy) == 2 and
                  all(0x50000000 <= int(a) < 0x60000000 for a in adresy))
            # The writer mapped a region on the side earlier, so it sees
            # the shared one higher up than the reader does. The same
            # bytes at two different addresses - that is the point.
            check("kazdy widzi te sama pamiec pod SWOIM adresem",
                  len(adresy) == 2 and adresy[0] != adresy[1])
        finally:
            q.close()
    finally:
        shutil.rmtree(d, ignore_errors=True)
        for f in (iso, slog):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
