#!/usr/bin/env python3
"""ProtonOS smoke test: who is at the machine, and what that lets them do.

Everything that ran could do everything. A program in ring 3 could
overwrite the file the system reads its settings from at boot, delete
the swap file, or change the machine's name - and so could anybody who
typed a command. There was nobody to ask, because there was nobody.

There are two now: an admin, who may change the system, and a plain
user, who may do everything else. Two levels, no groups, no owners per
file - enough to say the thing worth saying, that changing the system
is a different act from using it.

What is checked here is the whole road, not the flag:

  the refusal is REAL - after it, the setting still has its old value,
  and the system file is still there
  the password is the only way up, a wrong one does not open anything,
  and three wrong ones in a row shut the door for a while
  the right lasts for ONE command: after 'sudo set ...' the next
  command is a plain user's again
  a program in ring 3 is refused by the KERNEL, not by the program: rm
  and cp are told 'not allowed', which is a different answer from
  'there is no such file'
  the new password survives a restart, and the file on the disk holds
  a number computed from it rather than the password itself

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
from ptest import check, summary

DISK_MB = 16


def new_disk():
    path = tempfile.mktemp(suffix=".img")
    with open(path, "wb") as f:
        f.truncate(DISK_MB * 1024 * 1024)
    return path


def sudo(q, command, password, wait, timeout=30):
    """'sudo <command>', then the password on the line the shell asks
    for it on. The password never appears on the screen, so we wait for
    what comes after it."""
    q.type_line("sudo " + command)
    time.sleep(1)
    return q.command(password, wait, timeout=timeout)


def main():
    disk = new_disk()
    slog = tempfile.mktemp(suffix=".log")

    print("--- zwykly uzytkownik ---")
    try:
        q = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE, serial=slog)
        try:
            q.wait_for("ProtonOS shell", timeout=40)

            out = q.command("kim", "uzytkownik", timeout=30)
            check("maszyna wstaje jako zwykly uzytkownik",
                  "uzytkownik" in out)

            refused = q.command("set nazwa maszyna", "set:", timeout=30)
            check("zmiana ustawienia odrzucona",
                  "wymaga admina" in refused)
            # A refusal that changed the setting anyway would be worse
            # than no refusal at all
            listing = q.command("set", "uzycie:", timeout=30)
            check("i ustawienie NAPRAWDE sie nie zmienilo",
                  re.search(r"nazwa = proton\b", listing) is not None)

            # The console must not be the way round the rule the rest of
            # the machine keeps
            wr = q.command("write system.cfg cokolwiek", "uprawnien",
                           timeout=30)
            check("zapis do pliku systemowego z konsoli odrzucony",
                  "brak uprawnien" in wr)

            # ...and neither must a program in ring 3: this refusal
            # comes from the kernel, through the syscall
            rm = q.command("run rm swap.sys", "zakonczyl", timeout=60)
            check("program w pierscieniu trzecim tez dostaje odmowe",
                  "brak uprawnien" in rm)
            check("i jest to odmowa, a nie 'nie ma pliku'",
                  "nie ma pliku" not in rm)
            fls = q.command("fls", "swap.sys", timeout=30)
            check("plik systemowy dalej jest na dysku", "swap.sys" in fls)

            # --- the password ---------------------------------------
            q.type_line("sudo set nazwa maszyna")
            time.sleep(1)
            bad = q.command("nieprawidlowe", "sudo:", timeout=30)
            check("zle haslo nie otwiera niczego", "zle haslo" in bad)
            listing = q.command("set", "uzycie:", timeout=30)
            check("i po zlym hasle ustawienie nadal stare",
                  re.search(r"nazwa = proton\b", listing) is not None)

            out = sudo(q, "set nazwa maszyna", "proton", "zapisane")
            check("dobre haslo wykonuje komende jako admin",
                  "nazwa = maszyna (zapisane)" in out)

            # One command, not a session
            out = q.command("kim", "uzytkownik", timeout=30)
            check("po komendzie znowu jestesmy zwyklym uzytkownikiem",
                  "uzytkownik" in out)

            # --- an admin session, and the way out of it -------------
            q.type_line("sudo")
            time.sleep(1)
            out = q.command("proton", "adminem", timeout=30)
            check("'sudo' bez komendy daje sesje admina",
                  "jestes adminem" in out)
            out = q.command("kim", "admin", timeout=30)
            check("i 'kim' to potwierdza", "admin (" in out)
            out = q.command("set kolor 14", "zapisane", timeout=30)
            check("w sesji admina ustawienia zmieniaja sie bez pytania",
                  "kolor = 14 (zapisane)" in out)
            out = q.command("wyloguj", "uzytkownik", timeout=30)
            check("'wyloguj' konczy sesje", "zwykly uzytkownik" in out)
            out = q.command("set kolor 9", "set:", timeout=30)
            check("i po wylogowaniu znowu nie wolno",
                  "wymaga admina" in out)

            # --- three wrong answers in a row ------------------------
            # A six-character password is an afternoon's work for a
            # program that may ask as fast as it likes
            for _ in range(3):
                q.type_line("sudo set kolor 9")
                time.sleep(0.8)
                q.command("zlehaslo", "sudo:", timeout=30)
            q.type_line("sudo set kolor 9")
            time.sleep(0.8)
            out = q.command("proton", "sudo:", timeout=30)
            check("po trzech zlych probach nawet dobre haslo nie dziala",
                  "zle haslo" in out)
            time.sleep(7)               # the lock is about five seconds
            out = sudo(q, "set kolor 9", "proton", "zapisane", timeout=40)
            check("po chwili dobre haslo znowu dziala",
                  "kolor = 9 (zapisane)" in out)

            # --- changing the password -------------------------------
            q.type_line("sudo haslo")
            time.sleep(1)
            q.type_line("proton")       # for sudo itself
            time.sleep(1)
            q.type_line("nowehaslo")    # the new one
            time.sleep(0.8)
            out = q.command("nowehaslo", "haslo", timeout=30)
            check("haslo zmienione", "haslo zmienione" in out)

            # The file holds a number, not the password
            f = q.command("cat system.cfg", "haslo=", timeout=30)
            check("plik na dysku nie zawiera hasla otwartym tekstem",
                  "nowehaslo" not in f)
            check("tylko liczbe z niego policzona",
                  re.search(r"haslo=\d{6,}", f) is not None)
        finally:
            q.close()

        print("--- po restarcie na tym samym dysku ---")
        slog2 = tempfile.mktemp(suffix=".log")
        q2 = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE, serial=slog2)
        try:
            q2.wait_for("ProtonOS shell", timeout=40)
            q2.type_line("sudo set kolor 12")
            time.sleep(1)
            out = q2.command("proton", "sudo:", timeout=30)
            check("stare haslo po restarcie juz nie dziala",
                  "zle haslo" in out)
            time.sleep(1)
            out = sudo(q2, "set kolor 12", "nowehaslo", "zapisane",
                       timeout=40)
            check("nowe haslo przezylo restart",
                  "kolor = 12 (zapisane)" in out)
        finally:
            q2.close()
            if os.path.exists(slog2):
                os.unlink(slog2)
    finally:
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


sys.exit(main())
