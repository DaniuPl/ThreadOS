#!/usr/bin/env python3
"""Test dymny ProtonOS: ACPI, klawiatura USB, AHCI, konsole wirtualne, swap.

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
    # konczy dzialanie - to najuczciwszy dowod, jaki mozna miec.
    print("--- ACPI (wylaczanie zasilania) ---")
    q20 = Qemu(KERNEL, INITRD, append=TEXT_MODE)
    try:
        boot = q20.wait_for("ACPI:", timeout=30)
        check("tablice ACPI znalezione przy starcie",
              re.search(r"ACPI: tablice od '\S+", boot) is not None)
        q20.wait_for("ProtonOS shell", timeout=20)
        q20.type_line("help")
        check("'poweroff' jest w spisie komend",
              "poweroff" in q20.wait_for("Dostepne komendy", timeout=20))
        q20.type_line("poweroff")
        check("'poweroff' faktycznie gasi maszyne", q20.wait_exit(timeout=20))
    finally:
        q20.close()

    # --- Klawiatura USB --------------------------------------------
    # Kontroler USB, rozpoznanie urzadzenia, protokol rozruchowy HID.
    # QEMU kieruje klawisze do urzadzenia USB, wiec to, ze komenda
    # w ogole sie wykonuje, jest dowodem, ze przeszly ta droga.
    print("--- klawiatura USB (HID) ---")
    udisk = tempfile.mktemp(suffix=".img")
    with open(udisk, "wb") as fh:
        fh.truncate(DISK_SIZE)
    q19 = Qemu(KERNEL, INITRD, udisk, append=TEXT_MODE, usbkbd=True)
    try:
        boot = q19.wait_for("Klawiatura USB", timeout=30)
        check("klawiatura USB rozpoznana na magistrali",
              re.search(r"Klawiatura USB \(HID, UHCI\) na porcie \d+", boot)
              is not None)
        q19.wait_for("ProtonOS shell", timeout=20)
        q19.type_line("clear")
        q19.type_line("uptime")
        out = q19.wait_for("tyknac timera", timeout=20)
        check("komenda wpisana z klawiatury USB wykonuje sie",
              "uptime:" in out)
        # Wielka litera powstaje z modyfikatora w raporcie HID -
        # 'type_line' umie tylko male, wiec skladamy klawisze recznie.
        # Klawiature odpytujemy piecdziesiat razy na sekunde, a QEMU
        # trzyma klawisz okolo stu milisekund - przy zbyt szybkim
        # nadawaniu press i release sasiednich klawiszy zachodza na
        # siebie. Stad wolniejsze tempo i druga proba.
        def type_shift_a():
            for ch in "write":
                q19.sendkey(ch)
                time.sleep(0.15)
            for k in ("spc", "shift-a", "spc", "x", "ret"):
                q19.sendkey(k)
                time.sleep(0.25)

        saved = ""
        for attempt in range(3):
            type_shift_a()
            try:
                saved = q19.wait_for("zapisano", timeout=10)
                if "do 'A'" in saved:
                    break
            except RuntimeError:
                pass
            q19.type_line("clear")
        check("modyfikator Shift z raportu HID daje wielkie litery",
              "do 'A'" in saved)
    finally:
        q19.close()
        if os.path.exists(udisk):
            os.unlink(udisk)

    # --- Dysk za kontrolerem AHCI ----------------------------------
    # Ten sam system plikow i ta sama wymiana stron, tyle ze dane
    # przenosi kontroler przez DMA zamiast procesora slowo po slowie.
    print("--- kontroler AHCI (SATA) ---")
    sdisk = tempfile.mktemp(suffix=".img")
    with open(sdisk, "wb") as fh:
        fh.truncate(64 * 1024 * 1024)
    q18 = Qemu(KERNEL, INITRD, sdisk, append=TEXT_MODE, ram_mib=16, sata=True)
    try:
        boot = q18.wait_for("+ PFS gotowy", timeout=30)
        check("kontroler AHCI wykryty po PCI i wybrany zamiast ATA",
              "Dysk AHCI (SATA, DMA) + PFS gotowy" in boot)
        check("PFS sformatowal dysk przez AHCI",
              "sformatowany" in boot)
        check("plik wymiany zalozony na dysku SATA",
              re.search(r"Wymiana stron: \d+ KiB",
                        q18.wait_for("Wymiana stron:", timeout=20)) is not None)

        q18.wait_for("ProtonOS shell", timeout=20)
        q18.type_line("clear")
        q18.type_line("write ahci proton przez dma")
        q18.wait_for("zapisano", timeout=20)
        q18.type_line("cat ahci")
        back = q18.wait_for("proton przez dma", timeout=20)
        check("zapis i odczyt pliku przez DMA oddaja te same dane",
              "proton przez dma" in back)

        # Wymiana stron przez AHCI: te same 18 MiB na maszynie z 16 MiB
        q18.type_line("clear")
        q18.type_line("run swaptest 18")
        out = q18.wait_for("[swaptest] wszystkie", timeout=120)
        check("wymiana stron przez AHCI oddaje strony bez zmian",
              "stron wrocilo w calosci" in out)
    finally:
        q18.close()
        if os.path.exists(sdisk):
            os.unlink(sdisk)

    # --- Konsole wirtualne -----------------------------------------
    # Alt+F1..F4 przelacza ekran, ale kazda konsola ma wlasna tresc,
    # wlasny shell i wlasnych "swoich" pisarzy: program uruchomiony na
    # drugiej pisze na niej dalej, kiedy patrzymy na pierwsza.
    print("--- konsole wirtualne (Alt+F1..F4) ---")
    q17 = Qemu(KERNEL, INITRD, append=TEXT_MODE)
    try:
        q17.wait_for("ProtonOS shell", timeout=30)
        q17.type_line("clear")
        q17.type_line("uptime")
        first = q17.wait_for("tyknac timera", timeout=20)
        check("pierwsza konsola dziala jak dotad", "uptime:" in first)

        q17.sendkey("alt-f2")
        time.sleep(0.6)
        second = q17.screen()
        check("Alt+F2 pokazuje druga konsole z wlasnym promptem",
              "konsola 2" in second)
        check("tresc pierwszej konsoli nie przecieka na druga",
              "tyknac timera" not in second)

        q17.type_line("run spin")
        widok = q17.wait_for("[spin] tik", timeout=20)
        check("program uruchomiony na drugiej konsoli pisze na niej",
              "[spin] tik" in widok)

        q17.sendkey("alt-f1")
        time.sleep(1.5)
        back = q17.screen()
        check("pierwsza konsola nietknieta po powrocie",
              "tyknac timera" in back)
        check("program z drugiej konsoli nie pisze po pierwszej",
              "[spin] tik" not in back)

        q17.sendkey("alt-f2")
        time.sleep(0.5)
        before = q17.screen().count("[spin] tik")
        time.sleep(1.5)
        after = q17.screen().count("[spin] tik")
        check("program pisal na swoja konsole takze wtedy, gdy jej nie bylo"
              f" widac ({before} -> {after})",
              after != before or before >= 8)
        q17.type_line("kill spin")
        widok = q17.wait_for("zabity (kod -9)", timeout=20)
        check("kill z wlasnej konsoli konczy jej program",
              "zabity (kod -9)" in widok)
    finally:
        q17.close()

    # --- Wymiana stron na dysk -------------------------------------
    # Maszyna z 16 MiB RAM-u i program, ktory bierze 18 MiB: bez wymiany
    # nie ma prawa sie udac. Wzorzec zalezy od numeru strony, wiec strona
    # wczytana z niewlasciwego miejsca od razu sie wysypie.
    print("--- wymiana stron na dysk ---")
    swdisk = tempfile.mktemp(suffix=".img")
    with open(swdisk, "wb") as fh:
        fh.truncate(64 * 1024 * 1024)
    q16 = Qemu(KERNEL, INITRD, swdisk, append=TEXT_MODE, ram_mib=16)
    try:
        boot = q16.wait_for("Wymiana stron:", timeout=30)
        check("plik wymiany zakladany przy starcie",
              re.search(r"Wymiana stron: \d+ KiB na dysku", boot) is not None)
        q16.wait_for("ProtonOS shell", timeout=20)
        q16.type_line("clear")
        q16.type_line("run swaptest 18")
        out = q16.wait_for("[swaptest] wszystkie", timeout=120)
        check("18 MiB zmiescilo sie na maszynie z 16 MiB RAM-u",
              "stron wrocilo w calosci" in out)
        q16.type_line("clear")
        q16.type_line("mem")
        stats = q16.wait_for("wymiana:", timeout=20)
        m = re.search(r"wyrzuconych (\d+), sciagnietych (\d+)", stats)
        check("strony naprawde poszly na dysk i wrocily"
              + (f" (-{m.group(1)} +{m.group(2)})" if m else ""),
              m is not None and int(m.group(1)) > 1000
              and int(m.group(2)) > 1000)
        # Program skonczyl sie, wiec jego miejsca w pliku wymiany wrocily
        m = re.search(r"wymiana:\s+(\d+)/(\d+) stron", stats)
        check("miejsca w pliku wymiany zwolnione po smierci procesu"
              + (f" ({m.group(1)}/{m.group(2)})" if m else ""),
              m is not None and int(m.group(1)) == 0)
    finally:
        q16.close()
        if os.path.exists(swdisk):
            os.unlink(swdisk)

    return wynik()


if __name__ == "__main__":
    sys.exit(main())
