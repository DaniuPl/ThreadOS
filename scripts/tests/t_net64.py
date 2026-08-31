#!/usr/bin/env python3
"""ProtonOS smoke test: the network as seen from a program in long mode.

Until now only the kernel used the network - 'ping' and 'ifconfig' are
its own commands. Here we check the path through system calls: a program
in ring 3 asks for its own address, sends a datagram to itself and
connects to a server over TCP.

We put the HTTP server up on the host and forward a port to it, so the
attempt does not depend on any network outside.
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

        # On QEMU's user network the gateway 10.0.2.2 IS the host, so the
        # program connects to the server standing right here without any
        # forwarding - those are for traffic the other way
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

            # The loopback does not go out on the wire, so this attempt
            # checks the stack itself, not the network around it
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
