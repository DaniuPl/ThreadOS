#!/usr/bin/env python3
"""ThreadOS smoke test: shared memory in the 64-bit kernel.

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
    print("--- shared memory (the 64-bit kernel) ---")

    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return 0

    d = tempfile.mkdtemp()
    iso = tempfile.mktemp(suffix=".iso")
    slog = tempfile.mktemp(suffix=".log")
    try:
        check("the disc with the 64-bit kernel builds", make_iso64(d, iso))
        if not os.path.exists(iso):
            return summary()

        q = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog,
                 qemu="qemu-system-x86_64")
        try:
            q.wait_serial("thread64>", timeout=60)
            out = q.command("run shmwrite64", "done, exit code",
                              timeout=40)

            check("the writer got a region of shared memory",
                  "[shmwrite64] I wrote 12345" in out)
            check("the reader - a separate process - sees the same value",
                  "[shmread64] value=12345" in out)
            check("the reader confirms it is the same memory",
                  "two processes, one memory" in out)
            check("the reader's answer comes back to the writer the same way",
                  "the reader answered through the same memory" in out)
            check("both processes finished with no error",
                  "done, exit code 0" in out and "WRONG" not in out)

            # The region lies in the mapping area (from 0x50000000
            # upwards), not in the heap or on the stack - and that can be
            # seen from the address
            addresses = re.findall(r"the region at address (\d+)", out)
            check("both processes got an address from the mapping area",
                  len(addresses) == 2 and
                  all(0x50000000 <= int(a) < 0x60000000 for a in addresses))
            # The writer mapped a region on the side earlier, so it sees
            # the shared one higher up than the reader does. The same
            # bytes at two different addresses - that is the point.
            check("each sees the same memory at ITS OWN address",
                  len(addresses) == 2 and addresses[0] != addresses[1])
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
