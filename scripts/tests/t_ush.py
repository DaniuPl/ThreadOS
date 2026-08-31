#!/usr/bin/env python3
"""ProtonOS smoke test: ush - the user shell in ring 3.

The kernel's shell is a convenience; ush is a proof. An ordinary process
takes the console over through sys_read, starts other programs through
spawn and wait, joins them with pipes and redirects their output into
files - all through system calls, with no help from outside.

The numbers in the assertions are not decoration. We measure the result
with 'wc', because the content on the screen alone gets confused with
the echo of the typed command: "wierszy=3 bajtow=8" is unambiguous,
while the sight of '2' on the screen is not.

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


def main():
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.write(b"\0" * DISK_SIZE)

    # The demonstration threads' counters - for checking whether the
    # system lives on after something in it died
    def counters(s):
        a = re.findall(r"watek-A: (\d+)", s)
        b = re.findall(r"watek-B: (\d+)", s)
        return (int(a[-1]) if a else None, int(b[-1]) if b else None)

    q = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE)
    try:
        q.wait_for("ProtonOS shell", timeout=40)
        q.wait_for("[siec] DHCP", timeout=30)
        time.sleep(0.5)

        # The user shell: sys_read takes the console over
        q.type_line("run sh")
        q.wait_for("ush - shell uzytkownika")
        view = q.wait_for("ush:/")  # the prompt shows the working directory (root = /)
        check("ush wystartowal w ring 3", "ush:/" in view)

        q.type_line("pid")
        check("ush zna swoj pid (sys_getpid)",
              "moj pid to" in q.wait_for("moj pid to"))

        q.type_line("hello")
        view = q.wait_for("[hello] koncze sie grzecznie")
        check("ush odpalil hello przez spawn+wait",
              "[hello] koncze sie grzecznie" in view)

        q.type_line("nieistnieje")
        check("ush zglasza nieznany program",
              "ush: nie znam 'nieistnieje'" in q.wait_for("ush: nie znam"))

        # Output redirection: 'echo ... > file' (inheriting fd 1)
        q.type_line("echo przekierowanie-ok > redir.txt")
        screen = q.wait_for("wyjscie zapisane do 'redir.txt'")
        # echo should not print the text to the console - it went into the file
        idx = screen.rfind("redir.txt")
        check("echo z '>' nie pisze na konsole (wyjscie do pliku)",
              "przekierowanie-ok" not in screen.split("echo przekierowanie-ok")[-1]
              .split("wyjscie zapisane")[0])
        # ...and 'cat' of that file shows the content written
        q.type_line("cat redir.txt")
        check("przekierowane wyjscie trafilo do pliku (cat redir.txt)",
              "przekierowanie-ok" in q.wait_for("przekierowanie-ok"))

        # A pipe: 'echo proton-pipe | wc' - two processes in parallel, echo
        # writes into the pipe, wc reads to EOF and counts
        # ('proton-pipe\n' = 11 characters + a newline = 12 bytes,
        # 1 line)
        q.type_line("echo proton-pipe | wc")
        screen = q.wait_for("wc: wierszy=")
        check("potok laczy dwa procesy (echo | wc): poprawny licznik",
              "wc: wierszy=1 bajtow=12" in screen)

        # seq as a producer of many lines: 'seq 5 | wc' -> 5 lines,
        # 10 bytes (1\n2\n3\n4\n5\n)
        q.type_line("seq 5 | wc")
        check("seq przez potok do wc: 5 wierszy, 10 bajtow",
              "wc: wierszy=5 bajtow=10" in q.wait_for("wc: wierszy=5"))

        # grep filters in a pipe; we measure the result through a file + wc,
        # because a bare '7' on the screen gets confused with the echo of
        # the typed command.
        # seq 9 | grep 7 -> only '7' (1 line, 2 bytes)
        q.type_line("seq 9 | grep 7 > g.txt")
        time.sleep(0.5)
        q.type_line("cat g.txt | wc")
        check("grep w potoku przepuszcza tylko pasujace wiersze",
              "wc: wierszy=1 bajtow=2" in q.wait_for("wc: wierszy=1 bajtow=2"))

        # head cuts to N lines: seq 20 | head 3 -> 3 lines, 6 bytes
        q.type_line("seq 20 | head 3 > h.txt")
        time.sleep(0.5)
        q.type_line("cat h.txt | wc")
        check("head ogranicza potok do N wierszy",
              "wc: wierszy=3 bajtow=6" in q.wait_for("wc: wierszy=3 bajtow=6"))

        # A 3-stage pipe: seq 20 | grep 1 | wc. The numbers 1..20 with the
        # digit '1' are 1,10..19 = 11 lines; bytes 2 + 10*3 = 32
        q.type_line("seq 20 | grep 1 | wc")
        check("potok 3-stopniowy (seq | grep | wc)",
              "wc: wierszy=11 bajtow=32" in q.wait_for("wc: wierszy=11"))

        # A 4-stage pipe with a redirect: seq 30 | grep 2 | head 3 > p.txt
        # the numbers with '2': 2,12,20,...; the first 3 -> "2\n12\n20\n"
        # = 3 lines/8 B
        q.type_line("seq 30 | grep 2 | head 3 > p.txt")
        time.sleep(0.5)
        q.type_line("cat p.txt | wc")
        check("potok 4-stopniowy z przekierowaniem do pliku",
              "wc: wierszy=3 bajtow=8" in q.wait_for("wc: wierszy=3 bajtow=8"))

        # ls/rm as ring 3 programs. 'echo xyz > usuwany' -> "xyz\n" (4 B);
        # ls shows "usuwany  4 B" (the size does not occur in the typed
        # command, so that is unambiguous proof)
        q.type_line("echo xyz > usuwany")
        time.sleep(0.5)
        q.type_line("ls")
        check("ls (ring 3) listuje plik z rozmiarem",
              "usuwany  4 B" in q.wait_for("usuwany  4 B"))
        # after rm the file disappears - we check that deterministically
        # through a pipe
        q.type_line("rm usuwany")
        time.sleep(0.5)
        q.type_line("ls | grep usuwany | wc")
        check("rm usunal plik (ls juz go nie pokazuje)",
              "wc: wierszy=0 bajtow=0" in q.wait_for("wc: wierszy=0 bajtow=0"))

        # Exit codes: grep with no matches ends with code 1, and the last
        # pipeline stage's code comes back from wait to the shell
        q.type_line("seq 5 | grep 9")
        check("kod wyjscia z potoku (grep bez dopasowan -> kod 1)",
              "ush: [kod 1]" in q.wait_for("ush: [kod 1]"))

        # A process killed by a CPU exception returns code -1
        q.type_line("evil")
        check("kod wyjscia procesu ubitego wyjatkiem (-1)",
              "ush: [kod -1]" in q.wait_for("ush: [kod -1]"))

        # The RTC: 'date' (ring 3) reads the CMOS clock; QEMU has a fixed
        # base. We check only the date (the time will move on during the
        # test), plus the format 'hour:min:sec' (two digits each)
        q.type_line("date")
        screen = q.wait_for(RTC_DATE)
        m = re.search(RTC_DATE + r" (\d\d):(\d\d):(\d\d)", screen)
        check("date (ring 3) czyta RTC: data i format z bazy QEMU",
              m is not None)

        # Appending with '>>': apf = "appendtest\n"(11B) + "zz\n"(3B) =
        # 14B/2 lines. If '>>' truncated like '>', there would be only
        # "zz\n" (3B/1 line).
        q.type_line("echo appendtest > apf")
        time.sleep(0.4)
        q.type_line("echo zz >> apf")
        time.sleep(0.4)
        q.type_line("cat apf | wc")
        check("dopisywanie '>>' nie obcina pliku (14 B, 2 wiersze)",
              "wc: wierszy=2 bajtow=14" in q.wait_for("wc: wierszy=2 bajtow=14"))

        # cp makes an INDEPENDENT copy: appending to the copy (+ "more\n"
        # 5B) gives 19B without touching the original
        q.type_line("cp apf cpf")
        time.sleep(0.4)
        q.type_line("echo more >> cpf")
        time.sleep(0.4)
        q.type_line("cat cpf | wc")
        check("cp tworzy niezalezna kopie pliku (19 B, 3 wiersze)",
              "wc: wierszy=3 bajtow=19" in q.wait_for("wc: wierszy=3 bajtow=19"))

        # mv moves: the source disappears, the target has the content
        q.type_line("mv cpf mvf")
        time.sleep(0.4)
        q.type_line("cat cpf")
        check("mv usunal plik zrodlowy",
              "nie moge otworzyc 'cpf'" in q.wait_for("nie moge otworzyc 'cpf'"))
        q.type_line("echo end >> mvf")
        time.sleep(0.4)
        q.type_line("cat mvf | wc")
        check("mv zachowal tresc w pliku docelowym (23 B, 4 wiersze)",
              "wc: wierszy=4 bajtow=23" in q.wait_for("wc: wierszy=4 bajtow=23"))

        # Directories: mkdir + cd (the prompt shows the working path)
        q.type_line("mkdir d1")
        time.sleep(0.4)
        q.type_line("cd d1")
        check("cd wchodzi do podkatalogu (prompt = sciezka robocza)",
              "ush:/d1>" in q.wait_for("ush:/d1>"))
        # a file created in a subdirectory (relative to cwd); "hejka\n" = 6 B
        q.type_line("echo hejka > f1")
        time.sleep(0.4)
        q.type_line("cd ..")
        time.sleep(0.4)
        # the subdirectory visible in the root directory as 'd1/'
        q.type_line("ls")
        check("ls pokazuje podkatalog ze slashem (d1/)",
              "d1/" in q.wait_for("d1/"))
        # the file from the subdirectory reachable by a relative path; it is
        # not in the root
        q.type_line("cat d1/f1 | wc")
        check("sciezka wzgledna do pliku w podkatalogu (6 B, 1 wiersz)",
              "wc: wierszy=1 bajtow=6" in q.wait_for("wc: wierszy=1 bajtow=6"))

        # The background ('&') + kill: spin runs in the background, kill ends
        # it
        q.type_line("spin &")
        sbg = q.wait_for("w tle, pid=")
        pidm = re.search(r"w tle, pid=(\d+)", sbg)
        check("uruchomienie w tle ('&') zwraca pid", pidm is not None)
        time.sleep(1.0)
        q.type_line("kill " + pidm.group(1))
        check("kill konczy proces w tle (komunikat 'zabity')",
              "zabity (kod -9)" in q.wait_for("zabity (kod -9)"))
        # after being killed spin no longer produces markers
        base = q.screen().count("[spin] tik")
        time.sleep(1.6)
        check("zabity proces przestal dzialac (brak nowych tikow)",
              q.screen().count("[spin] tik") == base)

        q.type_line("exit")
        view = q.wait_for("ush: pa!")
        check("ush wyszedl przez sys_exit", "ush: pa!" in view)

        # The console goes back to the kernel's shell
        before = len(re.findall(r"tyknac timera", q.screen()))
        q.type_line("uptime")
        time.sleep(0.3)
        after = len(re.findall(r"tyknac timera", q.screen()))
        check("konsola wrocila do shella jadra po smierci ush",
              after > before or "tyknac timera" in q.screen() and before == 0)

        # The system has to live on: the counters grow, the shell answers
        a3, _ = counters(q.screen())
        time.sleep(1)
        a4, _ = counters(q.screen())
        check(f"system zyje po ubiciu procesu ({a3} -> {a4})",
              a3 is not None and a4 is not None and a4 > a3)
        q.type_line("uptime")
        check("shell odpowiada po ubiciu procesu",
              "tyknac timera" in q.screen())
    finally:
        q.close()
        if os.path.exists(disk):
            os.unlink(disk)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
