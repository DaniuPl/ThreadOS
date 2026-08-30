#!/usr/bin/env python3
"""Test dymny ProtonOS: siec od strony programu w trybie dlugim.

Do tej pory z sieci korzystalo tylko jadro - 'ping' i 'ifconfig' to
jego wlasne polecenia. Tutaj sprawdzamy droge przez wywolania
systemowe: program w ring 3 pyta o swoj adres, wysyla datagram do
samego siebie i laczy sie z serwerem po TCP.

Serwer HTTP stawiamy na hoscie i przekierowujemy do niego port, wiec
proba nie zalezy od zadnej sieci na zewnatrz.
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
    print("--- siec od strony programu (tryb dlugi) ---")

    if KERNEL64 is None or not os.path.exists(KERNEL64):
        print("POMINIETE: brak obrazu 64-bitowego (make kernel64)")
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
    http_port, http_stop = start_http_server()
    try:
        os.makedirs(d + "/boot/grub")
        shutil.copy(KERNEL64, d + "/boot/protonos64.bin")
        mod = ""
        if INITRD64 and os.path.exists(INITRD64):
            shutil.copy(INITRD64, d + "/boot/initrd64.img")
            mod = "    module2 /boot/initrd64.img initrd64\n"
        with open(d + "/boot/grub/grub.cfg", "w") as fh:
            fh.write("set timeout=0\nset default=0\n"
                     'menuentry "ProtonOS 64" {\n'
                     "    multiboot2 /boot/protonos64.bin\n" + mod + "}\n")
        made = subprocess.run(["grub-mkrescue", "-o", iso, d],
                              stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL).returncode == 0
        check("plyta z jadrem 64-bitowym buduje sie", made)
        if not made:
            return summary()

        # W sieci uzytkownika QEMU brama 10.0.2.2 JEST hostem, wiec
        # program laczy sie z serwerem stojacym tu obok bez zadnego
        # przekierowania - te sa od ruchu w druga strone
        q = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog,
                 qemu="qemu-system-x86_64")
        try:
            q.wait_serial("proton64>", timeout=60)
            q.wait_serial("[siec] DHCP", timeout=40)

            seen = len(q.serial())
            q.type_line(f"run unet64 10.0.2.2 {http_port}")
            out = q.wait_serial("[unet64] koniec, kod wyjscia",
                                timeout=60)[seen:]

            m = re.search(r"moj adres (\d+\.\d+\.\d+\.\d+), "
                          r"brama (\d+\.\d+\.\d+\.\d+)", out)
            check("program pyta jadro o swoj adres z DHCP"
                  + (f" ({m.group(1)})" if m else ""),
                  m is not None)

            # Petla zwrotna nie wychodzi na kabel, wiec ta proba
            # sprawdza sam stos, a nie siec dookola
            m = re.search(r"datagram wrocil do mnie \((\d+) B\) "
                          r"od (\d+\.\d+\.\d+\.\d+)", out)
            check("datagram wyslany do siebie wraca taki sam"
                  + (f" ({m.group(1)} B)" if m else ""),
                  m is not None and int(m.group(1)) > 0)

            m = re.search(r"serwer odpowiedzial \((\d+) B\): (HTTP/1\.\d)",
                          out)
            check("polaczenie TCP z ring 3 dostaje odpowiedz serwera"
                  + (f" ({m.group(2)}, {m.group(1)} B)" if m else ""),
                  m is not None)

            check("program konczy sie bez bledu",
                  "[unet64] koniec, kod wyjscia 0" in out)
            check("zadna proba nie zglosila bledu",
                  "[unet64] BLAD" not in out)
        finally:
            q.close()
    finally:
        http_stop()
        shutil.rmtree(d, ignore_errors=True)
        for f in (iso, slog):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
