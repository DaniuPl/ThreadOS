#!/usr/bin/env python3
"""Test dymny ProtonOS: aplikacje graficzne w pierscieniu 3 (jadro 64-bit).

Pulpit dzialal do tej pory sam ze soba - kompozytor siedzial w jadrze,
a ikony wskazywaly programy, ktorych w trybie dlugim nie bylo. Teraz
ikona uruchamia prawdziwy proces z initrd, ktory rysuje we wlasnym
oknie przez wywolania systemowe i dostaje z powrotem zdarzenia.

Sprawdzamy cala te droge tak, jak widzi ja uzytkownik: klikniecie
ikony ma dac okno z narysowana paleta, klikniecie w okno ma zostawic
w nim znacznik (czyli zdarzenie doszlo do procesu i wrocilo rysunkiem),
a klawisze maja trafiac do okna z fokusem.

Okno aplikacji jest trzecim z kolei (dwa pierwsze to okna jadra), wiec
kompozytor stawia je pod (94,54); obszar tresci zaczyna sie o piksel
w prawo i o pasek tytulu nizej - pod (95,65). Wszystkie wspolrzedne
ekranu ponizej biora sie z tego jednego punktu.
"""

import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, wynik

TRESC_X, TRESC_Y = 95, 65    # lewy gorny rog obszaru tresci okna aplikacji

CZERWONY = (255, 0, 0)
ZIELONY = (0, 255, 0)
NIEBIESKI = (0, 0, 255)
BIALY = (255, 255, 255)


def ekr(cx, cy):
    """Wspolrzedne ekranu punktu (cx,cy) obszaru tresci okna."""
    return TRESC_X + cx, TRESC_Y + cy


def main():
    print("--- aplikacja graficzna w ring 3 (jadro 64-bitowe) ---")

    if KERNEL64 is None or not os.path.exists(KERNEL64):
        print("POMINIETE: brak obrazu 64-bitowego (make kernel64)")
        return 0
    if INITRD64 is None or not os.path.exists(INITRD64):
        print("POMINIETE: brak initrd 64-bitowego")
        return 0
    if shutil.which("grub-mkrescue") is None:
        print("POMINIETE: brak grub-mkrescue")
        return 0
    if shutil.which("qemu-system-x86_64") is None:
        print("POMINIETE: brak qemu-system-x86_64")
        return 0

    d = tempfile.mkdtemp()
    iso = tempfile.mktemp(suffix=".iso")
    slog = tempfile.mktemp(suffix=".log")
    try:
        os.makedirs(d + "/boot/grub")
        shutil.copy(KERNEL64, d + "/boot/protonos64.bin")
        shutil.copy(INITRD64, d + "/boot/initrd64.img")
        with open(d + "/boot/grub/grub.cfg", "w") as fh:
            fh.write("insmod all_video\nset gfxpayload=keep\n"
                     "set timeout=0\nset default=0\n"
                     'menuentry "ProtonOS 64" {\n'
                     "    multiboot2 /boot/protonos64.bin\n"
                     "    module2 /boot/initrd64.img initrd64\n}\n")
        made = subprocess.run(["grub-mkrescue", "-o", iso, d],
                              stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL).returncode == 0
        check("plyta z jadrem 64-bitowym buduje sie", made)
        if not made:
            return wynik()

        q = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog,
                 qemu="qemu-system-x86_64")
        try:
            q.wait_serial("proton64>", timeout=60)
            q.type_line("pulpit")
            q.wait_serial("Pulpit startuje", timeout=30)
            check("pulpit wstaje w trybie graficznym", q.wait_desktop(30))

            # Mysz jest wzgledna: najpierw dosun kursor do rogu, zeby
            # wiedziec, gdzie w ogole jest.
            q.mouse_home()
            q.mouse_goto(24, 32)          # ikona 'App' (kafelek 8,20,32x24)
            q.mouse_click()
            q.mouse_goto(400, 300)        # odsun kursor od okna

            # Aplikacja rysuje palete trzech kwadratow u gory okna.
            # Kazdy z innego wywolania systemowego, wiec kazdy osobno.
            cx, cy = ekr(10, 10)
            check("klik ikony uruchamia program z initrd (czerwony kwadrat)",
                  q.wait_pixel(cx, cy, CZERWONY, timeout=20))
            cx, cy = ekr(38, 10)
            check("program rysuje drugi kwadrat (zielony)",
                  q.wait_pixel(cx, cy, ZIELONY, timeout=5))
            cx, cy = ekr(66, 10)
            check("program rysuje trzeci kwadrat (niebieski)",
                  q.wait_pixel(cx, cy, NIEBIESKI, timeout=5))

            # Podpis 'klikniec:' rysowany przez win_text - bialy tekst
            # na szarym tle; szukamy w jego pasie bialych pikseli.
            _, _, at = q.screen_raw()
            tx, ty = ekr(6, 24)
            bialych = sum(1 for dy in range(8) for dx in range(72)
                          if at(tx + dx, ty + dy) == BIALY)
            check(f"program pisze tekst w oknie ({bialych} bialych pikseli)",
                  bialych > 20)

            # Licznik klikniec przed kliknieciem - zapamietujemy komorke
            # z cyfra, zeby bylo z czym porownac
            lx, ly = ekr(80, 24)

            def komorka_licznika(fun):
                return [fun(lx + dx, ly + dy)
                        for dy in range(8) for dx in range(8)]

            licznik_przed = komorka_licznika(at)

            # Zdarzenie w druga strone: klik w obszar tresci ma dojsc do
            # procesu, ktory odpowiada bialym znacznikiem w tym miejscu.
            # Tolerancja musi tu byc waska: szare tlo okna (227,227,227)
            # miesci sie w domyslnej i przepusciloby test, w ktorym nie
            # dzieje sie nic.
            kx, ky = ekr(20, 50)
            q.mouse_goto(kx, ky)
            q.mouse_click()
            q.mouse_goto(400, 300)
            check("klik w okno wraca do programu jako zdarzenie (znacznik)",
                  q.wait_pixel(kx, ky, BIALY, tol=8, timeout=10))

            # Licznik klikniec przestaje byc zerem - program policzyl
            # zdarzenie, a nie tylko odrysowal okno
            _, _, at2 = q.screen_raw()
            check("licznik klikniec w oknie rosnie",
                  komorka_licznika(at2) != licznik_przed)

            # Klawiatura: okno ma fokus po kliknieciu, wiec wpisane
            # znaki maja pojawic sie w polu 'txt:'.
            px, py = ekr(40, 38)

            def pole_txt(fun):
                return [fun(px + dx, py + dy)
                        for dy in range(8) for dx in range(40)]

            przed = pole_txt(at2)
            q.sendkey("o")
            q.sendkey("k")
            time.sleep(1.5)
            _, _, at3 = q.screen_raw()
            po = pole_txt(at3)
            check("klawisze trafiaja do okna z fokusem", przed != po)

            # Kompozytor przezyl to wszystko: tlo pulpitu dalej jest.
            w, h, at4 = q.screen_raw()
            tlo = at4(w - 20, h // 2)
            check("kompozytor dalej rysuje pulpit",
                  all(abs(tlo[i] - (0, 130, 162)[i]) <= 40 for i in range(3)))
        finally:
            q.close()
    finally:
        shutil.rmtree(d, ignore_errors=True)
        for f in (iso, slog):
            if os.path.exists(f):
                os.unlink(f)

    return wynik()


if __name__ == "__main__":
    sys.exit(main())
