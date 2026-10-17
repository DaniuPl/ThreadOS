#!/usr/bin/env python3
"""ThreadOS smoke test: sound (Intel HD Audio).

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
    print("--- sound (Intel HDA) ---")

    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return summary()

    d = tempfile.mkdtemp()
    iso = tempfile.mktemp(suffix=".iso")
    wav = tempfile.mktemp(suffix=".wav")
    slog = tempfile.mktemp(suffix=".log")
    slog2 = tempfile.mktemp(suffix=".log")
    slog3 = tempfile.mktemp(suffix=".log")

    try:
        check("the disc with the 64-bit kernel builds",
              make_iso64(d, iso, text=True))
        if not os.path.exists(iso):
            return summary()

        q = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog, audio=wav,
                 qemu="qemu-system-x86_64")
        try:
            out = q.wait_serial("The 64-bit system is ready", timeout=90)
            check("the system finds the controller and the codec",
                  "Sound: Intel HDA" in out)
            check("and it says which way the sound goes",
                  "output" in out and "socket" in out)

            # --- the first tone
            before = os.path.getsize(wav) - 44 if os.path.exists(wav) else 0
            out = q.command("beep 440 200", "beep:", timeout=30)
            check("'beep' does not refuse", "there is no sound" not in out)
            time.sleep(2)
            tone = wav_tone(wav, max(before, 0))
            check("something came out of the sound card",
                  tone is not None)
            if tone:
                peak, ms, hz = tone
                check("and it is 440 Hz (%.0f Hz)" % hz, 420 <= hz <= 460)
                check("for 200 ms (%.0f ms)" % ms, 150 <= ms <= 260)
                check("loud enough to be a sound and not noise "
                      "(peak %d)" % peak, peak > 1000)

            # --- and a different one, because one number is a coincidence
            before = os.path.getsize(wav) - 44
            q.command("beep 880 100", "beep:", timeout=30)
            time.sleep(2)
            tone = wav_tone(wav, max(before, 0))
            check("the second tone comes out too", tone is not None)
            if tone:
                peak, ms, hz = tone
                check("and it is an octave higher (%.0f Hz)" % hz,
                      850 <= hz <= 910)
                check("and shorter (%.0f ms)" % ms, 60 <= ms <= 160)

            out = q.command("beep 3 100", "beep:", timeout=20)
            check("a frequency nobody can hear is refused",
                  "is not a sound" in out)

            # --- and how loud. The codec's amplifier has as many steps
            # as it has, so the percentage is of that; what a test can
            # say for certain is that louder is louder, quieter is
            # quieter and silence is silent.
            out = q.command("volume", "volume:", timeout=20)
            check("the system says how loud it is and what it computes that from",
                  "%" in out and "steps in the codec" in out)

            def loud_after(command):
                at = os.path.getsize(wav) - 44
                q.command(command, "volume:", timeout=20)
                q.command("beep 440 150", "beep:", timeout=30)
                time.sleep(2)
                t = wav_tone(wav, max(at, 0))
                return t[0] if t else 0

            full = loud_after("volume 100")
            check("at 100%% something comes out loud (peak %d)" % full,
                  full > 3000)
            third = loud_after("volume 30")
            check("at 30%% quieter than at 100%% (%d against %d)" % (third, full),
                  0 < third < full)
            low = loud_after("volume 5")
            check("at 5%% quieter still (%d)" % low, low < third)
            quiet = loud_after("volume mute")
            check("and after 'mute' nothing comes out (%d)" % quiet, quiet == 0)
            out = q.command("volume mute", "volume:", timeout=20)
            check("and a second 'mute' turns it back on",
                  "audible again" in out)
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
                q3.wait_serial("The 64-bit system is ready", timeout=120)
                q3.type_line("desktop")
                q3.wait_serial("The desktop is starting", timeout=40)
                time.sleep(4)
                out = q3.command("run gmikser", "from initrd", timeout=30)
                check("the mixer starts as a ring 3 program",
                      "'gmikser' from initrd" in out)
                time.sleep(3)
                q3.mouse_goto(700, 500)
                time.sleep(0.8)

                # The window: slot two, content a pixel in and a title
                # bar down (kernel/desktop.c)
                CX, CY = 95, 54 + CONTENT_DY
                dark = theme("dark")
                inks = [dark[k] for k in ("app_text", "app_dim", "accent",
                                          "app_danger")]

                def lines(n):
                    """The window's first n rows of text (user/gmikser.c:
                    UG_ROW apart from six pixels down), in the lettering
                    the program writes in"""
                    return q3.read_ui_lines(
                        CX + 2, CX + 356,
                        [CY + ui_font_baseline_in(UD_PX, 6 + r * UG_ROW, UG_ROW)
                         for r in range(n)], px=UD_PX, colours=inks)
                text = lines(4)
                check("the window says how loud it is",
                      "Volume:" in text and "%" in text)
                check("and what it computes the percentage from",
                      "steps" in text and "decibel" in text)

                BAR_X, BAR_Y, BAR_H = 12, 86, 18
                BTN_X, BTN_Y, BTN_W, BTN_H = 12, 116, 96, 24
                bar_w = 360 - 2 * BAR_X

                def click(x, y):
                    q3.mouse_goto(CX + x, CY + y)
                    q3.mouse_click()
                    time.sleep(0.8)
                    q3.mouse_goto(700, 500)
                    time.sleep(0.5)

                def test_tone():
                    at = os.path.getsize(wav2) - 44
                    q3.mouse_goto(CX + BTN_X + BTN_W + 10 + BTN_W // 2,
                                  CY + BTN_Y + BTN_H // 2)
                    q3.mouse_click()
                    time.sleep(3)
                    q3.mouse_goto(700, 500)
                    t = wav_tone(wav2, max(at, 0))
                    return t[0] if t else 0

                click(BAR_X + bar_w - 1, BAR_Y + BAR_H // 2)
                text = lines(1)
                check("a click at the end of the slider gives 100%% ('%s')"
                      % text.strip(), "100%" in text)
                loud = test_tone()
                check("and the 'Test' button really plays (peak %d)" % loud,
                      loud > 3000)

                # The knob is BAR_H across and its middle stands on the
                # value, so the values run from half a knob in at one end
                # to half a knob in at the other (user/ugui.h)
                click(BAR_X + BAR_H // 2 + (bar_w - BAR_H) // 5,
                      BAR_Y + BAR_H // 2)
                text = lines(1)
                check("a click a fifth of the way along the slider gives about 20%% ('%s')"
                      % text.strip(),
                      "19%" in text or "20%" in text or "21%" in text)
                soft = test_tone()
                check("and then it plays quieter (%d against %d)" % (soft, loud),
                      0 < soft < loud)

                click(BTN_X + BTN_W // 2, BTN_Y + BTN_H // 2)
                text = lines(1)
                check("the 'Mute' button says it is muted ('%s')"
                      % text.strip(), "muted" in text)
                check("and then it plays nothing", test_tone() == 0)
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
            out = q2.wait_serial("The 64-bit system is ready", timeout=90)
            check("a machine with no card says so outright",
                  "there is no HDA controller on the PCI bus" in out)
            out = q2.command("beep", "beep:", timeout=20)
            check("and 'beep' on it refuses instead of pretending",
                  "there is no sound" in out)
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
