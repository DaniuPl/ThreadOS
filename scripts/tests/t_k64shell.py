#!/usr/bin/env python3
"""ProtonOS smoke test: the shell, signals and SSE (64-bit kernel).

A system one can talk to rather than merely watch replay a script: a
program started FROM THE KEYBOARD, a signal stopping an endless loop
from the outside, a handler of one's own returning exactly where it was
interrupted, and a user shell joining programs with pipes.

At the end floating-point numbers. On x86-64 SSE is part of the calling
convention, so the compiler uses it without asking - and the XMM
registers are as much a task's state as the rest, and have to survive a
context switch.

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

            # The keyboard and the shell: a system one can talk
            # to rather than merely watch replay a script
            ready = q23.wait_serial("proton64>", timeout=40)
            check("jadro 64-bitowe uruchamia shella",
                  "Klawiatura PS/2 (IRQ 1)" in ready
                  and "System 64-bitowy gotowy" in ready)

            q23.type_line("mem")
            mem = q23.wait_serial("Pamiec:", timeout=20)
            check("shell odpowiada na 'mem'",
                  re.search(r"Pamiec: \d+ KiB RAM, ramki "
                            r"\d+/\d+ zajete", mem) is not None)
            q23.type_line("ls")
            lsout = q23.wait_serial("hello64  (", timeout=20)
            check("shell wypisuje programy z initrd",
                  "spawn64" in lsout and "sep64" in lsout)
            q23.type_line("ps")
            ps = q23.wait_serial("shell (biezacy)", timeout=20)
            check("shell widzi sam siebie na liscie zadan",
                  "bezczynny" in ps)

            # A program started FROM THE KEYBOARD, not from a script
            q23.type_line("run hello64")
            run = q23.wait_serial("koniec, kod wyjscia", timeout=40)
            check("program uruchomiony z klawiatury dziala",
                  "[hello64] czesc, jestem ELF-em" in run)
            check("shell doczekal programu i podal kod wyjscia",
                  "[hello64] koniec, kod wyjscia 7" in run)
            q23.type_line("nieznane")
            bad = q23.wait_serial("Nieznane polecenie", timeout=20)
            check("shell nie udaje, ze zna kazde polecenie",
                  "Nieznane polecenie: nieznane" in bad)

            # Signals: a program in an endless loop has to be
            # stoppable from the outside, because from the inside it
            # cannot be
            q23.type_line("run spin64")
            q23.wait_serial("[spin64] krece sie", timeout=30)
            time.sleep(1)
            q23.sendkey("ctrl-c")
            kill = q23.wait_serial("[spin64] koniec", timeout=30)
            check("Ctrl+C wysyla sygnal do programu",
                  re.search(r"zadanie \d+ dostalo sygnal 2", kill)
                  is not None)
            check("program bez wlasnej procedury ginie od sygnalu",
                  "(spin64) zabite: sygnal" in kill)
            check("shell odzyskuje konsole po zabitym programie",
                  "[spin64] koniec, kod wyjscia -1" in kill)

            # And a program with a handler of its own is to return
            # after it to where it was interrupted - that is the whole
            # difference between a signal and a kill
            q23.type_line("run sigtest64")
            q23.wait_serial("[sigtest64] czekam", timeout=30)
            time.sleep(1)
            q23.sendkey("ctrl-c")
            sig = q23.wait_serial("[sigtest64] koniec", timeout=30)
            check("program przechwytuje sygnal wlasna procedura",
                  "procedura obslugi: dostalem sygnal" in sig)
            check("po procedurze program wraca w przerwane miejsce",
                  "wrocilem do przerwanego miejsca" in sig)
            check("przechwycony sygnal nie zabija programu",
                  "[sigtest64] koniec, kod wyjscia 0" in sig)
            check("jadro nie zglasza smierci przechwyconego sygnalu",
                  "(sigtest64) zabite" not in sig)

            # Only what arrived AFTER this command - earlier in
            # the log both programs are there, of course
            seen = len(q23.serial())
            q23.type_line("ps")
            after = q23.wait_serial("shell (biezacy)",
                                    timeout=20)[seen:]
            check("po obu programach nie zostaje zadne zadanie",
                  "spin64" not in after
                  and "sigtest64" not in after
                  and "bezczynny" in after)

            # A shell in USER SPACE: an ordinary process with no
            # rights beyond those every other one has - it reads the
            # keyboard with a system call, and starts programs
            # through fork and exec
            q23.type_line("run ush64")
            u = q23.wait_serial("ush64$", timeout=40)
            check("shell uzytkownika startuje jako proces ring 3",
                  "ush64 - shell uzytkownika" in u)

            # The word has to appear TWICE: once as the echo of
            # the typing, once as the program's answer. We count the
            # occurrences instead of looking for the end of a line -
            # the serial port ends them with the sequence "\n\r", and
            # tying oneself to such a detail is asking for trouble.
            q23.type_line("echo abrakadabra")
            limit = time.time() + 25
            while (time.time() < limit
                   and q23.serial().count("abrakadabra") < 2):
                time.sleep(0.3)
            ec = q23.serial()
            check("program czyta klawiature wywolaniem systemowym"
                  f" ({ec.count('abrakadabra')} wystapien)",
                  ec.count("abrakadabra") >= 2)

            seen = len(q23.serial())
            q23.type_line("uecho64 z shella uzytkownika")
            ua = q23.wait_serial("ush64: koniec, kod 0",
                                 timeout=40)[seen:]
            check("shell uzytkownika przekazuje argumenty do exec",
                  "z shella uzytkownika" in ua)

            # Pipes: one program's output enters another's
            # input, and neither of them knows about it. uupper64
            # reads from number zero and writes to number one - what
            # is on the other side is the business of whoever started
            # it.
            seen = len(q23.serial())
            q23.type_line("uecho64 proton przez potok | uupper64")
            pp = q23.wait_serial("PROTON PRZEZ POTOK",
                                 timeout=40)[seen:]
            check("wyjscie programu wchodzi na wejscie drugiego",
                  "PROTON PRZEZ POTOK" in pp)

            # The second program has to see the END of the data -
            # and that happens only when the shell closes both ends of
            # the pipe at its own end
            seen = len(q23.serial())
            q23.type_line("uls64 | uupper64")
            pl = q23.wait_serial("ULS64: WPISOW", timeout=40)[seen:]
            check("czytajacy widzi koniec danych i konczy sie sam",
                  "ULS64: WPISOW" in pl)

            seen = len(q23.serial())
            q23.type_line("uecho64 sam |")
            pe = q23.wait_serial("po kresce nic nie ma",
                                 timeout=30)[seen:]
            check("pusta strona potoku nie wywraca shella",
                  "ush64: po kresce nic nie ma" in pe)

            q23.type_line("hello64")
            hi = q23.wait_serial("ush64: koniec, kod 7", timeout=45)
            check("shell uzytkownika uruchamia program (fork+exec)",
                  "[hello64] czesc, jestem ELF-em" in hi)
            check("i odbiera jego kod wyjscia przez wait",
                  "ush64: koniec, kod 7" in hi)

            # A failed exec must not kill the shell - the child
            # ends by itself, the parent lives on
            q23.type_line("nieistniejacy")
            ne = q23.wait_serial("ush64: koniec, kod 127",
                                 timeout=30)
            check("nieudany exec konczy tylko dziecko",
                  "nie ma programu 'nieistniejacy'" in ne
                  and "ush64: koniec, kod 127" in ne)

            # Ctrl+C in the user shell. The signal has to reach
            # the PROGRAM that is running, not the shell waiting for
            # it - and it has to reach a shell waiting inside the
            # kernel, because otherwise it waits there forever.
            q23.type_line("spin64")
            q23.wait_serial("[spin64] krece sie", timeout=30)
            time.sleep(1.5)
            q23.sendkey("ctrl-c")
            ci = q23.wait_serial("ush64: koniec, kod -1", timeout=30)
            check("Ctrl+C trafia w program, nie w czekajacy shell",
                  "(spin64) zabite: sygnal" in ci)
            check("kod wyjscia zabitego programu jest ujemny",
                  "ush64: koniec, kod -1" in ci)
            # And the shell itself is to survive it - otherwise
            # every accidental Ctrl+C would end the session
            seen = len(q23.serial())
            q23.type_line("echo poctrlc")
            limit = time.time() + 25
            while (time.time() < limit
                   and q23.serial()[seen:].count("poctrlc") < 2):
                time.sleep(0.3)
            check("shell uzytkownika przezyl Ctrl+C i czyta dalej",
                  q23.serial()[seen:].count("poctrlc") >= 2)

            q23.type_line("exit")
            ex = q23.wait_serial("[ush64] koniec, kod wyjscia 0",
                                 timeout=30)
            check("po wyjsciu shella uzytkownika wraca shell jadra",
                  "ush64: pa!" in ex)

            # Floating-point numbers. On x86-64 SSE is part of
            # the calling convention, so the compiler uses it without
            # asking - and the XMM registers are as much a task's
            # state as the rest.
            sse = q23.wait_serial("Stan SSE przetrwal", timeout=40)
            check("jadro wlacza jednostke zmiennoprzecinkowa",
                  "Jednostka zmiennoprzecinkowa i SSE wlaczone"
                  in sse)
            results = re.findall(r"\[fpu64\] start (\d+): wyszlo "
                                r"(\d+), mialo (\d+) - (\S+)", sse)
            check(f"dwa procesy licza rownoczesnie "
                  f"({len(results)})", len(results) == 2)
            # Each is to get ITS OWN result - if the SSE state
            # were not saved, it would get the other one's
            check("kazdy dostaje wlasny wynik, nie cudzy",
                  len(results) == 2
                  and results[0][1] != results[1][1]
                  and all(w[1] == w[2] for w in results))
            check("jadro potwierdza brak pomylek",
                  "2 policzylo, 0 pomylek" in sse)
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
