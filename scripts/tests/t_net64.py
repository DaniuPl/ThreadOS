#!/usr/bin/env python3
"""ThreadOS smoke test: the network as seen from a program in long mode.

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
    print("--- the network from a program's side (long mode) ---")

    if KERNEL64 is None or not os.path.exists(KERNEL64):
        print("SKIPPED: no 64-bit image (make kernel64)")
        return 0
    if shutil.which("grub-mkrescue") is None:
        print("SKIPPED: no grub-mkrescue")
        return 0
    if shutil.which("qemu-system-x86_64") is None:
        print("SKIPPED: no qemu-system-x86_64")
        return 0

    d = tempfile.mkdtemp()
    iso = tempfile.mktemp(suffix=".iso")
    slog = tempfile.mktemp(suffix=".log")
    http_port, http_stop = start_http_server()
    try:
        os.makedirs(d + "/boot/grub")
        shutil.copy(KERNEL64, d + "/boot/threados64.bin")
        mod = ""
        if INITRD64 and os.path.exists(INITRD64):
            shutil.copy(INITRD64, d + "/boot/initrd64.img")
            mod = "    module2 /boot/initrd64.img initrd64\n"
        with open(d + "/boot/grub/grub.cfg", "w") as fh:
            fh.write("set timeout=0\nset default=0\n"
                     'menuentry "ThreadOS 64" {\n'
                     "    multiboot2 /boot/threados64.bin\n" + mod + "}\n")
        made = subprocess.run(["grub-mkrescue", "-o", iso, d],
                              stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL).returncode == 0
        check("the disc with the 64-bit kernel builds", made)
        if not made:
            return summary()

        # On QEMU's user network the gateway 10.0.2.2 IS the host, so the
        # program connects to the server standing right here without any
        # forwarding - those are for traffic the other way
        q = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog,
                 qemu="qemu-system-x86_64")
        try:
            q.wait_serial("thread64>", timeout=60)
            q.wait_serial("[net] DHCP", timeout=40)

            seen = len(q.serial())
            q.type_line(f"run unet64 10.0.2.2 {http_port}")
            out = q.wait_serial("[unet64] done, exit code",
                                timeout=60)[seen:]

            m = re.search(r"my address (\d+\.\d+\.\d+\.\d+), "
                          r"gateway (\d+\.\d+\.\d+\.\d+)", out)
            check("the program asks the kernel for its DHCP address"
                  + (f" ({m.group(1)})" if m else ""),
                  m is not None)

            # The loopback does not go out on the wire, so this attempt
            # checks the stack itself, not the network around it
            m = re.search(r"the datagram came back to me \((\d+) B\) "
                          r"od (\d+\.\d+\.\d+\.\d+)", out)
            check("a datagram sent to itself comes back the same"
                  + (f" ({m.group(1)} B)" if m else ""),
                  m is not None and int(m.group(1)) > 0)

            m = re.search(r"the server answered \((\d+) B\): (HTTP/1\.\d)",
                          out)
            check("a TCP connection from ring 3 gets the server's answer"
                  + (f" ({m.group(2)}, {m.group(1)} B)" if m else ""),
                  m is not None)

            check("the program ends with no error",
                  "[unet64] done, exit code 0" in out)
            check("no attempt reported an error",
                  "[unet64] FAILED" not in out)
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
