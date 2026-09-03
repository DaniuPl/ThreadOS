#!/usr/bin/env python3
"""ProtonOS smoke test: the text editor in a window.

The system could make files - 'write' at the prompt, 'echo' with a
'>' - but it could not CHANGE one. Everything that made a file was one
line typed at a prompt, and a line typed at a prompt cannot go back to
what it said a moment ago. That is the difference between a machine
that stores text and a machine somebody can work on.

So there is a page of white with black text on it, a caret that goes
where the arrows and the mouse say, Enter that splits a line and
Backspace that joins two, Ctrl+S to save and Ctrl+O to open. The
status line at the bottom is the whole of the interface: the file's
name, whether it has changed since it was saved, where the caret is -
and, when the editor needs a name, the question, asked in that same
line rather than in a second window to move, focus and close.

The assertions read the screen for the text and the status line, and
then step outside the editor entirely: the kernel's own shell types
'cat' at the file, so what is on the disk is checked by something that
was not the program that wrote it. Then the editor reads it back, the
clipboard carries a line from one place in the file to another, and
Backspace at the start of a line joins it to the one above - the only
way back from an Enter pressed by mistake.

Run on its own or through scripts/run_tests.py, which starts all the
groups in parallel.
"""

import os
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, font8x8

# The window: slot 2 of the cascade, content a pixel right and a title
# bar down. The editor asks for 420x260 and pads its page by four.
WX, WY = 94, 54
CX, CY = WX + 1, WY + 11
PAD = 4
ROW_H = 10
STATUS_H = 10
CW, CH = 420, 260

# The terminal opens in the next slot of the cascade, behind the editor
TWX, TWY = 106, 66


def term_rows(at, rows=20, cols=48):
    """The terminal's window: light letters on its black screen."""
    font = font8x8()
    ox, oy = TWX + 1 + 4, TWY + 11 + 4
    out = []
    for r in range(rows):
        line = ""
        for c in range(cols):
            pattern = []
            for y in range(8):
                bits = 0
                for x in range(8):
                    p = at(ox + c * 8 + x, oy + r * 10 + y)
                    if p[0] > 120 or p[1] > 120 or p[2] > 120:
                        bits |= 1 << x
                pattern.append(bits)
            line += font.get(tuple(pattern), " " if not any(pattern) else "?")
        out.append(line.rstrip())
    return out


TB_H = 18
TBL_X0 = 46
CLOCK_W = 8 * 8 + 6


def buttons(at, w, y):
    """The window buttons on the taskbar: everything on that row which
    is not the bar's own grey."""
    out, start = [], None
    for x in range(TBL_X0, w - CLOCK_W - 2):
        on_bar = all(abs(c - 210) <= 6 for c in at(x, y))
        if not on_bar and start is None:
            start = x
        elif on_bar and start is not None:
            out.append((start, x - 1))
            start = None
    if start is not None:
        out.append((start, w - CLOCK_W - 3))
    return out


def is_page(px):
    return all(c > 235 for c in px)          # the white of the page


def is_bar(px):
    r, g, b = px                             # the blue of the status line
    return b > 120 and r < 90 and g < 90


# The file manager: names at content (6,18), a row every ten pixels,
# and the two buttons held against the right-hand edge of a 320-wide
# window ("Otworz" at 212, "Usun" at 272).
FM_NAME_X = 6
FM_ROW_Y0 = 18
FM_OPEN_X = 320 - 48 - 56 - 4


def file_row(at, i, cols=12, wx=WX, wy=WY):
    """One name from the file manager's list: light letters on the
    window's grey."""
    font = font8x8()
    x0, y0 = wx + 1 + FM_NAME_X, wy + 11 + FM_ROW_Y0 + i * 10
    out = ""
    for c in range(cols):
        pattern = []
        for y in range(8):
            bits = 0
            for x in range(8):
                p = at(x0 + c * 8 + x, y0 + y)
                if not all(abs(v - 227) <= 8 for v in p):
                    bits |= 1 << x
            pattern.append(bits)
        out += font.get(tuple(pattern), " " if not any(pattern) else "?")
    return out.strip()


def is_ink(px):
    """Black on the page. The caret is blue and stands between two
    characters, so it is not part of any of them."""
    return all(c < 120 for c in px)


def row_text(at, r, cols=48, wx=WX, wy=WY):
    """One line of the page, read through the kernel's own font."""
    font = font8x8()
    x0, y0 = wx + 1 + PAD, wy + 11 + PAD + r * ROW_H
    out = ""
    for c in range(cols):
        pattern = []
        for y in range(8):
            bits = 0
            for x in range(8):
                if is_ink(at(x0 + c * 8 + x, y0 + y)):
                    bits |= 1 << x
            pattern.append(bits)
        out += font.get(tuple(pattern), " " if not any(pattern) else "?")
    return out.rstrip()


def win_title(at, wx=WX, wy=WY, cols=24):
    """The window's own name, white on its title bar."""
    font = font8x8()
    out = ""
    for c in range(cols):
        pattern = []
        for y in range(8):
            bits = 0
            for x in range(8):
                if all(v > 200 for v in at(wx + 4 + c * 8 + x, wy + 2 + y)):
                    bits |= 1 << x
            pattern.append(bits)
        out += font.get(tuple(pattern), " " if not any(pattern) else "?")
    return out.strip()


def status_text(at, cols=48):
    """The status line: white letters on the accent blue."""
    font = font8x8()
    x0, y0 = CX + PAD, CY + CH - STATUS_H + 1
    out = ""
    for c in range(cols):
        pattern = []
        for y in range(8):
            bits = 0
            for x in range(8):
                if not is_bar(at(x0 + c * 8 + x, y0 + y)):
                    bits |= 1 << x
            pattern.append(bits)
        out += font.get(tuple(pattern), " " if not any(pattern) else "?")
    return out.rstrip()


def sixty_four():
    """The same editor on the 64-bit kernel. It is one source compiled
    twice, so what is asked here is the part that is not: that the
    calls it makes - a window, keys, and a file written to the disk -
    are all there in long mode too."""
    if KERNEL64 is None or not os.path.exists(KERNEL64):
        print("POMINIETE (64): brak obrazu 64-bitowego (make kernel64)")
        return
    if shutil.which("grub-mkrescue") is None or \
       shutil.which("qemu-system-x86_64") is None:
        print("POMINIETE (64): brak grub-mkrescue lub qemu-system-x86_64")
        return

    d = tempfile.mkdtemp()
    iso = tempfile.mktemp(suffix=".iso")
    slog = tempfile.mktemp(suffix=".log")
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.truncate(16 * 1024 * 1024)

    try:
        os.makedirs(d + "/boot/grub")
        shutil.copy(KERNEL64, d + "/boot/protonos64.bin")
        mod = ""
        if INITRD64 and os.path.exists(INITRD64):
            shutil.copy(INITRD64, d + "/boot/initrd64.img")
            mod = "    module2 /boot/initrd64.img initrd64\n"
        with open(d + "/boot/grub/grub.cfg", "w") as fh:
            fh.write("insmod all_video\nset gfxpayload=keep\n"
                     "set timeout=0\nset default=0\n"
                     'menuentry "ProtonOS 64" {\n'
                     "    multiboot2 /boot/protonos64.bin\n" + mod + "}\n")
        made = subprocess.run(["grub-mkrescue", "-o", iso, d],
                              stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL).returncode == 0
        check("plyta z jadrem 64-bitowym buduje sie", made)
        if not made:
            return

        q = Qemu(KERNEL, INITRD, disk=disk, cdrom=iso, serial=slog,
                 qemu="qemu-system-x86_64")
        try:
            q.wait_serial("proton64>", timeout=60)
            q.type_line("pulpit")
            q.wait_serial("Pulpit startuje", timeout=30)
            time.sleep(4)
            q.mouse_home()

            q.mouse_goto(24, 312)              # the "Edytor" icon
            q.mouse_click()
            time.sleep(4)
            q.mouse_goto(CX + 200, CY + 100)
            q.mouse_click()                    # the focus, into the page
            time.sleep(1)

            q.type_line("dwa jadra", enter=False)
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            check("(64) edytor pisze na kartce ('%s')" % row_text(at, 0),
                  row_text(at, 0) == "dwa jadra")

            # The same refusal, through the long-mode syscall of its own
            q.sendkey("alt-f4")
            time.sleep(3)
            w, h, at = q.screen_raw()
            check("(64) Alt+F4 przy niezapisanym pliku pyta, a nie zamyka "
                  "('%s')" % status_text(at),
                  "Niezapisane zmiany" in status_text(at) and
                  row_text(at, 0) == "dwa jadra")

            q.sendkey("ctrl-s")
            time.sleep(1)
            q.type_line("long", enter=False)
            time.sleep(0.5)
            q.sendkey("ret")
            time.sleep(2)
            w, h, at = q.screen_raw()
            check("(64) i zapisuje pod nazwa ('%s')" % status_text(at),
                  "long" in status_text(at) and "long*" not in status_text(at))

            # Checked by a program that is not the one that wrote the
            # file: the terminal, running the 64-bit 'cat'. In long
            # mode the kernel's own shell cannot be typed at from the
            # desktop - there is no console behind it - so the check
            # goes through the terminal window, which is a fuller
            # answer anyway: one application wrote the file and
            # another read it.
            q.mouse_goto(24, 192)              # the "Term" icon
            q.mouse_click()
            time.sleep(5)
            q.mouse_goto(TWX + 200, TWY + 100)
            q.mouse_click()
            time.sleep(1)
            q.type_line("ucat64 long")
            time.sleep(4)
            q.mouse_goto(600, 430)
            time.sleep(1)
            w, h, at = q.screen_raw()
            rows = term_rows(at)
            check("(64) inny program czyta z dysku to, co edytor zapisal",
                  any("dwa jadra" in r for r in rows))

            # And the file manager hands a file to the editor here too -
            # in long mode that is a fork and an exec from a program
            # drawing in a window, which is its own piece of code
            fx, fy = 118, 78              # the next slot of the cascade
            q.mouse_goto(24, 112)         # the "Pliki" icon
            q.mouse_click()
            time.sleep(5)
            q.mouse_goto(600, 430)
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            listed = [file_row(at, i, wx=fx, wy=fy) for i in range(6)]
            row = -1
            for i, name in enumerate(listed):
                if name == "long":
                    row = i
            check("(64) menedzer plikow widzi plik z edytora (%s)"
                  % ", ".join(n for n in listed if n), row >= 0)
            if row >= 0:
                q.mouse_goto(fx + 1 + 20, fy + 11 + FM_ROW_Y0 + row * 10 + 3)
                time.sleep(0.5)
                q.mouse_click()
                time.sleep(1.5)
                q.mouse_goto(fx + 1 + FM_OPEN_X + 20, fy + 11 + 5)  # "Otworz"
                time.sleep(0.5)
                q.mouse_click()
                time.sleep(5)
                q.mouse_goto(600, 430)
                time.sleep(1.5)
                w, h, at = q.screen_raw()
                check("(64) 'Otworz' otwiera plik w edytorze ('%s')"
                      % row_text(at, 0, wx=fx + 12, wy=fy + 12),
                      row_text(at, 0, wx=fx + 12, wy=fy + 12) == "dwa jadra")
        finally:
            q.close()
    finally:
        shutil.rmtree(d, ignore_errors=True)
        for f in (iso, slog, disk):
            if os.path.exists(f):
                os.unlink(f)


def main():
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.truncate(16 * 1024 * 1024)
    slog = tempfile.mktemp(suffix=".log")

    try:
        q = Qemu(KERNEL, INITRD, disk, serial=slog)
        try:
            check("pulpit wstal", q.wait_desktop(timeout=40))
            q.mouse_home()

            q.mouse_goto(24, 312)              # the "Edytor" icon
            q.mouse_click()
            time.sleep(4)
            q.mouse_goto(600, 430)
            time.sleep(1.5)

            w, h, at = q.screen_raw()
            check("edytor otworzyl sie i jest bialy jak kartka",
                  is_page(at(CX + 200, CY + 100)))
            check("a na dole ma pasek stanu",
                  is_bar(at(CX + 200, CY + CH - 5)))

            # --- typing -------------------------------------------------
            q.type_line("Ala ma kota", enter=False)
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            check("to, co wpisano, jest na kartce ('%s')" % row_text(at, 0),
                  row_text(at, 0) == "Ala ma kota")
            st = status_text(at)
            check("pasek stanu mowi, ze plik nie ma nazwy i sie zmienil "
                  "('%s')" % st, "(bez nazwy)*" in st)
            check("i gdzie stoi kursor (wiersz 1, kolumna 12)",
                  "W1 K12" in st)

            q.sendkey("ret")
            q.type_line("i psa", enter=False)
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            check("Enter zaczyna nowy wiersz",
                  row_text(at, 0) == "Ala ma kota" and
                  row_text(at, 1) == "i psa")
            check("a pasek stanu liczy wiersze", "W2 K6" in status_text(at))

            # --- Ctrl+F: finding, and going round the end ---------------
            q.sendkey("ctrl-f")
            time.sleep(1)
            w, h, at = q.screen_raw()
            check("Ctrl+F pyta, czego szukac ('%s')" % status_text(at),
                  "Szukaj:" in status_text(at))
            q.type_line("kota", enter=False)
            time.sleep(0.5)
            q.sendkey("ret")
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            st = status_text(at)
            # The caret was on line two, past the word - so finding it
            # at all means the search went round the end of the file
            check("znajduje slowo LEZACE WYZEJ niz kursor, czyli okraza "
                  "koniec pliku ('%s')" % st,
                  "znaleziono w wierszu 1" in st and "W1 K8" in st)

            q.sendkey("ctrl-f")
            time.sleep(0.8)
            q.sendkey("ret")                   # nothing typed = the same again
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            check("Enter przy pustym pytaniu powtarza ostatnie szukanie "
                  "('%s')" % status_text(at),
                  "znaleziono" in status_text(at))

            q.sendkey("ctrl-f")
            time.sleep(0.8)
            q.type_line("czegotuniema", enter=False)
            time.sleep(0.5)
            q.sendkey("ret")
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            check("a czego nie ma, tego nie znajduje ('%s')"
                  % status_text(at), "nie znaleziono" in status_text(at))

            # --- Ctrl+Z: taking it back ---------------------------------
            q.sendkey("down")
            q.sendkey("end")
            time.sleep(0.5)
            q.sendkey("ctrl-z")
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            check("Ctrl+Z cofa ostatni znak ('%s')" % row_text(at, 1),
                  row_text(at, 1) == "i ps")
            check("i mowi, ze cofnal", "cofniete" in status_text(at))

            q.sendkey("ctrl-z")
            q.sendkey("ctrl-z")
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            # Three characters back the line reads "i " - the space is
            # still there, and the screen shows it as nothing
            check("i cofa dalej, po jednym ('%s')" % row_text(at, 1),
                  row_text(at, 1) == "i")

            # Back to what it was - the checks below expect it
            q.type_line("psa", enter=False)
            time.sleep(1)
            w, h, at = q.screen_raw()
            check("a to, co dopisano po cofnieciu, jest na swoim miejscu",
                  row_text(at, 1) == "i psa")

            # --- saving -------------------------------------------------
            q.sendkey("ctrl-s")
            time.sleep(1)
            w, h, at = q.screen_raw()
            check("Ctrl+S pyta o nazwe w pasku stanu",
                  "Zapisz jako:" in status_text(at))
            q.type_line("notatka", enter=False)
            time.sleep(0.5)
            q.sendkey("ret")
            time.sleep(2)
            w, h, at = q.screen_raw()
            st = status_text(at)
            check("po zapisie pasek zna nazwe i gwiazdki juz nie ma "
                  "('%s')" % st, "notatka" in st and "notatka*" not in st)
            # And the window says which file it holds - three editors
            # open on three files would be three windows called
            # "Edytor" otherwise
            check("okno nosi nazwe pliku ('%s')" % win_title(at),
                  win_title(at) == "Edytor: notatka")

            # --- and the file is really on the disk ---------------------
            # Checked by something that is not the program that wrote
            # it: the kernel's shell, which listens when no window has
            # the focus.
            q.mouse_goto(600, 430)
            q.mouse_click()                    # the desktop takes the focus
            time.sleep(1)
            q.type_line("cat notatka")
            time.sleep(2)
            log = q.serial()
            check("plik jest na dysku i ma to, co napisano",
                  "Ala ma kota" in log and "i psa" in log)

            # --- reading it back ----------------------------------------
            q.mouse_goto(CX + 200, CY + 100)   # back into the editor
            q.mouse_click()
            time.sleep(1)
            q.type_line("smieci", enter=False)
            time.sleep(1)
            q.sendkey("ctrl-o")
            time.sleep(1)
            w, h, at = q.screen_raw()
            check("Ctrl+O pyta, co otworzyc",
                  "Otworz:" in status_text(at))
            q.type_line("notatka", enter=False)
            time.sleep(0.5)
            q.sendkey("ret")
            time.sleep(2)
            w, h, at = q.screen_raw()
            check("otwarty plik zastapil to, co bylo w oknie",
                  row_text(at, 0) == "Ala ma kota" and
                  row_text(at, 1) == "i psa" and
                  row_text(at, 2) == "")

            # --- the clipboard, shared with the rest of the desktop -----
            q.sendkey("ctrl-insert")           # copies the caret's line
            time.sleep(1)
            q.sendkey("down")
            q.sendkey("down")
            q.sendkey("end")
            q.sendkey("ret")
            time.sleep(0.5)
            q.sendkey("shift-insert")
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            check("Ctrl+Insert i Shift+Insert przenosza wiersz "
                  "('%s')" % row_text(at, 2),
                  row_text(at, 2) == "Ala ma kota")

            # --- backspace joins two lines ------------------------------
            q.sendkey("home")
            time.sleep(0.5)
            q.sendkey("backspace")
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            check("Backspace na poczatku wiersza laczy go z poprzednim "
                  "('%s')" % row_text(at, 1),
                  row_text(at, 1) == "i psaAla ma kota" and
                  row_text(at, 2) == "")

            # --- closing is a request, and it can be declined -----------
            # A file with changes in it is not something to lose
            # because a window was closed. The editor answers the first
            # request with "not yet" - the desktop stops counting down
            # - and turns it into a question; the second close is the
            # answer to that question.
            q.type_line("zmiana", enter=False)
            # The pointer away from the page: the cursor is drawn white,
            # and left on the page it is indistinguishable from the page
            # - a probe would then say "the window is still there" about
            # a window that had gone.
            q.mouse_goto(600, 200)
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            row = h - TB_H + 2
            open_before = len(buttons(at, w, row))
            q.sendkey("alt-f4")
            time.sleep(3)
            w, h, at = q.screen_raw()
            st = status_text(at)
            check("Alt+F4 przy niezapisanym pliku NIE zamyka, tylko pyta "
                  "('%s')" % st, "Niezapisane zmiany" in st)
            check("i okno stoi tam, gdzie stalo",
                  is_page(at(CX + 200, CY + 100)))
            # And the desktop did stop counting: two seconds is the
            # whole of the grace, so a window still here five seconds
            # later was not merely slow to die
            time.sleep(5)
            w, h, at = q.screen_raw()
            check("pulpit przestal odliczac - po pieciu sekundach okno "
                  "dalej jest", is_page(at(CX + 200, CY + 100)))

            q.sendkey("alt-f4")
            left = open_before
            for _ in range(10):                # waiting, not asserting
                time.sleep(1)
                w, h, at = q.screen_raw()
                left = len(buttons(at, w, row))
                if left < open_before:
                    break
            check("drugie Alt+F4 zamyka (%d -> %d przyciskow na pasku)"
                  % (open_before, left), left == open_before - 1)
            check("i program zakonczyl sie sam, a nie zostal zabity",
                  "proces 'gedit' zakonczyl sie (kod 0)" in q.serial())

            # --- a file named when the editor is started ----------------
            # 'gedit notatka' opens that file at once. Making a person
            # type the name again, in the program they have just told
            # which file they mean, is what makes an editor a chore.
            q.mouse_goto(600, 430)
            q.mouse_click()
            time.sleep(1)
            q.type_line("run gedit notatka")
            time.sleep(4)
            q.mouse_goto(600, 430)
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            check("'gedit notatka' otwiera plik od razu ('%s')"
                  % row_text(at, 0),
                  row_text(at, 0) == "Ala ma kota" and
                  row_text(at, 1) == "i psa")

            # --- and the file manager can hand a file over --------------
            # What a file manager is FOR is getting to a file; getting
            # to a text file means opening it in the editor.
            q.sendkey("alt-f4")
            time.sleep(2.5)
            q.mouse_goto(24, 112)              # the "Pliki" icon
            q.mouse_click()
            time.sleep(4)
            q.mouse_goto(600, 430)
            time.sleep(1.5)
            w, h, at = q.screen_raw()

            listed = [file_row(at, i) for i in range(6)]
            row = -1
            for i, name in enumerate(listed):
                if name == "notatka":
                    row = i
            check("menedzer plikow widzi zapisany plik (%s)"
                  % ", ".join(n for n in listed if n), row >= 0)
            if row >= 0:
                q.mouse_goto(WX + 1 + 20, WY + 11 + FM_ROW_Y0 + row * 10 + 3)
                time.sleep(0.5)
                q.mouse_click()
                time.sleep(1.5)
                q.mouse_goto(WX + 1 + FM_OPEN_X + 20, WY + 11 + 5)  # "Otworz"
                time.sleep(0.5)
                q.mouse_click()
                time.sleep(4)
                q.mouse_goto(600, 430)
                time.sleep(1.5)
                w, h, at = q.screen_raw()
                # The new window is the next slot of the cascade
                check("przycisk 'Otworz' otwiera plik w edytorze ('%s')"
                      % row_text(at, 0, wx=WX + 12, wy=WY + 12),
                      row_text(at, 0, wx=WX + 12, wy=WY + 12) ==
                      "Ala ma kota")
        finally:
            q.close()
    finally:
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)

    print("--- ten sam edytor na jadrze 64-bitowym ---")
    sixty_four()
    return summary()


sys.exit(main())
