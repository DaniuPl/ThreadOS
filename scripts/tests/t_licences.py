#!/usr/bin/env python3
"""ThreadOS smoke test: the licences the system hands out.

Most of third_party/ is linked into programs of the initrd, so every
disc and update is a binary distribution of FFmpeg, GLib, WebKit and
thirty-odd more - and their licences let that happen only with their
texts alongside (the LGPL also asks for a note on where the source is).
scripts/mknotices.py builds LICENSES.txt for that; this group holds it
to the libraries themselves, not to the script's own idea of them:

  every licence file third_party/README.md names for a component, read
  out of its archive by GNU tar (not by Python's tarfile, which the
  script uses), is in the initrd's LICENSES.txt word for word - so a
  library added, or updated to a release whose licence changed, cannot
  ship without its text;

  the first line counts the components, and the count is the README
  table's rows and the fonts - Settings -> About shows that number
  (t_settings), so it has to be right;

  the booted kernel has the file in its initrd, at the size the build
  made it - the system that runs is the one that carries the texts.

Run on its own or through scripts/run_tests.py.
"""

import os
import re
import struct
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, make_iso64, no64

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))
TP = os.path.join(ROOT, "third_party")


def initrd_file(name):
    """A file of the initrd, straight from the image (the TIRD format of
    scripts/mkinitrd.py)"""
    with open(INITRD, "rb") as fh:
        img = fh.read()
    if img[:4] != b"TIRD":
        return None
    count = struct.unpack_from("<I", img, 4)[0]
    for k in range(count):
        n, off, size = struct.unpack_from("<16sII", img, 8 + 24 * k)
        if n.rstrip(b"\0") == name.encode():
            return img[off:off + size]
    return None


def readme_rows():
    rows = []
    with open(os.path.join(TP, "README.md"), encoding="utf-8") as fh:
        for line in fh:
            if line.startswith("| `"):
                cols = line.strip().strip("|").split("|")
                rows.append((cols[0].strip().strip("`"), cols[3]))
    return rows


def named_files(licence_cell):
    """The file names a README row's Licence column puts in backticks
    and that look like licence files"""
    names = re.findall(r"`([^`]+)`", licence_cell)
    return [n for n in names
            if re.search(r"(LICEN[CS]E|COPYING|COPYRIGHT|Copyright|PATENTS"
                         r"|README\.ijg|FTL\.TXT|README\.md)", n)]


def gnu_tar_member(archive, inner):
    """'<top>/<inner>' of an archive, through GNU tar - followed when it
    is a link (GLib's COPYING points into LICENSES/)"""
    listing = subprocess.run(["tar", "-tvf", archive], capture_output=True,
                             text=True).stdout.split("\n")
    for line in listing:
        m = re.search(r" (\S+?)(?: -> (\S+))?$", line)
        if not m or "/" not in m.group(1):
            continue
        name, target = m.group(1), m.group(2)
        if name.split("/", 1)[1] != inner:
            continue
        if target:
            name = os.path.normpath(os.path.join(os.path.dirname(name),
                                                 target))
        got = subprocess.run(["tar", "-xOf", archive, name],
                             capture_output=True)
        if got.returncode == 0:
            return got.stdout
    return None


def texts_of(directory, inner):
    """Every copy of a licence file a component has: in its directory,
    or at the top of each archive it is made of"""
    where = os.path.join(TP, directory.rstrip("/"))
    out = []
    if os.path.isdir(where):
        if os.path.isfile(os.path.join(where, inner)):
            with open(os.path.join(where, inner), "rb") as fh:
                out.append((inner, fh.read()))
        for a in sorted(os.listdir(where)):
            if re.search(r"\.tar\.(xz|gz|bz2)$|\.tgz$", a):
                got = gnu_tar_member(os.path.join(where, a), inner)
                if got is not None:
                    out.append((a + ": " + inner, got))
    elif re.search(r"\.tar\.(xz|gz|bz2)$|\.tgz$", where):
        got = gnu_tar_member(where, inner)
        if got is not None:
            out.append((directory + ": " + inner, got))
    return out


def main():
    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return summary()

    data = initrd_file("LICENSES.txt")
    check("the initrd carries LICENSES.txt", data is not None)
    if data is None:
        return summary()
    text = data.decode("utf-8", errors="replace")

    rows = readme_rows()
    m = re.match(r"ThreadOS contains (\d+) third-party components", text)
    check("its first line counts the README table's %d components and the "
          "fonts (%s)" % (len(rows), m.group(1) if m else "no count"),
          m is not None and int(m.group(1)) == len(rows) + 1)

    # Every licence file the README names, out of the archive by GNU tar
    found, missing = 0, []
    for directory, licence in rows:
        for inner in named_files(licence):
            for where, body in texts_of(directory, inner):
                body = body.decode("utf-8", errors="replace").rstrip()
                if body and body in text:
                    found += 1
                else:
                    missing.append(where)
    check("every licence file the README names is in it word for word "
          "(%d found%s)" % (found, ", missing: " + ", ".join(missing)
                            if missing else ""),
          not missing and found >= len(rows))

    # The texts the binaries' own licences ask for by name
    for what, needle in (
            ("ThreadOS's own MIT licence",
             open(os.path.join(ROOT, "LICENSE"), encoding="utf-8")
             .read().rstrip()),
            ("the GNU LGPL 2.1, for FFmpeg, GLib and the rest",
             "GNU LESSER GENERAL PUBLIC LICENSE\n"
             "                       Version 2.1, February 1999"),
            ("the DejaVu fonts' licence",
             open(os.path.join(ROOT, "fonts", "LICENSE-DejaVu.txt"),
                  encoding="utf-8", errors="replace").read().rstrip()),
            ("where the source of the release is",
             "https://github.com/DaniuPl/ThreadOS/releases")):
        check("it holds %s" % what, needle in text)

    # The kernel that boots has it, at the size the build made it
    d = tempfile.mkdtemp()
    iso = tempfile.mktemp(suffix=".iso")
    slog = tempfile.mktemp(suffix=".log")
    try:
        check("the disc with the 64-bit kernel builds", make_iso64(d, iso))
        if not os.path.exists(iso):
            return summary()
        q = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog,
                 qemu="qemu-system-x86_64")
        try:
            q.wait_serial("thread64>", timeout=90)
            q.type_line("ls")
            # The line can be half written when the name shows: look
            # again until its size has come too
            got = None
            deadline = time.time() + 30
            while got is None and time.time() < deadline:
                got = re.search(r"LICENSES\.txt  \((\d+) B\)", q.serial())
                if got is None:
                    time.sleep(0.3)
            check("the booted kernel lists LICENSES.txt in its initrd, "
                  "%s B as built (%d B)"
                  % (got.group(1) if got else "?", len(data)),
                  got is not None and int(got.group(1)) == len(data))
        finally:
            q.close()
    finally:
        for f in (iso, slog):
            if os.path.exists(f):
                os.remove(f)
        subprocess.run(["rm", "-rf", d])
    return summary()


if __name__ == "__main__":
    sys.exit(main())
