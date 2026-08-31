#!/usr/bin/env python3
"""ProtonOS smoke test: processes in ring 3 (64-bit kernel).

Only the processor knows the code selector, and its two lowest bits say
which ring the program REALLY ran in - and to say that, it had to enter
the kernel with a syscall instruction.

After that comes everything a process gets and everything it must not
do: a program read from a file (the kernel did not see it at build
time), the NX bit checked by executing its own data, a refusal for
pointers into kernel memory, two processes at the same virtual address
in separate spaces, and a fork with copy on write - with a page bill
that shows only what somebody wrote gets copied.

Run on its own or through scripts/run_tests.py, which starts all the
groups in parallel.
"""

import os
import re
import shutil
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, make_iso64, no64


def main():
    reason = no64()
    if reason:
        print("POMINIETE: " + reason)
        return summary()

    d = tempfile.mkdtemp()
    iso = tempfile.mktemp(suffix=".iso")
    slog = tempfile.mktemp(suffix=".log")
    slog2 = tempfile.mktemp(suffix=".log")
    # A disk behind an AHCI controller: it was this path that required
    # adding the controller's registers to the mapping
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as fh:
        fh.write(b"\0" * DISK_SIZE)

    try:
        check("plyta z jadrem 64-bitowym buduje sie", make_iso64(d, iso))
        if not os.path.exists(iso):
            return summary()

        q23 = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog, disk=disk,
                   sata=True, qemu="qemu-system-x86_64")
        try:
            # The kernel reaches the shell by itself; everything this
            # group checks happens only afterwards
            q23.wait_serial("proton64>", timeout=90)

            # Ring 3: only the processor knows the code selector,
            # and its two lowest bits say which ring the program
            # REALLY ran in. To say that, it had to enter the kernel
            # with a syscall instruction.
            q23.wait_serial("Wywolania systemowe przez instrukcje",
                            timeout=20)
            r3 = q23.wait_serial("Program w ring 3:", timeout=25)
            m = re.search(r"Program w ring 3: (.+?), cs=(\w+), "
                          r"pierscien (\d)", r3)
            check("program wszedl do jadra instrukcja syscall",
                  m is not None and m.group(1) == "odezwal sie")
            check("procesor potwierdza pierscien 3"
                  + (f" (cs={m.group(2)})" if m else ""),
                  m is not None and m.group(3) == "3")

            # A program from a FILE: the kernel did not see it at
            # build time - it learns everything from the ELF header
            ini = q23.wait_serial("initrd:", timeout=25)
            check("jadro 64-bitowe czyta initrd od loadera",
                  re.search(r"initrd: \d+ plik", ini) is not None)
            ld = q23.wait_serial("z initrd:", timeout=25)
            m = re.search(r"Program 'hello64' z initrd: (\d+) B, "
                          r"wejscie ([0-9a-f]+)", ld)
            check("ELF64 z initrd zaladowany"
                  + (f" ({m.group(1)} B, wejscie {m.group(2)})"
                     if m else ""),
                  m is not None and int(m.group(1)) > 0
                  # the entry point has to lie in user space, that
                  # is, from 1 GiB upwards
                  and int(m.group(2), 16) >= 0x40000000)
            out64 = q23.wait_serial("[hello64] koncze", timeout=30)
            check("program z pliku pisze przez wywolanie systemowe",
                  "[hello64] czesc, jestem ELF-em 64-bitowym"
                  in out64)
            check("program liczy i usypia sam siebie",
                  "[hello64] licze: 1" in out64
                  and "[hello64] licze: 3" in out64)
            m = re.search(r"uplynelo tyknac: (\d+)", out64)
            check("sen programu trwa tyle, ile ma"
                  + (f" ({m.group(1)} tyknac)" if m else ""),
                  m is not None and 13 <= int(m.group(1)) <= 25)
            fin = q23.wait_serial("ELF64 z initrd:", timeout=25)
            m = re.search(r"pierscien (\d), kod wyjscia (\d+)", fin)
            check("program z pliku dzialal w ring 3",
                  m is not None and m.group(1) == "3")
            check("kod wyjscia programu dotarl do jadra"
                  + (f" ({m.group(2)})" if m else ""),
                  m is not None and m.group(2) == "7")

            # Memory protection as seen from the program. The NX
            # bit had only been set so far - here a program really
            # tries to execute its data and dies.
            nx = q23.wait_serial("Zakaz wykonywania:", timeout=30)
            check("program probuje wykonac wlasne dane",
                  "[nx64] i probuje ja wykonac" in nx)
            check("procesor odmawia wykonania danych (bit NX)",
                  "procesor odmowil wykonania danych" in nx)
            # 0x15 = page present, access from ring 3, an
            # instruction fetch. The present bit alone would rule out
            # an ordinary missing mapping - this really was NX.
            m = re.search(r"Zakaz wykonywania: .*err=([0-9a-f]+)", nx)
            check("blad strony mowi: pobranie instrukcji z ring 3"
                  + (f" (err={m.group(1)})" if m else ""),
                  m is not None and int(m.group(1), 16) & 0x15 == 0x15)
            check("winny program zostaje zabity, nie caly system",
                  "zabite: Page Fault" in nx)

            # The other road to kernel memory: ask the kernel
            # itself for it. It has to refuse, because it checks the
            # pointer.
            wild = q23.wait_serial("Pamiec jadra:", timeout=30)
            check("jadro odrzuca wskaznik spoza programu",
                  "nie nalezy do programu" in wild
                  and "[wild64] jadro odmowilo" in wild)
            check("pamiec jadra niedostepna z ring 3",
                  "niedostepna dla pierscienia 3" in wild)
            m = re.search(r"Pamiec jadra: .*err=([0-9a-f]+), "
                          r"adres=([0-9a-f]+)", wild)
            check("blad strony mowi: odczyt z ring 3 strony jadra"
                  + (f" (err={m.group(1)}, adres={m.group(2)})"
                     if m else ""),
                  m is not None and int(m.group(1), 16) == 0x05
                  and int(m.group(2), 16) == 0x100000)

            # And what this whole handling exists for
            surv = q23.wait_serial("System przezyl", timeout=30)
            m = re.search(r"licznik (\d+) -> (\d+)", surv)
            check("system pracuje po smierci obu programow"
                  + (f" (licznik {m.group(1)} -> {m.group(2)})"
                     if m else ""),
                  m is not None
                  and int(m.group(2)) > int(m.group(1)))

            # Two processes at once, each in its own space. The
            # same file, the same virtual address, separate memory -
            # and that is exactly what has to be visible.
            sep = q23.wait_serial("Pamiec po procesach", timeout=40)
            addrs = re.findall(r"proces (\d): adres ([0-9a-f]+), "
                               r"wpisalem (\d+)", sep)
            check(f"dwa procesy ruszyly naraz ({len(addrs)})",
                  len(addrs) == 2)
            check("oba pisza pod TEN SAM adres wirtualny"
                  + (f" ({addrs[0][1]})" if addrs else ""),
                  len(addrs) == 2 and addrs[0][1] == addrs[1][1])
            check("kazdy wpisal co innego"
                  + (f" ({addrs[0][2]} i {addrs[1][2]})"
                     if len(addrs) == 2 else ""),
                  len(addrs) == 2 and addrs[0][2] != addrs[1][2])
            kept = re.findall(r"proces \d: nadal (\d+) - moja "
                              r"pamiec nietknieta", sep)
            check(f"zaden nie zobaczyl pamieci drugiego "
                  f"({len(kept)}/2)", len(kept) == 2)
            check("obie kopie skonczyly sie bez bledu",
                  "Dwa procesy naraz: 2 zakonczonych, 0 bledow"
                  in sep)
            # The space has to disappear along with the process,
            # otherwise every program that ran takes memory forever
            m = re.search(r"Pamiec po procesach oddana "
                          r"\((\d+) -> (\d+) ramek\)", sep)
            check("pamiec po procesach wrocila do puli"
                  + (f" ({m.group(1)} -> {m.group(2)})"
                     if m else ""),
                  m is not None
                  and int(m.group(2)) <= int(m.group(1)))

            # fork: the second process comes no longer from the
            # kernel's decision but from the program's own
            fk = q23.wait_serial("Pamiec po fork-u", timeout=45)
            m = re.search(r"rodzic: fork dal numer dziecka (\d+)", fk)
            check("rodzic dostaje z fork-a numer dziecka"
                  + (f" ({m.group(1)})" if m else ""),
                  m is not None and int(m.group(1)) > 0)
            check("dziecko dostaje z fork-a zero",
                  "dziecko: fork dal 0" in fk)
            # Both return to the same place with the same state,
            # but their memory is already separate
            check("dziecko pisze po swojej kopii (200)",
                  "moja wartosc to 200" in fk)
            check("zapis dziecka nie dosiega rodzica (nadal 100)",
                  "rodzic: nadal 100 - dziecko dostalo kopie" in fk)
            check("obie galezie skonczyly sie bez bledu",
                  "Proces powielil sie sam: 2 zakonczonych, "
                  "0 bledow" in fk)
            m = re.search(r"Pamiec po fork-u oddana "
                          r"\((\d+) -> (\d+) ramek\)", fk)
            check("kopia pamieci wrocila do puli"
                  + (f" ({m.group(1)} -> {m.group(2)})"
                     if m else ""),
                  m is not None
                  and int(m.group(2)) <= int(m.group(1)))

            # fork + exec + wait: the classic three that are
            # enough for every shell in the world
            sp = q23.wait_serial("fork + exec + wait:", timeout=60)
            check("proces uruchamia INNY program przez exec",
                  "powielam sie i uruchamiam w dziecku 'hello64'"
                  in sp
                  and "[hello64] czesc, jestem ELF-em" in sp)
            m = re.search(r"doczekalem dziecka (\d+), "
                          r"kod wyjscia (\d+)", sp)
            check("rodzic doczekal wlasnego dziecka"
                  + (f" (nr {m.group(1)})" if m else ""),
                  m is not None and int(m.group(1)) > 0)
            # hello64 ends with a seven - that seven is the proof
            # that it waited for THAT child, not for any finished task
            check("kod wyjscia doszedl do rodzica przez wait"
                  + (f" ({m.group(2)})" if m else ""),
                  m is not None and m.group(2) == "7")
            check("wait bez dzieci mowi, ze nie ma na co czekac",
                  "BLAD: wait wymyslil sobie dziecko" not in sp)
            check("program sam potwierdza cala trojke",
                  "[spawn64] fork + exec + wait dzialaja" in sp)
            check("jadro potwierdza werdykt programu",
                  "fork + exec + wait: 2 zakonczonych, werdykt 0"
                  in sp)

            # And now the bill: fork does NOT copy memory. The
            # program has seven pages (code, constants, data, four
            # stack pages), so copying outright would cost seven of
            # them plus the tables. With shared pages only the tables
            # are left, and only what somebody really wrote gets
            # copied.
            cow = q23.wait_serial("Kopiowanie przy zapisie",
                                  timeout=30)
            m = re.search(r"fork wzial (\d+) ramek, "
                          r"rozdzielono (\d+) stron", cow)
            check("fork nie kopiuje pamieci programu"
                  + (f" ({m.group(1)} ramek - same tablice)"
                     if m else ""),
                  m is not None and int(m.group(1)) <= 6)
            check("rozdzielone tylko strony naprawde zapisane"
                  + (f" ({m.group(2)})" if m else ""),
                  m is not None and 1 <= int(m.group(2)) <= 6)
        finally:
            q23.close()
    finally:
        shutil.rmtree(d, ignore_errors=True)
        for f in (iso, slog, slog2, disk):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
