#!/usr/bin/env python3
"""ProtonOS smoke test: the shared library and symbol binding.

Until now every program carried inside itself everything it used. The
cryptography and TLS meant dozens of kilobytes of the same code in each
of several programs - once in the initrd and once in memory, separately
for every process.

Now the code lies once, at a fixed address, in frames shared by all the
address spaces. Instead of it a program has a table of empty slots,
which the kernel fills in at load time.

We check that with things that cannot be faked:

  1. The code the program executes lies OUTSIDE its image. The address
     has to be supplied by the library itself: a non-relocatable program
     taking the address of its function gets the address of its own jump
     stub.
  2. The result is correct - SHA-256('abc') from FIPS 180-4. A jump
     through an unfilled slot has no way of giving the right answer.
  3. The library's data are PRIVATE. The random generator's counter
     starts from zero in every process; if the data were shared, the
     second run would find the first one's counter.
  4. The library's code cannot be overwritten. The frames are shared, so
     a write into them would swap a function out for everybody at once -
     it is to fail and to kill whoever tries.
  5. The programs really did slim down: the four heaviest together weigh
     less than the library itself.

Both kernels get their library from the same headers - the only
differences are the address and the width of the registers.

Run on its own or through scripts/run_tests.py, which starts all the
groups in parallel.
"""

import os
import re
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, make_iso64, no64


def size(listing, name):
    """A file's size from the 'ls' listing of the initrd."""
    m = re.search(r"\b" + re.escape(name) + r"\s+\((\d+) B\)", listing)
    return int(m.group(1)) if m else None


def addresses(text):
    """The three addresses dynlib prints."""
    def one_core(co):
        m = re.search(r"the program's code at (\d+)" if co == "prog" else
                      (r"the library's code at (\d+)" if co == "code" else
                       r"the library's data at (\d+)"), text)
        return int(m.group(1)) if m else None
    return one_core("prog"), one_core("code"), one_core("data")


def check_shared(q, marker, suffix, lib_base):
    """The part common to both kernels: the dynlib program and the initrd
    listing."""
    first = q.command("run dynlib", marker, timeout=60)
    prog, code, data = addresses(first)
    check("the program gives all three addresses" + suffix,
          prog is not None and code is not None and data is not None)
    if code is None:
        return

    # The library's code lies in ITS area, not in the program's image.
    # That is the whole point: the program does not have that code inside
    # itself.
    check("the library's code lies outside the program image" + suffix,
          code >= lib_base and code < lib_base + 16 * 1024 * 1024)
    check("the program's code lies where it always does" + suffix,
          prog is not None and 0x40000000 <= prog < 0x41000000)
    check("the library's data is in a separate, higher segment" + suffix,
          data is not None and data > code)
    check("the cryptography from the library computes correctly" + suffix,
          "matches FIPS 180-4" in first and "WRONG" not in first)

    counter = re.search(r"the counter after taking bytes: (\d+)", first)
    check("the program reached the end" + suffix,
          "[dynlib] done" in first and counter is not None)

    # The second run: the same code at the same address (shared frames),
    # but the counter from zero again (private data). If the data were
    # shared, it would find the previous process's state.
    second = q.command("run dynlib", marker, timeout=60)
    prog2, code2, data2 = addresses(second)
    counter2 = re.search(r"the counter after taking bytes: (\d+)", second)
    check("the second process sees the code at the same address" + suffix,
          code2 == code and data2 == data)
    check("but it gets the library's data of its own, from zero" + suffix,
          counter is not None and counter2 is not None and
          counter2.group(1) == counter.group(1))

    # A write into the shared code has to kill whoever tries
    corrupt = q.command("run dynlib write", "Page Fault", timeout=60)
    check("writing to the library's code ends with a protection fault" + suffix,
          "trying to write over the library's code" in corrupt and "Page Fault" in corrupt and
          "writing to the library's code worked" not in corrupt)

    # Sizes: the four heaviest programs together lighter than the library
    listing = q.command("ls", "dynlib", timeout=20)
    lib = size(listing, "libproton.so")
    check("the library is in the initrd and weighs what it weighs" + suffix,
          lib is not None and lib > 60000)
    heavy = [size(listing, n)
               for n in ("cryptotest", "certtest", "httpget", "gweb")]
    check("all four programs are in the listing" + suffix,
          all(v is not None for v in heavy))
    if lib and all(v is not None for v in heavy):
        check("the four heaviest programs together weigh less than the library"
              " (%d B < %d B)%s" % (sum(heavy), lib, suffix),
              sum(heavy) < lib)


def main():
    print("--- the shared library (the 64-bit kernel) ---")

    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
    else:
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
                start = q.wait_serial("proton64>", timeout=90)
                m = re.search(r"Library libproton\.so: (\d+) pages", start)
                check("the kernel read the library at boot",
                      m is not None and int(m.group(1)) >= 8)
                check_shared(q, "exit code", "", 0x4F000000)
            finally:
                q.close()
        finally:
            shutil.rmtree(d, ignore_errors=True)
            for f in (iso, slog):
                if os.path.exists(f):
                    os.unlink(f)

    # --- The same in the 32-bit kernel ---
    print("--- the shared library (the 32-bit kernel) ---")
    slog32 = tempfile.mktemp(suffix=".log")
    disk32 = tempfile.mktemp(suffix=".img")
    with open(disk32, "wb") as fh:
        fh.truncate(DISK_SIZE)
    try:
        q3 = Qemu(KERNEL, INITRD, disk32, append=TEXT_MODE, serial=slog32)
        try:
            start = q3.wait_for("ProtonOS shell", timeout=40)
            m = re.search(r"Library libproton\.so: (\d+) pages", start)
            check("the kernel read the library at boot (32-bit)",
                  m is not None and int(m.group(1)) >= 8)
            q3.wait_for("[net] DHCP", timeout=30)
            check_shared(q3, "has ended", " (32-bit)", 0xAF000000)
        finally:
            q3.close()
    finally:
        for f in (slog32, disk32):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
