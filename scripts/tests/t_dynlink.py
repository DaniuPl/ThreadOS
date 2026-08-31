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


def adresy(text):
    """The three addresses dynlib prints."""
    def one_core(co):
        m = re.search(r"kod programu pod (\d+)" if co == "prog" else
                      (r"kod biblioteki pod (\d+)" if co == "kod" else
                       r"dane biblioteki pod (\d+)"), text)
        return int(m.group(1)) if m else None
    return one_core("prog"), one_core("kod"), one_core("dane")


def check_shared(q, marker, suffix, lib_base):
    """The part common to both kernels: the dynlib program and the initrd
    listing."""
    first = q.command("run dynlib", marker, timeout=60)
    prog, code, dane = adresy(first)
    check("program podaje wszystkie trzy adresy" + suffix,
          prog is not None and code is not None and dane is not None)
    if code is None:
        return

    # The library's code lies in ITS area, not in the program's image.
    # That is the whole point: the program does not have that code inside
    # itself.
    check("kod biblioteki lezy poza obrazem programu" + suffix,
          code >= lib_base and code < lib_base + 16 * 1024 * 1024)
    check("kod programu lezy tam, gdzie zawsze" + suffix,
          prog is not None and 0x40000000 <= prog < 0x41000000)
    check("dane biblioteki sa w osobnym, wyzszym segmencie" + suffix,
          dane is not None and dane > code)
    check("kryptografia z biblioteki liczy poprawnie" + suffix,
          "zgadza sie z FIPS 180-4" in first and "ZLE" not in first)

    counter = re.search(r"licznik po pobraniu bajtow: (\d+)", first)
    check("program doszedl do konca" + suffix,
          "[dynlib] gotowe" in first and counter is not None)

    # The second run: the same code at the same address (shared frames),
    # but the counter from zero again (private data). If the data were
    # shared, it would find the previous process's state.
    second = q.command("run dynlib", marker, timeout=60)
    prog2, code2, dane2 = adresy(second)
    counter2 = re.search(r"licznik po pobraniu bajtow: (\d+)", second)
    check("drugi proces widzi kod pod tym samym adresem" + suffix,
          code2 == code and dane2 == dane)
    check("ale dane biblioteki dostaje wlasne, od zera" + suffix,
          counter is not None and counter2 is not None and
          counter2.group(1) == counter.group(1))

    # A write into the shared code has to kill whoever tries
    corrupt = q.command("run dynlib pisz", "Page Fault", timeout=60)
    check("zapis do kodu biblioteki konczy sie bledem ochrony" + suffix,
          "probuje pisac po kodzie" in corrupt and "Page Fault" in corrupt and
          "zapis do kodu biblioteki sie udal" not in corrupt)

    # Sizes: the four heaviest programs together lighter than the library
    listing = q.command("ls", "dynlib", timeout=20)
    lib = size(listing, "libproton.so")
    check("biblioteka jest w initrd i wazy swoje" + suffix,
          lib is not None and lib > 60000)
    heavy = [size(listing, n)
               for n in ("cryptotest", "certtest", "httpget", "gweb")]
    check("wszystkie cztery programy sa w wypisie" + suffix,
          all(v is not None for v in heavy))
    if lib and all(v is not None for v in heavy):
        check("cztery najciezsze programy razem lzejsze niz biblioteka"
              " (%d B < %d B)%s" % (sum(heavy), lib, suffix),
              sum(heavy) < lib)


def main():
    print("--- biblioteka wspoldzielona (jadro 64-bitowe) ---")

    reason = no64()
    if reason:
        print("POMINIETE: " + reason)
    else:
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
                start = q.wait_serial("proton64>", timeout=90)
                m = re.search(r"Biblioteka libproton\.so: (\d+) stron", start)
                check("jadro wczytalo biblioteke przy rozruchu",
                      m is not None and int(m.group(1)) >= 8)
                check_shared(q, "kod wyjscia", "", 0x4F000000)
            finally:
                q.close()
        finally:
            shutil.rmtree(d, ignore_errors=True)
            for f in (iso, slog):
                if os.path.exists(f):
                    os.unlink(f)

    # --- The same in the 32-bit kernel ---
    print("--- biblioteka wspoldzielona (jadro 32-bitowe) ---")
    slog32 = tempfile.mktemp(suffix=".log")
    disk32 = tempfile.mktemp(suffix=".img")
    with open(disk32, "wb") as fh:
        fh.truncate(DISK_SIZE)
    try:
        q3 = Qemu(KERNEL, INITRD, disk32, append=TEXT_MODE, serial=slog32)
        try:
            start = q3.wait_for("ProtonOS shell", timeout=40)
            m = re.search(r"Biblioteka libproton\.so: (\d+) stron", start)
            check("jadro wczytalo biblioteke przy rozruchu (32-bit)",
                  m is not None and int(m.group(1)) >= 8)
            q3.wait_for("[siec] DHCP", timeout=30)
            check_shared(q3, "zakonczyl sie", " (32-bit)", 0xAF000000)
        finally:
            q3.close()
    finally:
        for f in (slog32, disk32):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
