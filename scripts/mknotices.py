#!/usr/bin/env python3
"""Builds LICENSES.txt: ThreadOS's own licence and the licence of every
third-party component in the tree, as the components' authors wrote them.

    mknotices.py <out>

Most of the libraries in third_party/ are linked into the programs of the
initrd, so every disc, stick image and update hands out their binaries -
and the MIT, BSD, Apache and zlib licences let that happen only with
their notices alongside, the LGPL only with its text and a note on where
the source is (docs/LICENSING.md). The texts are taken from the archives
themselves at build time rather than copied into the tree, so a library
updated to a release whose licence changed brings its new text with it.

The list of components is third_party/README.md's table - the record
every library must have anyway - and each of them needs an entry in
SOURCES below saying which files of it hold its licence. A component
without one, or an entry naming a file its archive no longer has, stops
the build: a library added without its licence must not ship quietly.
"""

import fnmatch
import hashlib
import os
import re
import sys
import tarfile
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TP = os.path.join(ROOT, "third_party")

# Where each component's licence lives. The key is the component's
# directory in the README table with its version taken off (stem());
# each source is (where, pattern, how):
#   where   - an archive or a directory under third_party/; a pattern
#             with * picks several archives (gstreamer/*.tar.xz);
#   pattern - a path inside it, below the archive's top directory;
#             fnmatch's * crosses '/', so Source/WTF/*LICENSE* finds
#             them all;
#   how     - "file" for the whole file, "comment" for the first
#             C comment of the file that names a copyright (licences
#             kept in a header rather than a file of their own),
#             "lines" for the comment lines a data file starts with,
#             "note" for a sentence of ours where the data itself says
#             nothing of its licence (the pattern is the sentence).
SOURCES = {
    "musl": [("musl", "COPYRIGHT", "file")],
    "quickjs": [("quickjs", "LICENSE", "file")],
    "litehtml": [("litehtml", "LICENSE", "file"),
                 ("litehtml", "src/gumbo/LICENSE", "file")],
    "ffmpeg": [("ffmpeg-*.tar.xz", "LICENSE.md", "file"),
               ("ffmpeg-*.tar.xz", "COPYING.LGPLv2.1", "file")],
    "zlib": [("zlib-*.tar.xz", "LICENSE", "file")],
    "curl": [("curl-*.tar.xz", "COPYING", "file")],
    "mbedtls": [("mbedtls-*.tar.bz2", "LICENSE", "file")],
    "cacert": [("cacert", "cacert.pem", "lines"),
               ("cacert", "Mozilla's certificate data is under the Mozilla "
                "Public License 2.0, https://mozilla.org/MPL/2.0/ - the "
                "system carries it unchanged, as curl.se publishes it.",
                "note")],
    "freetype": [("freetype-*.tar.xz", "LICENSE.TXT", "file"),
                 ("freetype-*.tar.xz", "docs/FTL.TXT", "file")],
    "harfbuzz": [("harfbuzz", "COPYING", "file")],
    "sqlite-amalgamation": [("sqlite-amalgamation-*.zip", "sqlite3.h",
                             "comment")],
    "acpica-unix": [("acpica-unix-*.tar.gz", "source/include/acpi.h",
                     "comment")],
    "webkit": [("webkit/wpewebkit-*.tar.xz", p, "file") for p in (
        "Source/JavaScriptCore/COPYING.LIB",
        "Source/JavaScriptCore/*LICENSE*",
        "Source/WTF/*LICENSE*",
        "Source/WTF/*COPYING*",
        "Source/bmalloc/*LICENSE*",
        "Source/WebCore/LICENSE-APPLE",
        "Source/WebCore/LICENSE-LGPL-2.1")],
    "icu": [("icu/icu4c-*-sources.tgz", "LICENSE", "file")],
    "glib": [("glib-[0-9]*.tar.xz", "COPYING", "file")],
    "libffi": [("libffi-*.tar.gz", "LICENSE", "file")],
    "pcre2": [("pcre2-*.tar.bz2", "LICENCE.md", "file")],
    "libpng": [("libpng-*.tar.xz", "LICENSE", "file")],
    "libjpeg-turbo": [("libjpeg-turbo-*.tar.gz", "LICENSE.md", "file"),
                      ("libjpeg-turbo-*.tar.gz", "README.ijg", "file")],
    "libxml2": [("libxml2-*.tar.xz", "Copyright", "file")],
    "fontconfig": [("fontconfig-*.tar.xz", "COPYING", "file")],
    "openssl": [("openssl-*.tar.gz", "LICENSE.txt", "file")],
    "nghttp2": [("nghttp2-*.tar.xz", "COPYING", "file")],
    "libpsl": [("libpsl-*.tar.gz", "COPYING", "file"),
               ("libpsl-*.tar.gz", "list/public_suffix_list.dat", "lines")],
    "libsoup": [("libsoup-*.tar.xz", "COPYING", "file")],
    "glib-networking": [("glib-networking-*.tar.xz", "COPYING", "file"),
                        ("glib-networking-*.tar.xz", "LICENSE_EXCEPTION",
                         "file")],
    "libwebp": [("libwebp-*.tar.gz", "COPYING", "file"),
                ("libwebp-*.tar.gz", "PATENTS", "file")],
    "libtasn1": [("libtasn1-*.tar.gz", "COPYING.LESSERv2", "file")],
    "libgpg-error": [("libgpg-error-*.tar.bz2", "COPYING.LIB", "file")],
    "libgcrypt": [("libgcrypt-*.tar.bz2", "COPYING.LIB", "file"),
                  ("libgcrypt-*.tar.bz2", "LICENSES", "file")],
    "libepoxy": [("libepoxy-*.tar.xz", "COPYING", "file")],
    "libxkbcommon": [("libxkbcommon-*.tar.gz", "LICENSE", "file")],
    "khronos": [("khronos", "KHR/khrplatform.h", "comment"),
                ("khronos", "EGL/egl.h", "comment")],
    "iwlwifi": [("iwlwifi", "LICENCE.iwlwifi_firmware", "file")],
    "gstreamer": [("gstreamer/*.tar.xz", "COPYING", "file")],
    "opus": [("opus/opus-*.tar.gz", "COPYING", "file")],
    "brotli-rfc7932": [("brotli-rfc7932", "README.md", "file")],
}

# Not under third_party/, so not in its table, but handed out all the same
EXTRA = [
    ("DejaVu fonts (fonts/sans.ttf, bold.ttf, mono.ttf)", "2.37",
     "Bitstream Vera licence and public domain changes",
     "https://dejavu-fonts.github.io/",
     [os.path.join(ROOT, "fonts", "LICENSE-DejaVu.txt")]),
]

SOURCE_NOTE = """\
Where the source is

ThreadOS is free software under the MIT licence below. The complete
source code of this release - ThreadOS's own code, the build system
that links the programs, and every third-party component listed here,
in the archive its authors published - is the "Source code" archive
on this release's page:

    https://github.com/DaniuPl/ThreadOS/releases

Several of the components (FFmpeg, GLib, libsoup, glib-networking,
GStreamer, libgcrypt, libgpg-error, libtasn1 and parts of WebKit) are
under the GNU Lesser General Public License. You may change them and
build the system again with your changed copies: `make` in that source
tree links every program anew against the libraries it builds there.
"""


def stem(directory):
    """'ffmpeg-9.0.tar.xz' -> 'ffmpeg', 'webkit/wpewebkit-...' -> 'webkit'"""
    directory = directory.strip("`").strip()
    if "/" in directory:
        return directory.split("/")[0]
    return re.split(r"-[0-9]", directory, maxsplit=1)[0].split(".")[0]


def table():
    """The rows of third_party/README.md's table, as dicts"""
    rows = []
    with open(os.path.join(TP, "README.md"), encoding="utf-8") as fh:
        for line in fh:
            if not line.startswith("| `"):
                continue
            cols = [c.strip() for c in line.strip().strip("|").split("|")]
            if len(cols) < 5:
                sys.exit("mknotices: a row of third_party/README.md has "
                         "fewer than five columns: " + line.strip())
            rows.append({"dir": cols[0].strip("`"), "what": cols[1],
                         "version": cols[2], "licence": cols[3],
                         "from": cols[4]})
    return rows


def first_comment(text):
    """The first C comment that names a copyright, or one that disclaims it"""
    for m in re.finditer(r"/\*.*?\*/", text, re.S):
        block = m.group(0)
        if re.search(r"copyright|disclaims", block, re.I):
            return block
    return None


def leading_lines(text):
    """The comment lines a data file starts with (## in the CA bundle,
    // in the public suffix list)"""
    out = []
    for line in text.splitlines():
        if line.startswith(("##", "//")) or (out and not line.strip()
                                              and len(out) < 3):
            out.append(line)
        elif out:
            break
    return "\n".join(out).strip() + "\n" if out else None


def shape(text, how, where):
    if how == "file":
        return text
    got = first_comment(text) if how == "comment" else leading_lines(text)
    if not got:
        sys.exit("mknotices: no licence comment in " + where)
    return got + "\n"


def from_archive(path, pattern):
    """(name, text) of the members matching the pattern, below the
    archive's top directory - in one pass, the archives being large"""
    found = []

    def want(name):
        inner = name.split("/", 1)[1] if "/" in name else ""
        return inner and fnmatch.fnmatch(inner, pattern)

    if path.endswith(".zip"):
        with zipfile.ZipFile(path) as z:
            for name in z.namelist():
                if want(name) and not name.endswith("/"):
                    found.append((name, z.read(name)))
    else:
        # A licence can be a link to the file that holds it (GLib's
        # COPYING points into LICENSES/): those are read in a second pass
        links = {}
        with tarfile.open(path, "r|*") as t:
            for m in t:
                if m.isfile() and want(m.name):
                    found.append((m.name, t.extractfile(m).read()))
                elif m.issym() and want(m.name):
                    target = os.path.normpath(os.path.join(
                        os.path.dirname(m.name), m.linkname))
                    links[target] = m.name
        if links:
            with tarfile.open(path, "r|*") as t:
                for m in t:
                    if m.isfile() and m.name in links:
                        found.append((links[m.name],
                                      t.extractfile(m).read()))
    return found


def gather(component, sources):
    texts = []
    for where, pattern, how in sources:
        if how == "note":
            texts.append(("note", pattern + "\n"))
            continue
        places = sorted(p for p in _glob(where))
        if not places:
            sys.exit("mknotices: %s: nothing in third_party/ matches %s"
                     % (component, where))
        for place in places:
            rel = os.path.relpath(place, ROOT)
            if os.path.isdir(place):
                found = []
                for dirpath, _, files in os.walk(place):
                    for f in files:
                        full = os.path.join(dirpath, f)
                        if fnmatch.fnmatch(os.path.relpath(full, place),
                                           pattern):
                            with open(full, "rb") as fh:
                                found.append((os.path.relpath(full, ROOT),
                                              fh.read()))
            else:
                found = [(rel + ": " + n, d)
                         for n, d in from_archive(place, pattern)]
            if not found:
                sys.exit("mknotices: %s: %s has no %s" % (component, rel,
                                                          pattern))
            for name, data in sorted(found):
                text = data.decode("utf-8", errors="replace")
                texts.append((name, shape(text, how, name)))
    return texts


def _glob(where):
    import glob
    return glob.glob(os.path.join(TP, where))


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = sys.argv[1]

    rows = table()
    missing = [r["dir"] for r in rows if stem(r["dir"]) not in SOURCES]
    if missing:
        sys.exit("mknotices: no licence source for %s - add it to SOURCES "
                 "in scripts/mknotices.py" % ", ".join(missing))
    stale = set(SOURCES) - {stem(r["dir"]) for r in rows}
    if stale:
        sys.exit("mknotices: SOURCES names %s, which third_party/README.md "
                 "does not list" % ", ".join(sorted(stale)))

    components = []
    for r in rows:
        components.append((r["what"], r["version"], r["licence"], r["from"],
                           gather(r["dir"], SOURCES[stem(r["dir"])])))
    for what, version, licence, url, files in EXTRA:
        texts = []
        for f in files:
            with open(f, encoding="utf-8", errors="replace") as fh:
                texts.append((os.path.relpath(f, ROOT), fh.read()))
        components.append((what, version, licence, url, texts))

    # The same text (the LGPL, Apache-2.0) is printed once and referred
    # to by number - GStreamer's five archives carry one LGPL between them
    numbered, order = {}, []

    def number(text):
        key = hashlib.sha256(text.encode()).hexdigest()
        if key not in numbered:
            numbered[key] = len(order) + 1
            order.append(text)
        return numbered[key]

    lines = []
    # The first line is what Settings -> About reads the count from
    lines.append("ThreadOS contains %d third-party components; "
                 "their licences follow." % len(components))
    lines.append("")
    lines.append(SOURCE_NOTE)
    lines.append("=" * 72)
    lines.append("ThreadOS")
    with open(os.path.join(ROOT, "LICENSE"), encoding="utf-8") as fh:
        lines.append("")
        lines.append(fh.read().rstrip())
    lines.append("")

    body = []
    for what, version, licence, url, texts in components:
        refs = []
        for name, text in texts:
            refs.append("[%d] %s" % (number(text), name))
        body.append("=" * 72)
        body.append(re.sub(r"`", "", what))
        body.append("Version: " + re.sub(r"`", "", version))
        body.append("Licence: " + re.sub(r"`", "", licence))
        body.append("Source:  " + re.sub(r"`", "", url))
        body.append("Texts:   " + ("\n         ".join(refs)))
        body.append("")
    lines += body

    for i, text in enumerate(order, 1):
        lines.append("=" * 72)
        lines.append("[%d]" % i)
        lines.append("")
        lines.append(text.rstrip())
        lines.append("")

    data = "\n".join(lines) + "\n"
    tmp = out + ".tmp"
    with open(tmp, "w", encoding="utf-8") as fh:
        fh.write(data)
    os.replace(tmp, out)


if __name__ == "__main__":
    main()
