#!/usr/bin/env python3
"""Runs the groups of the ThreadOS smoke test - in parallel by default.

Every group needs its own QEMU machine, and those spend most of their
time waiting anyway: for the boot, for an answer, for the screen to be
redrawn. Running them one after another took a good ten minutes, of
which the processor was busy for a fraction.

Every group's output we hold whole and print only once it has finished -
otherwise lines from several machines would mix into one unreadable
column.

  make test              all the groups
  make test T=k64        the 64-bit kernel only
  make test J=1          one after another (for debugging)
"""

import os
import subprocess
import sys
import time

DIRECTORY = os.path.join(os.path.dirname(os.path.abspath(__file__)), "tests")

# The order matters only with J=1; in parallel the longest ones start
# first anyway, so that they are not left alone at the end
GROUPS = [
    "boot", "bootsplash", "shell", "process", "fork", "lazymem", "signals", "protect",
    "net32", "http32", "mouse", "ush", "tfs", "foreigndisk", "k64core", "k64proc",
    "k64shell", "k64disk", "acpi", "usbkbd", "usbmouse", "ahci", "consoles", "swap",
    "tls32", "tls64", "net", "net64", "e1000", "virtio", "nvme", "xhci", "ehci", "ttf", "shapes", "sem", "disk",
    "swap64", "shm64", "mmap", "mmapwrite", "dynlink", "smp", "locks",
    "kthreads", "proccores", "lockless", "tlb", "faults", "config", "settings", "netbar", "winmgr", "manywin", "maxwin", "clock", "gterm", "terminal", "gtermlive", "gtermsh", "alttab", "clip", "gtermsel", "winkeys", "snap", "desks", "gedit", "gcode", "gtasks", "sysbar", "files", "filetree", "screenshot", "colours", "wheel", "scale", "look", "bar", "udraw", "motion", "notify", "appear", "apps", "lock", "session", "polish", "wall", "icons", "users", "usersgui", "login", "multiuser", "gfx64", "desktop1",
    "desktop2", "desktop64", "gui64", "gterm64", "acpi64", "consoles64",
    "usbkbd64", "iso", "uefi", "threadboot", "fat", "install", "ginstal", "wholedisk", "disks", "stick", "sound", "aes", "wpa", "usbdisk", "bt", "usbnet",
    "libc", "browser", "web", "webform", "webtools", "webjs", "isoinstall", "wc", "update"
]


def main():
    argv = sys.argv[1:]
    kernels = [a for a in argv if not a.startswith("-")]
    parallel = 4
    selected = GROUPS

    for a in argv:
        if a.startswith("-j"):
            parallel = max(1, int(a[2:] or 4))
        elif a.startswith("-t"):
            selected = [g for g in a[2:].split(",") if g]

    unknown = [g for g in selected if g not in GROUPS]
    if unknown:
        print(f"Unknown groups: {', '.join(unknown)}")
        print(f"Available: {', '.join(GROUPS)}")
        return 2

    start = time.time()
    waiting = list(selected)
    running = []          # (name, process, start time)
    results = []          # (name, code, seconds, text)

    while waiting or running:
        while waiting and len(running) < parallel:
            g = waiting.pop(0)
            p = subprocess.Popen(
                [sys.executable, "-u", os.path.join(DIRECTORY, f"t_{g}.py")]
                + kernels,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            running.append((g, p, time.time()))
            print(f">>> start: {g}", flush=True)

        for entry in list(running):
            g, p, t0 = entry
            if p.poll() is None:
                continue
            running.remove(entry)
            text = p.stdout.read().decode(errors="replace")
            results.append((g, p.returncode, time.time() - t0, text))
            print(f"\n=== {g} ({time.time() - t0:.0f} s) "
                  f"{'OK' if p.returncode == 0 else 'ERRORS'} ===")
            print(text.rstrip(), flush=True)
        if running:
            time.sleep(0.5)

    ok = sum(t.count("\nOK:   ") + t.startswith("OK:   ")
             for _, _, _, t in results)
    bad = sum(t.count("\nFAIL: ") + t.startswith("FAIL: ")
              for _, _, _, t in results)

    print("\n" + "=" * 60)
    for g, code, sec, _ in sorted(results, key=lambda w: -w[2]):
        print(f"  {g:12} {sec:5.0f} s   {'OK' if code == 0 else 'ERRORS'}")
    print(f"\nAssertions: {ok} passed, {bad} failed "
          f"({time.time() - start:.0f} s in all)")

    if bad or any(code for _, code, _, _ in results):
        print("FAILED")
        return 1
    print("ALL TESTS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
