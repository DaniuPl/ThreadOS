#!/usr/bin/env python3
"""ThreadOS smoke test: fontconfig and FreeType.

How a web engine finds a font for the family a page names, and reads
it: fontconfig on the FreeType built with it (Makefile, 'Libraries for
WebKit's processes'), with libxml2 reading its configuration.
libc/tests/fctest.c is given a directory with the system's three fonts
(fonts/) and nothing of a system's configuration; it is built a second
time on this machine against this machine's fontconfig and FreeType,
given the same three files, and the two have to say the same:

  the directory scanned - on ThreadOS through opendir() and readdir(),
  which musl's overlay lists through the kernel - and of every font its
  family, style, weight, slant, spacing, character count and whether
  it covers Polish and English

  six patterns matched (a family by name, bold, monospace, a family not
  there, a spacing, a weight and slant) and two sorted

  FreeType's glyph count, units per em, ascender and descender of each
  font, and for a Polish sentence its glyphs, outlines and advances in
  font units

And the system's own fonts, as a program that asks fontconfig for no
configuration in particular finds them (fctest --system): the C library
makes the initrd's fonts files of /usr/share/fonts/threados and writes
/etc/fonts/fonts.conf (libc/overlay/src/threados/socket.c), which names
them for sans-serif, serif and monospace and makes the bold file the
bold of the sans family. The host's fontconfig is given that same
configuration, pointed at copies of the same fonts, and the two have to
list the same fonts and match the same families to the same files - a
WebKit page asking for sans-serif bold gets the bold font.

Run on its own or through scripts/run_tests.py.
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import ptest                 # noqa: E402
from ptest import *          # noqa: F401,F403,E402
from ptest import check, summary  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FONTS = ["sans.ttf", "bold.ttf", "mono.ttf"]
PROMPTS = ("thread64>", "thread64#")


def host_fctest(work):
    """fctest against this machine's fontconfig and FreeType, on copies
    of the same fonts: its lines, or None without them"""
    flags = subprocess.run(["pkg-config", "--cflags", "--libs", "fontconfig",
                            "freetype2"], capture_output=True, text=True)
    if flags.returncode:
        return None
    exe = os.path.join(work, "fctest")
    if subprocess.run(["gcc", "-O2", "-w", "-o", exe,
                       os.path.join(ROOT, "libc", "tests", "fctest.c"),
                       *flags.stdout.split()]).returncode:
        return None
    os.makedirs(os.path.join(work, "fonts"))
    for f in FONTS:
        shutil.copy(os.path.join(ROOT, "fonts", f),
                    os.path.join(work, "fonts", f))
    p = subprocess.run([exe, "fonts"], cwd=work, capture_output=True,
                       text=True, timeout=60)
    return [l for l in p.stdout.splitlines() if l and not l.startswith("~")]


def fonts_conf(directory):
    """The configuration the C library makes up, with the fonts'
    directory as given: read out of socket.c, so the two cannot part"""
    with open(os.path.join(ROOT, "libc", "overlay", "src", "threados",
                           "socket.c")) as fh:
        src = fh.read()
    body = src.split("static const char fonts_conf[] =", 1)[1].split(";", 1)[0]
    parts = re.findall(r'"((?:[^"\\]|\\.)*)"|\b(FONT_DIR)\b', body)
    return "".join(directory if macro else
                   lit.encode().decode("unicode_escape")
                   for lit, macro in parts)


def host_system(work):
    """fctest --system on this machine, given the same configuration
    with the fonts in work/fonts: its lines, or None"""
    exe = os.path.join(work, "fctest")
    if not os.path.exists(exe):
        return None
    conf = os.path.join(work, "fonts.conf")
    with open(conf, "w") as fh:
        fh.write(fonts_conf(os.path.join(work, "fonts")))
    env = dict(os.environ, XDG_CACHE_HOME=os.path.join(work, "cache"))
    p = subprocess.run([exe, "--system", conf], cwd=work, env=env,
                       capture_output=True, text=True, timeout=60)
    return [l for l in p.stdout.splitlines() if l]


def run(q, command, timeout=120):
    seen = len(q.serial())
    q.type_line(command)
    name = command.split()[1]
    end = time.time() + timeout
    while time.time() < end and f"[{name}] done, exit code" not in \
            q.serial()[seen:]:
        time.sleep(0.5)
    out = q.serial()[seen:].encode("latin1").decode("utf-8", "replace")
    start = out.find(f"Program '{name}' from initrd")
    out = out[start:] if start >= 0 else out
    m = re.search(r"\[%s\] done, exit code (-?\d+)" % re.escape(name), out)
    # fontconfig says on stderr that it has nowhere to write a cache,
    # which is so, and lists the places it tried a line each, indented:
    # that is not the program's output
    body = [l.rstrip() for l in out.replace("\r", "").split("\n")[1:]
            if l.strip() and not l.startswith("\t") and
            not l.strip().startswith(("[", "Fontconfig ") + PROMPTS)]
    return body, int(m.group(1)) if m else None


def main():
    print("--- fontconfig and FreeType ---")
    work = tempfile.mkdtemp()
    try:
        want = host_fctest(work)
        want_system = host_system(work) if want is not None else None
    finally:
        shutil.rmtree(work, ignore_errors=True)
    if want is None:
        print("SKIPPED: no fontconfig and FreeType on this machine "
              "(libfontconfig-dev) to compare fctest with")
    for name in FONTS:
        with open(os.path.join(ROOT, "fonts", name), "rb") as fh:
            ptest.SERVED["/" + name] = fh.read()
    work = tempfile.mkdtemp()
    disk = os.path.join(work, "disk.img")
    with open(disk, "wb") as fh:
        fh.truncate(16 * 1024 * 1024)
    port, stop_http = start_http_server()
    try:
        q = Qemu(KERNEL, INITRD, disk, serial=os.path.join(work, "s.log"))
        try:
            check("the desktop came up", q.wait_desktop())
            q.click_desktop()
            q.command("mkdir fonts", PROMPTS[0], timeout=20)
            fetched = 0
            for name in FONTS:
                _, c = run(q, f"run httpget 10.0.2.2 {port} /{name} "
                              f"fonts/{name}")
                fetched += c == 0
            check(f"the fonts reach a directory on the disk ({fetched} of "
                  f"{len(FONTS)})", fetched == len(FONTS))
            got, code = run(q, "run fctest fonts")
            plain = [l for l in got if not l.startswith("~")]
            check(f"fctest scanned the directory and found the three fonts "
                  f"({plain[:1]})", plain[:1] == ["fonts in the directory: 3"])
            if want is not None:
                first = next((i for i, (w, g) in enumerate(zip(want, plain))
                              if w != g), None)
                where = (f"first difference at line {first + 1}: host "
                         f"{want[first]!r}, ThreadOS {plain[first]!r}"
                         if first is not None else
                         f"host {len(want)} lines, ThreadOS {len(plain)}")
                check(f"fctest says what it says against this machine's "
                      f"fontconfig and FreeType ({where})", plain == want)
            check(f"fctest ends with code 0 (got {code})", code == 0)
            for l in got:
                if l.startswith("~"):
                    print("   " + l)

            got, code = run(q, "run fctest --system")
            check(f"the system's configuration lists the initrd's three "
                  f"fonts ({got[:1]})", got[:1] == ["the system's fonts: 3"])
            bold = [l for l in got if l.startswith("'sans\\-serif:bold'")]
            check(f"a page asking for sans-serif bold gets the bold font "
                  f"({bold})", len(bold) == 1 and
                  " bold.ttf ('DejaVu Sans ThreadOS', 'Bold')" in bold[0])
            mono = [l for l in got if l.startswith("'monospace'")]
            sans = [l for l in got if l.startswith("'sans\\-serif'")]
            check(f"for monospace the monospaced one ({mono}), and for "
                  f"sans-serif the sans ({sans})",
                  len(mono) == 1 and " mono.ttf " in mono[0] and
                  len(sans) == 1 and " sans.ttf " in sans[0])
            other = [l for l in got if l.startswith("'Helvetica'")]
            check(f"a family the system does not have gets the sans "
                  f"({other})", len(other) == 1 and " sans.ttf " in other[0])
            if want_system is not None:
                first = next((i for i, (w, g) in
                              enumerate(zip(want_system, got)) if w != g), None)
                where = (f"first difference at line {first + 1}: host "
                         f"{want_system[first]!r}, ThreadOS {got[first]!r}"
                         if first is not None else
                         f"host {len(want_system)} lines, ThreadOS {len(got)}")
                check("the system's fonts and families are what this "
                      "machine's fontconfig makes of the same configuration "
                      f"({where})", got == want_system)
            check(f"fctest --system ends with code 0 (got {code})", code == 0)
        finally:
            q.close()
    finally:
        stop_http()
        shutil.rmtree(work, ignore_errors=True)
    return summary()


sys.exit(main())
