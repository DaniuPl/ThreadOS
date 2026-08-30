#!/usr/bin/env python3
"""Test dymny ProtonOS: menedzer plikow, ikony, monitor, terminal, rozmiar okien.

Uruchamiany osobno albo przez scripts/run_testy.py, ktory puszcza
wszystkie grupy rownolegle.
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary


def main():
    # --- Menedzer plikow GUI: FS -> okno aplikacji (osobny boot z dyskiem) ---
    print("--- pulpit: menedzer plikow (gfiles) ---")
    gdisk = tempfile.mktemp(suffix=".img")
    with open(gdisk, "wb") as f:
        f.write(b"\0" * DISK_SIZE)
    q9 = Qemu(KERNEL, INITRD, gdisk)
    try:
        q9.wait_desktop()
        q9.type_line("write raport abc")   # plik na dysku (przed pulpitem)
        time.sleep(0.5)
        time.sleep(1.0)

        def near(px, want, tol=45):
            return all(abs(px[i] - want[i]) <= tol for i in range(3))

        # Menu -> wpis 'gfiles'
        q9.mouse_goto(10, 472)
        q9.mouse_click()
        q9.mouse_goto(40, menu_y("gfiles"))
        q9.mouse_click()
        # gfiles slot2 (94,54), content (95,65); nazwa pliku przy
        # content(8,16) -> screen(103,81). Tekst (bialy) = pliki z PFS.
        def files_row_white():
            _, _, a = q9.screen_raw()
            return sum(1 for x in range(103, 160) for y in range(81, 89)
                       if near(a(x, y), (255, 255, 255), 10))
        appeared = False
        for _ in range(15):
            if files_row_white() > 0:
                appeared = True
                break
            time.sleep(0.2)
        check("menedzer plikow GUI listuje plik z dysku (FS -> okno)",
              appeared)

        # Klik nazwy pliku -> podglad tresci. Wiersz0 przy content(.,16)
        # -> ekran y81, kolejne co 10 px. Klikamy WIERSZ 1: wiersz 0 to
        # 'swap.sys', ktory jadro zaklada przy starcie (i-wezel 0), a nam
        # chodzi o 'raport'. Po kliknieciu panel 'tresc:' (content y102 ->
        # ekran y167) pokazuje zawartosc. Kursor odsuwamy przed zrzutem.
        q9.mouse_goto(115, 93)
        q9.mouse_click()
        q9.mouse_goto(60, 300)

        def preview_white():
            _, _, a = q9.screen_raw()
            return sum(1 for x in range(99, 200) for y in range(167, 175)
                       if near(a(x, y), (255, 255, 255), 10))
        prev = False
        for _ in range(15):
            if preview_white() > 0:
                prev = True
                break
            time.sleep(0.2)
        check("klik pliku w gfiles pokazuje jego tresc (open+read z ring3)",
              prev)
    finally:
        q9.close()
        if os.path.exists(gdisk):
            os.unlink(gdisk)

    # --- Ikony na pulpicie (osobny boot) ---
    print("--- pulpit: ikony ---")
    q10 = Qemu(KERNEL, INITRD)
    try:
        q10.wait_desktop()
        time.sleep(1.0)

        def near(px, want, tol=45):
            return all(abs(px[i] - want[i]) <= tol for i in range(3))

        # ikona 'App' (kafelek 8,20,32x24) pomaranczowa
        check("ikona pulpitu narysowana (pomaranczowy kafelek 20,30)",
              q10.wait_pixel(20, 30, (255, 162, 0)))
        # klik ikony uruchamia aplikacje (guiapp -> czerwony kwadrat)
        q10.mouse_goto(24, 32)
        q10.mouse_click()
        q10.mouse_goto(300, 300)         # odsun kursor
        check("klik ikony uruchamia aplikacje (czerwony kwadrat 101,71)",
              q10.wait_pixel(101, 71, (255, 0, 0)))
    finally:
        q10.close()

    # --- Wskaznik zasobow (gmon) na zywo ---
    print("--- pulpit: monitor zasobow (gmon) ---")
    q11 = Qemu(KERNEL, INITRD)
    try:
        q11.wait_desktop()
        time.sleep(0.8)
        q11.type_line("run gmon")

        def near(px, want, tol=45):
            return all(abs(px[i] - want[i]) <= tol for i in range(3))

        # gmon slot2 (94,54), content (95,65). Pasek RAM (content y48)
        # zawiera niebieski fragment uzycia -> ekran ~ (101,116).
        check("monitor zasobow: pasek RAM (niebieski 101,116)",
              q11.wait_pixel(101, 116, (0, 0, 255)))

        # uptime (liczba przy content(96,28) -> ekran (191,93)) rosnie:
        # komorka tekstu zmienia sie w czasie -> zywy monitor
        def up_cell():
            _, _, a = q11.screen_raw()
            return [a(191 + c, 93 + r) for r in range(8) for c in range(24)]
        c1 = up_cell()
        time.sleep(1.6)
        c2 = up_cell()
        check("monitor zasobow zywy: uptime aktualizuje sie w oknie",
              c1 != c2)
    finally:
        q11.close()

    # --- Terminal w oknie GUI (osobny boot z dyskiem) ---
    print("--- pulpit: terminal GUI (gterm) ---")
    tdisk = tempfile.mktemp(suffix=".img")
    with open(tdisk, "wb") as f:
        f.write(b"\0" * DISK_SIZE)
    q12 = Qemu(KERNEL, INITRD, tdisk)
    try:
        q12.wait_desktop()
        time.sleep(0.8)
        q12.type_line("run gterm")
        time.sleep(1.0)

        def near(px, want, tol=45):
            return all(abs(px[i] - want[i]) <= tol for i in range(3))

        # klik w okno terminala (slot2, content 95,65) -> fokus klawiatury
        q12.mouse_goto(150, 120)
        q12.mouse_click()
        q12.mouse_goto(400, 300)
        # wpisz 'seq 3' i Enter -> wyjscie (1,2,3) pojawia sie w oknie
        for ch in "seq 3":
            q12.sendkey({" ": "spc"}.get(ch, ch))
        q12.sendkey("ret")

        # wyjscie rysowane u gory okna (content y4 -> ekran y69)
        def out_white():
            _, _, a = q12.screen_raw()
            return sum(1 for x in range(99, 130) for y in range(69, 99)
                       if near(a(x, y), (255, 255, 255), 10))
        shown = False
        for _ in range(20):
            if out_white() > 0:
                shown = True
                break
            time.sleep(0.2)
        check("terminal GUI uruchamia komende i pokazuje wyjscie w oknie",
              shown)
    finally:
        q12.close()
        if os.path.exists(tdisk):
            os.unlink(tdisk)

    # --- Zwijanie i zmiana rozmiaru okien (osobny boot) ---
    print("--- pulpit: minimalizacja + zmiana rozmiaru ---")
    q13 = Qemu(KERNEL, INITRD)
    try:
        q13.wait_desktop()
        time.sleep(1.0)

        def near(px, want, tol=45):
            return all(abs(px[i] - want[i]) <= tol for i in range(3))

        # win0 "Terminal" (40,60,120x70): pasek tytulu niebieski na starcie
        check("okno 0: pasek tytulu niebieski przed zwinieciem (80,64)",
              q13.wait_pixel(80, 64, (0, 0, 255)))

        # Przycisk zwijania [_] win0: min_x = x+w-2*8-2 = 142, y=61 (8x8).
        # Klik srodka (146,65) zwija okno -> pasek znika, wraca tlo.
        q13.mouse_goto(146, 65)
        q13.mouse_click()
        q13.mouse_goto(400, 300)              # odsun kursor
        check("zwiniete okno znika z pulpitu (pasek 80,64 -> teal)",
              q13.wait_pixel(80, 64, (0, 128, 160)))

        # Okno wciaz na pasku zadan (slot0 'Terminal', x46..96, y468).
        # Klik przywraca je: pasek tytulu niebieski znow widoczny.
        q13.mouse_goto(60, 473)
        q13.mouse_click()
        q13.mouse_goto(400, 300)
        check("klik na pasku zadan przywraca zwiniete okno (80,64 niebieski)",
              q13.wait_pixel(80, 64, (0, 0, 255)))

        # Zmiana rozmiaru: uchwyt win0 w prawym dolnym rogu
        # grip_x = x+w-8 = 152, grip_y = y+h-8 = 122. Przed zmiana punkt
        # (170,140) jest poza oknem (teal). Przeciagamy uchwyt (155,125)
        # o (+60,+40) -> okno rosnie i pokrywa (170,140).
        _, _, at = q13.screen_raw()
        check("przed zmiana rozmiaru punkt poza oknem (170,140 teal)",
              near(at(170, 140), (0, 128, 160)))
        q13.mouse_goto(155, 125)
        q13.mouse_button(True)
        q13.mouse_move(60, 40)
        q13.mouse_button(False)
        q13.mouse_goto(400, 300)
        check("po zmianie rozmiaru okno powiekszone (170,140 szare tlo okna)",
              q13.wait_pixel(170, 140, (224, 224, 224)))
    finally:
        q13.close()

    # --- Zapis pozycji ikon na dysku (dwa boot-y na tym samym dysku) ---
    print("--- pulpit: zapis pozycji ikon na dysku ---")
    idisk = tempfile.mktemp(suffix=".img")
    with open(idisk, "wb") as f:
        f.write(b"\0" * DISK_SIZE)
    q14 = Qemu(KERNEL, INITRD, idisk)
    try:
        q14.wait_desktop()
        time.sleep(1.0)

        def near(px, want, tol=45):
            return all(abs(px[i] - want[i]) <= tol for i in range(3))

        # ikona 'App' domyslnie (8,20,32x24) - kafelek pomaranczowy ~ (20,30)
        check("ikona App w domyslnej pozycji (20,30 pomaranczowy)",
              q14.wait_pixel(20, 30, (255, 162, 0)))

        # Przeciagnij ikone: chwyt srodka (24,32), przesun na (416,312) z
        # wcisnietym LPM -> ikona laduje w (400,300). Pusc.
        q14.mouse_goto(24, 32)
        q14.mouse_button(True)
        q14.mouse_goto(416, 312)
        q14.mouse_button(False)
        q14.mouse_goto(100, 250)          # odsun kursor od ikony
        check("ikona przeniesiona na nowa pozycje (410,310 pomaranczowy)",
              q14.wait_pixel(410, 310, (255, 162, 0)))
        check("stara pozycja ikony wyczyszczona (20,30 teal)",
              q14.wait_pixel(20, 30, (0, 128, 160)))
    finally:
        q14.close()

    # Reboot na TYM SAMYM dysku: pozycja ikony przetrwala (icons.pos)
    q15 = Qemu(KERNEL, INITRD, idisk)
    try:
        q15.wait_desktop()
        time.sleep(1.0)

        def near(px, want, tol=45):
            return all(abs(px[i] - want[i]) <= tol for i in range(3))

        check("pozycja ikony przetrwala restart (410,310 pomaranczowy)",
              q15.wait_pixel(410, 310, (255, 162, 0)))
        check("domyslna pozycja ikony pusta po restarcie (20,30 teal)",
              q15.wait_pixel(20, 30, (0, 128, 160)))
    finally:
        q15.close()
        if os.path.exists(idisk):
            os.unlink(idisk)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
