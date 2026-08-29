#!/usr/bin/env python3
"""Test dymny ProtonOS: kursor, okna, kontrolki, aplikacja ring 3, pasek zadan.

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
from ptest import check, wynik


def main():
    # --- Tryb graficzny (osobny boot: 'gfx' jest jednokierunkowy) ---
    print("--- tryb graficzny (screendump) ---")
    q3 = Qemu(KERNEL, INITRD, append=TEXT_MODE)
    try:
        q3.wait_for("ProtonOS shell")
        q3.type_line("gfx")
        time.sleep(1.0)
        sx, sy, at = q3.screen_pixels()
        check(f"tryb graficzny aktywny (ekran skalowany {sx}x{sy})",
              sx >= 1 and sy >= 1)

        def near(px, want, tol=40):
            return all(abs(px[i] - want[i]) <= tol for i in range(3))

        check("framebuffer: tlo niebieskie (fb 160,180)",
              near(at(160, 180), (0, 0, 255)))
        check("framebuffer: czerwony prostokat na srodku (fb 160,100)",
              near(at(160, 100), (255, 0, 0)))
        check("framebuffer: zielony prostokat w rogu (fb 30,30)",
              near(at(30, 30), (0, 255, 0)))
        check("framebuffer: zolty prostokat w rogu (fb 290,30)",
              near(at(290, 30), (255, 255, 0)))
    finally:
        q3.close()

    # --- Pulpit graficzny: kursor myszy (osobny boot) ---
    print("--- pulpit: kursor myszy ---")
    q4 = Qemu(KERNEL, INITRD)
    try:
        q4.wait_desktop()
        time.sleep(1.0)

        def near(px, want, tol=40):
            return all(abs(px[i] - want[i]) <= tol for i in range(3))

        # Pulpit dziala w wysokiej rozdzielczosci VESA (640x480)
        gw, gh, _ = q4.screen_raw()
        check(f"pulpit w trybie VESA wysokiej rozdzielczosci ({gw}x{gh})",
              gw == 640 and gh == 480)

        # mysz startuje na srodku (160,100); przesun do ~(50,40)
        q4.mouse_move(-110, -60)
        check("kursor rysowany w pozycji myszy (bialy ~50,41)",
              q4.wait_pixel(50, 41, (255, 255, 255)))
        _, _, at = q4.screen_raw()
        check("tlo pulpitu (teal) z dala od kursora, ikon i okien",
              near(at(350, 200), (0, 128, 160)))
        # przesun w inne miejsce - kursor podaza
        q4.mouse_move(150, 80)
        check("kursor podaza za mysza (bialy ~200,121)",
              q4.wait_pixel(200, 121, (255, 255, 255)))
        check("poprzednia pozycja kursora wyczyszczona (teal ~50,41)",
              q4.wait_pixel(50, 41, (0, 128, 160)))

        # Tekst (czcionka 8x8): pasek tytulu z napisem "ProtonOS".
        # Pasek jasnoszary; glif czarny -> komorka litery ma i czarne,
        # i szare piksele (czyli to ksztalt, nie pelny prostokat).
        _, _, at = q4.screen_raw()
        check("pasek tytulu narysowany (jasnoszary, puste miejsce)",
              near(at(200, 4), (192, 192, 192)))
        cell = [at(2 + c, 1 + r) for r in range(8) for c in range(8)]
        blk = sum(1 for p in cell if near(p, (0, 0, 0)))
        gry = sum(1 for p in cell if near(p, (192, 192, 192)))
        check(f"tekst renderowany czcionka 8x8 (glif 'P': {blk} czarnych, "
              f"{gry} szarych)", blk > 5 and gry > 5)
    finally:
        q4.close()

    # --- Pulpit: okna i przeciaganie mysza (osobny boot) ---
    print("--- pulpit: okna + drag ---")
    q5 = Qemu(KERNEL, INITRD)
    try:
        q5.wait_desktop()
        time.sleep(1.0)

        def near(px, want, tol=45):
            return all(abs(px[i] - want[i]) <= tol for i in range(3))

        # Okna: win0 (40,60,120x70) i win1 (180,95). Mysz startuje (160,100).
        check("okno 0: pasek tytulu niebieski (80,64)",
              q5.wait_pixel(80, 64, (0, 0, 255)))
        _, _, at = q5.screen_raw()
        check("okno 0: tlo szare (80,100)",
              near(at(80, 100), (224, 224, 224)))
        check("okno 1: pasek tytulu niebieski (220,99)",
              near(at(220, 99), (0, 0, 255)))

        # Przeciaganie okna 0: kursor (160,100) -> pasek win0 (60,64),
        # wcisnij LPM, przesun (+80,+50), pusc. Okno -> (120,110).
        q5.mouse_move(-100, -36)
        q5.mouse_button(True)
        q5.mouse_move(80, 50)
        q5.mouse_button(False)
        # nowy pasek win0 w widocznej (niezaslonietej) czesci: (150,114)
        check("okno 0 przeciagniete na nowa pozycje (pasek ~150,114)",
              q5.wait_pixel(150, 114, (0, 0, 255)))
        _, _, at = q5.screen_raw()
        check("stare miejsce okna 0 wyczyszczone (teal ~60,90)",
              near(at(60, 90), (0, 128, 160)))
        check("okno 1 nieruszone podczas przeciagania (220,99)",
              near(at(220, 99), (0, 0, 255)))
    finally:
        q5.close()

    # --- Pulpit: klikalne kontrolki (przycisk akcji + zamkniecie) ---
    print("--- pulpit: kontrolki (klik) ---")
    q6 = Qemu(KERNEL, INITRD)
    try:
        q6.wait_desktop()
        time.sleep(1.0)

        def near(px, want, tol=45):
            return all(abs(px[i] - want[i]) <= tol for i in range(3))

        # win0 (40,60,120x70): przycisk [x] czerwony w prawym rogu paska
        check("przycisk zamkniecia [x] (czerwony, ~153,63)",
              q6.wait_pixel(153, 63, (255, 0, 0)))
        _, _, at = q6.screen_raw()

        # Licznik przy przycisku 'Klik' (komorka ~94,80). Klikamy 3x.
        cell_before = [at(94 + c, 80 + r) for r in range(8) for c in range(8)]
        q6.mouse_goto(68, 84)            # srodek przycisku 'Klik'
        for _ in range(3):
            q6.mouse_click()
        time.sleep(0.6)
        _, _, at = q6.screen_raw()
        cell_after = [at(94 + c, 80 + r) for r in range(8) for c in range(8)]
        white_after = sum(1 for p in cell_after if near(p, (255, 255, 255), 10))
        check("przycisk akcji reaguje na klik (licznik zmienil cyfre)",
              cell_before != cell_after and white_after > 0)

        # Zamkniecie okna: klik [x] okna 1 (close box ~293,99)
        q6.mouse_goto(293, 99)
        q6.mouse_click()
        check("klik [x] zamyka okno (pasek win1 znika, wraca tlo)",
              q6.wait_pixel(220, 99, (0, 128, 160)))
        _, _, at = q6.screen_raw()
        # okno 0 nadal jest (klik nie ruszyl innych okien)
        check("inne okno pozostaje otwarte po zamknieciu (win0 pasek 80,64)",
              near(at(80, 64), (0, 0, 255)))
    finally:
        q6.close()

    # --- Aplikacja ring 3 z wlasnym oknem (rysowanie przez syscalle) ---
    print("--- pulpit: aplikacja ring 3 ---")
    q7 = Qemu(KERNEL, INITRD)
    try:
        q7.wait_desktop()
        time.sleep(0.8)
        q7.type_line("run guiapp")
        time.sleep(1.2)

        def near(px, want, tol=45):
            return all(abs(px[i] - want[i]) <= tol for i in range(3))

        # guiapp -> slot 2 (sloty 0/1 to okna jadra): okno (94,54),
        # obszar treści od (95,65). Kwadraty RGB rysowane przez syscalle.
        check("aplikacja ring3 narysowala czerwony kwadrat (101,71)",
              q7.wait_pixel(101, 71, (255, 0, 0)))
        check("aplikacja ring3 narysowala niebieski kwadrat (170,75)",
              q7.wait_pixel(170, 75, (0, 0, 255)))

        # Zdarzenia: klik w cialo okna trafia do aplikacji (win_poll),
        # ktora rysuje bialy znacznik w miejscu klikniecia. Licznik
        # 'klikniec:' przy (175,89) rosnie. Kursor odsuwamy, by nie
        # zaslonil znacznika przy zrzucie.
        cnt_before = [at(175 + c, 89 + r) for r in range(8) for c in range(8)]
        q7.mouse_goto(150, 110)
        q7.mouse_click()
        q7.mouse_goto(60, 140)            # odsun kursor od znacznika
        check("klik dostarczony do aplikacji: znacznik w miejscu (149,109)",
              q7.wait_pixel(149, 109, (255, 255, 255)))
        _, _, at = q7.screen_raw()
        cnt_after = [at(175 + c, 89 + r) for r in range(8) for c in range(8)]
        check("aplikacja zliczyla klikniecie (licznik zmienil cyfre)",
              cnt_before != cnt_after)

        # Klawiatura do okna z fokusem: okno jest sfokusowane (klikniete
        # wyzej). Po wpisaniu znakow pole 'txt:' (~135,103) pokazuje tekst.
        for k in ("a", "b", "c"):
            q7.sendkey(k)

        def txt_white():
            _, _, a = q7.screen_raw()
            return sum(1 for x in range(135, 159) for y in range(103, 111)
                       if near(a(x, y), (255, 255, 255), 10))

        appeared = False
        for _ in range(15):          # poll: tekst pojawia sie po kilku klatkach
            if txt_white() > 0:
                appeared = True
                break
            time.sleep(0.2)
        check("klawiatura trafia do okna z fokusem (tekst pojawil sie)",
              appeared)

        # Klik w pulpit (puste tlo) zdejmuje fokus, by 'kill' trafilo
        # do shella jadra, a nie do okna aplikacji
        q7.mouse_goto(60, 140)
        q7.mouse_click()
        time.sleep(0.2)

        # Zabicie aplikacji: kompozytor sprzata jej okno. Zabijamy po
        # nazwie - pid zalezy od tego, ile watkow wystartowalo wczesniej
        q7.type_line("kill guiapp")
        check("po zabiciu aplikacji jej okno zostaje sprzatniete (170,75 teal)",
              q7.wait_pixel(170, 75, (0, 128, 160)))
    finally:
        q7.close()

    # --- Pasek zadan i menu uruchamiania (osobny boot) ---
    print("--- pulpit: pasek zadan + menu ---")
    q8 = Qemu(KERNEL, INITRD)
    try:
        q8.wait_desktop()

        def near(px, want, tol=45):
            return all(abs(px[i] - want[i]) <= tol for i in range(3))

        # Pasek zadan na dole (640x480 -> y=468); przycisk 'Menu' x2..42
        check("pasek zadan: przycisk 'Menu' (niebieski, 30,470)",
              q8.wait_pixel(30, 470, (0, 0, 255)))
        _, _, at = q8.screen_raw()
        # przed uruchomieniem aplikacji slot 2 jest pusty (tlo okna jadra)
        check("przed launchem brak okna aplikacji (101,71 nie czerwone)",
              not near(at(101, 71), (255, 0, 0)))

        # Wpisy menu liczone z MENU_ITEMS (patrz menu_y)
        q8.mouse_goto(10, 472)
        q8.mouse_click()
        check("klik 'Menu' rozwija menu (wpis szary)",
              q8.wait_pixel(70, menu_y("guiapp"), (224, 224, 224)))

        # Klik wpisu 'guiapp' (entry0) -> uruchamia aplikacje
        q8.mouse_goto(40, menu_y("guiapp"))
        q8.mouse_click()
        check("wybor z menu uruchamia aplikacje (czerwony kwadrat 101,71)",
              q8.wait_pixel(101, 71, (255, 0, 0)))
        _, _, at = q8.screen_raw()
        check("menu zamyka sie po wyborze (wpis znika)",
              not near(at(70, menu_y("guiapp")), (224, 224, 224)))

        # Lista okien na pasku zadan: przyciski Terminal/Pliki/guiapp.
        # guiapp jest na wierzchu (swiezo uruchomione) - czerwony przy (101,71).
        # Klik przycisku 'Terminal' (slot0, j0, x46..96, y468) wynosi je na
        # wierzch i fokusuje: czerwony guiappa zostaje zakryty oknem jadra.
        q8.mouse_goto(60, 473)
        q8.mouse_click()
        check("klik przycisku na pasku wynosi okno na wierzch (czerwony zakryty)",
              q8.wait_pixel(101, 71, (224, 224, 224), 15))
        check("fokusowane okno podswietlone na pasku zadan (niebieski 90,469)",
              q8.wait_pixel(90, 469, (0, 0, 255)))
    finally:
        q8.close()

    return wynik()


if __name__ == "__main__":
    sys.exit(main())
