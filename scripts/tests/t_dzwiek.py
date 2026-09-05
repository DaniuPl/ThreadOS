#!/usr/bin/env python3
"""ProtonOS smoke test: sound (Intel HD Audio).

The controller every machine made after about 2005 has. What makes it
worth a test group of its own is that "it played" is not something a
system can be asked: a driver that sets every register and produces
silence looks exactly like one that works.

So QEMU is told to write its sound to a file, and the file is read
here - the peak, how long the loud part lasts and the distance between
its zero crossings. A tone of 440 Hz for 200 ms has to come out as 440
Hz for 200 ms, and a tone of 880 for 100 as that; one number would be
a coincidence, two are a driver.

Run on its own or through scripts/run_tests.py.
"""

import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, no64, make_iso64, wav_tone


def main():
    print()
    print("--- dzwiek (Intel HDA) ---")

    reason = no64()
    if reason:
        print("POMINIETE: " + reason)
        return summary()

    d = tempfile.mkdtemp()
    iso = tempfile.mktemp(suffix=".iso")
    wav = tempfile.mktemp(suffix=".wav")
    slog = tempfile.mktemp(suffix=".log")
    slog2 = tempfile.mktemp(suffix=".log")

    try:
        check("plyta z jadrem 64-bitowym buduje sie",
              make_iso64(d, iso, text=True))
        if not os.path.exists(iso):
            return summary()

        q = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog, audio=wav,
                 qemu="qemu-system-x86_64")
        try:
            out = q.wait_serial("System 64-bitowy gotowy", timeout=90)
            check("system znajduje kontroler i kodek",
                  "Dzwiek: Intel HDA" in out)
            check("i mowi, ktoredy idzie dzwiek",
                  "wyjscie" in out and "gniazdo" in out)

            # --- the first tone
            before = os.path.getsize(wav) - 44 if os.path.exists(wav) else 0
            out = q.command("beep 440 200", "beep:", timeout=30)
            check("'beep' nie odmawia", "nie ma dzwieku" not in out)
            time.sleep(2)
            tone = wav_tone(wav, max(before, 0))
            check("cokolwiek wyszlo z karty dzwiekowej",
                  tone is not None)
            if tone:
                peak, ms, hz = tone
                check("i jest to 440 Hz (%.0f Hz)" % hz, 420 <= hz <= 460)
                check("przez 200 ms (%.0f ms)" % ms, 150 <= ms <= 260)
                check("dostatecznie glosno, zeby to byl dzwiek, a nie szum "
                      "(szczyt %d)" % peak, peak > 1000)

            # --- and a different one, because one number is a coincidence
            before = os.path.getsize(wav) - 44
            q.command("beep 880 100", "beep:", timeout=30)
            time.sleep(2)
            tone = wav_tone(wav, max(before, 0))
            check("drugi ton tez wychodzi", tone is not None)
            if tone:
                peak, ms, hz = tone
                check("i jest o oktawe wyzej (%.0f Hz)" % hz,
                      850 <= hz <= 910)
                check("i krotszy (%.0f ms)" % ms, 60 <= ms <= 160)

            out = q.command("beep 3 100", "beep:", timeout=20)
            check("czestotliwosc, ktorej nie da sie uslyszec, jest odrzucona",
                  "nie jest dzwiek" in out)
        finally:
            q.close()

        # --- and a machine with no sound card, which has to say so
        q2 = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog2,
                  qemu="qemu-system-x86_64")
        try:
            out = q2.wait_serial("System 64-bitowy gotowy", timeout=90)
            check("maszyna bez karty mowi to wprost",
                  "nie ma kontrolera HDA na PCI" in out)
            out = q2.command("beep", "beep:", timeout=20)
            check("a 'beep' na niej odmawia, zamiast udawac",
                  "nie ma dzwieku" in out)
        finally:
            q2.close()
    finally:
        shutil.rmtree(d, ignore_errors=True)
        for f in (iso, wav, slog, slog2):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
