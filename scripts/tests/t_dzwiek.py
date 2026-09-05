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
    slog3 = tempfile.mktemp(suffix=".log")

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

            # --- and how loud. The codec's amplifier has as many steps
            # as it has, so the percentage is of that; what a test can
            # say for certain is that louder is louder, quieter is
            # quieter and silence is silent.
            out = q.command("glosnosc", "glosnosc:", timeout=20)
            check("system mowi, jak glosno jest i z czego to liczy",
                  "%" in out and "stopni w kodeku" in out)

            def loud_after(command):
                at = os.path.getsize(wav) - 44
                q.command(command, "glosnosc:", timeout=20)
                q.command("beep 440 150", "beep:", timeout=30)
                time.sleep(2)
                t = wav_tone(wav, max(at, 0))
                return t[0] if t else 0

            full = loud_after("glosnosc 100")
            check("na 100%% cos wychodzi glosno (szczyt %d)" % full,
                  full > 3000)
            third = loud_after("glosnosc 30")
            check("na 30%% ciszej niz na 100%% (%d wobec %d)" % (third, full),
                  0 < third < full)
            low = loud_after("glosnosc 5")
            check("na 5%% jeszcze ciszej (%d)" % low, low < third)
            quiet = loud_after("glosnosc cisza")
            check("a po 'cisza' nie wychodzi nic (%d)" % quiet, quiet == 0)
            out = q.command("glosnosc cisza", "glosnosc:", timeout=20)
            check("i drugie 'cisza' wlacza z powrotem",
                  "znowu slychac" in out)
        finally:
            q.close()

        # --- the same knob in a window, which is where a person
        # actually reaches for it: dragged while listening rather than
        # typed and heard a second later
        wav2 = tempfile.mktemp(suffix=".wav")
        d2 = tempfile.mkdtemp()
        iso2 = tempfile.mktemp(suffix=".iso")
        try:
            make_iso64(d2, iso2, graphics=True)
            q3 = Qemu(KERNEL, INITRD, cdrom=iso2, serial=slog3, audio=wav2,
                      qemu="qemu-system-x86_64")
            try:
                q3.wait_serial("System 64-bitowy gotowy", timeout=120)
                q3.type_line("pulpit")
                q3.wait_serial("Pulpit startuje", timeout=40)
                time.sleep(4)
                out = q3.command("run gmikser", "z initrd", timeout=30)
                check("mikser startuje jako program w pierscieniu 3",
                      "'gmikser' z initrd" in out)
                time.sleep(3)
                q3.mouse_goto(700, 500)
                time.sleep(0.8)

                # The window: slot two, content a pixel in and a title
                # bar down (kernel/desktop.c)
                CX, CY = 95, 65
                text = q3.read_text(CX + 6, CY + 4, 44, 4, line_h=12)
                check("okno mowi, jak glosno jest",
                      "Glosnosc:" in text and "%" in text)
                check("i z czego liczy procenty",
                      "stopni" in text and "decybele" in text)

                BAR_X, BAR_Y, BAR_H = 8, 52, 16
                BTN_X, BTN_Y, BTN_W, BTN_H = 8, 82, 84, 16
                bar_w = 360 - 2 * BAR_X

                def click(x, y):
                    q3.mouse_goto(CX + x, CY + y)
                    q3.mouse_click()
                    time.sleep(0.8)
                    q3.mouse_goto(700, 500)
                    time.sleep(0.5)

                def test_tone():
                    at = os.path.getsize(wav2) - 44
                    q3.mouse_goto(CX + BTN_X + BTN_W + 10 + 20, CY + BTN_Y + 8)
                    q3.mouse_click()
                    time.sleep(3)
                    q3.mouse_goto(700, 500)
                    t = wav_tone(wav2, max(at, 0))
                    return t[0] if t else 0

                click(BAR_X + bar_w - 1, BAR_Y + BAR_H // 2)
                text = q3.read_text(CX + 6, CY + 4, 44, 1, line_h=12)
                check("klik na koncu suwaka daje 100%% ('%s')"
                      % text.strip(), "100%" in text)
                loud = test_tone()
                check("i przycisk 'Sprawdz' naprawde gra (szczyt %d)" % loud,
                      loud > 3000)

                click(BAR_X + bar_w // 5, BAR_Y + BAR_H // 2)
                text = q3.read_text(CX + 6, CY + 4, 44, 1, line_h=12)
                check("klik w piatej czesci suwaka daje okolo 20%% ('%s')"
                      % text.strip(),
                      "19%" in text or "20%" in text or "21%" in text)
                soft = test_tone()
                check("i wtedy gra ciszej (%d wobec %d)" % (soft, loud),
                      0 < soft < loud)

                click(BTN_X + BTN_W // 2, BTN_Y + BTN_H // 2)
                text = q3.read_text(CX + 6, CY + 4, 44, 1, line_h=12)
                check("przycisk 'Cisza' mowi, ze jest cisza ('%s')"
                      % text.strip(), "cisza" in text)
                check("i wtedy nie gra nic", test_tone() == 0)
            finally:
                q3.close()
        finally:
            shutil.rmtree(d2, ignore_errors=True)
            for f in (iso2, wav2):
                if os.path.exists(f):
                    os.unlink(f)

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
        for f in (iso, wav, slog, slog2, slog3):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
