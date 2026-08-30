#!/usr/bin/env python3
"""Test dymny ProtonOS: serwer TCP w ring 3 i przegladarka graficzna.

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
    # --- ProtonOS jako serwer TCP (pasywne otwarcie, osobny boot) ---
    print("--- siec: serwer TCP w ring 3 ---")
    srv_port = 5555
    q_srv = Qemu(KERNEL, INITRD, hostfwd_port=srv_port, append=TEXT_MODE)
    try:
        q_srv.wait_for("ProtonOS shell")
        q_srv.wait_for("[siec] DHCP", timeout=20)
        q_srv.type_line("run tcpserv")
        view = q_srv.wait_for("nasluchuje na porcie 5555", timeout=15)
        check("ring 3 nasluchuje na porcie TCP",
              "nasluchuje na porcie 5555" in view)

        # Laczy sie prawdziwy stos hosta (przez hostfwd QEMU)
        sock = socket.create_connection(("127.0.0.1", srv_port), timeout=15)
        sock.settimeout(15)
        greeting = sock.recv(200)
        check("host polaczyl sie i dostal powitanie od ProtonOS-a",
              b"ProtonOS TCP gotowy" in greeting)

        sock.sendall(b"czesc z hosta\n")
        echoed = sock.recv(200)
        check("dane z hosta wrocily odbite przez serwer w ring 3",
              b"czesc z hosta" in echoed)
        sock.close()

        seen = q_srv.wait_for("od klienta", timeout=15)
        check("serwer zobaczyl dane klienta",
              "od klienta: czesc z hosta" in seen)
    finally:
        q_srv.close()

    # --- Przegladarka w oknie pulpitu (siec -> GUI -> dysk) ---
    print("--- pulpit: przegladarka (gweb) ---")
    wdisk = tempfile.mktemp(suffix=".img")
    with open(wdisk, "wb") as f:
        f.write(b"\0" * DISK_SIZE)
    http_port, http_stop = start_http_server()
    dns_stop = start_dns_server()
    q_web = Qemu(KERNEL, INITRD, wdisk)
    try:
        q_web.wait_desktop()
        time.sleep(2.0)                  # chwila na dzierzawe DHCP

        # Serwer DNS hosta: przegladarka bedzie szukac adresu po nazwie.
        # Komenda idzie do shella jadra - w trybie graficznym slucha on
        # klawiatury, dopoki zadne okno nie ma fokusu.
        q_web.type_line("dns 10.0.2.2")
        time.sleep(0.5)
        time.sleep(1.0)
        q_web.type_line("run gweb")
        time.sleep(1.5)

        def page_white():
            _, _, a = q_web.screen_raw()
            return sum(1 for y in range(100, 250) for x in range(96, 400)
                       if a(x, y) == (255, 255, 255))

        before = page_white()

        # Kalibracja kursora - klikniecia w odnosniki musza trafiac co do
        # piksela, a delty myszy licza sie od biezacego polozenia
        q_web.mouse_home()

        # Klik w okno daje mu fokus klawiatury; potem kursor na bok
        q_web.mouse_goto(150, 120)
        q_web.mouse_click()
        q_web.mouse_goto(400, 350)
        time.sleep(0.3)

        for _ in range(20):              # skasuj domyslny adres
            q_web.sendkey("backspace")
        for ch in "protonos.test:%d/" % http_port:   # adres po nazwie
            q_web.sendkey(Qemu.KEYNAMES.get(ch, ch))
        q_web.sendkey("ret")

        shown = False
        for _ in range(25):
            if page_white() > before + 50:
                shown = True
                break
            time.sleep(0.4)
        check("przegladarka rozwiazala nazwe, pobrala strone i "
              "narysowala tekst", shown)

        # --- przewijanie i odnosniki ---
        for _ in range(40):              # nowy adres: strona z odnosnikiem
            q_web.sendkey("backspace")
        for ch in "protonos.test:%d/linki" % http_port:
            q_web.sendkey(Qemu.KEYNAMES.get(ch, ch))
        q_web.sendkey("ret")
        time.sleep(5)

        def link_pixels():
            """Odnosniki rysowane sa kolorem 5 z palety (zolty)."""
            _, _, a = q_web.screen_raw()
            return [(x, y) for y in range(180, 260) for x in range(96, 420)
                    if a(x, y) == (255, 255, 0)]

        first_view = page_white()
        q_web.sendkey("pgdn")
        time.sleep(1.0)
        check("PgDn przewija tresc w oknie", page_white() != first_view)

        found = []
        for _ in range(8):               # zjedz do konca, gdzie jest odnosnik
            found = link_pixels()
            if found:
                break
            q_web.sendkey("pgdn")
            time.sleep(0.8)
        check("odnosnik wyrozniony kolorem w tekscie", bool(found))

        if found:
            # Odnosnik potrafi zajmowac dwa wiersze - srodek ciezkosci
            # wszystkich pikseli wypadlby wtedy w przerwie miedzy nimi.
            # Bierzemy najgestszy wiersz i jego srodek.
            rows = {}
            for x, y in found:
                rows.setdefault(y, []).append(x)
            best_y = max(rows, key=lambda y: len(rows[y]))
            xs = sorted(rows[best_y])
            cx, cy = xs[len(xs) // 2], best_y
            q_web.mouse_goto(cx, cy)
            q_web.mouse_click()
            q_web.mouse_goto(560, 400)
            time.sleep(5)
    finally:
        q_web.close()
        http_stop()
        dns_stop()

    # Ten sam dysk w trybie tekstowym: tresc ze swiata przezyla restart
    q_web2 = Qemu(KERNEL, INITRD, wdisk, append=TEXT_MODE)
    try:
        q_web2.wait_for("ProtonOS shell")
        q_web2.type_line("cat strona.htm")
        saved = q_web2.wait_for("Strona druga", timeout=10)
        check("klikniety odnosnik pobral druga strone (widac ja na dysku)",
              "Odnosnik zadzialal" in saved)
        check("strona zapisana przez przegladarke przezyla restart",
              "<html><body><h1>Strona druga</h1>" in saved)
    finally:
        q_web2.close()
        if os.path.exists(wdisk):
            os.unlink(wdisk)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
