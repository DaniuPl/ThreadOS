"""The shared part of the ThreadOS smoke test.

Everything needed to start the machine and check something on it: the
Qemu class, the HTTP and DNS servers for the network tests, certificate
generation and the check function itself.

The checks themselves live in scripts/tests/ - every group in its own
file, because every one needs its own machine, and those start in
parallel.
"""

import base64
import http.server
import json
import os
import re
import shutil
import socket
import socketserver
import struct
import subprocess
import threading
import zlib
import sys
import tempfile
import time

KERNEL = sys.argv[1] if len(sys.argv) > 1 else "build/threados.bin"
INITRD = sys.argv[2] if len(sys.argv) > 2 else None
KERNEL64 = sys.argv[3] if len(sys.argv) > 3 else None
INITRD64 = sys.argv[4] if len(sys.argv) > 4 else None
VGA_ADDR = 0xB8000
VGA_BYTES = 80 * 25 * 2
VIDEO_W = 320
VIDEO_H = 200

# The geometry of the desktop menu (kernel/desktop.c): the entries grow
# upwards from the taskbar, so every new program shifts the previous
# ones. We keep it in one place, so that adding an application does not
# break the click tests.
# The system starts straight into the desktop; the console tests force
# text mode with a kernel command line.
TEXT_MODE = "nodesktop"

# The disk image under TFS3 (the kernel formats an empty disk on the
# first start)
DISK_SIZE = 16 * 1024 * 1024

# The guest's clock: fixed, but later than the issue date of the
# certificates generated during the test
RTC_BASE = "2030-06-13T12:34:56"
RTC_DATE = "2030-06-13"

def _menu_items():
    """The launch menu's entries, straight out of kernel/desktop.c"""
    full = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..",
                        "kernel", "desktop.c")
    with open(full) as fh:
        m = re.search(r"menu_items\[\] = \{([^}]*)\}", fh.read())
    return re.findall(r'"([^"]+)"', m.group(1))


MENU_ITEMS = _menu_items()
MENU_X0 = 6                 # the menu opened from the bar hangs here


def menu_y(name, top=None):
    """The middle of the menu entry with the given name, in the menu
    opened from the mark on the bar - or one standing at 'top'."""
    idx = MENU_ITEMS.index(name)
    t = MENU_Y if top is None else top
    return t + MENU_PAD + idx * MENU_ROW + MENU_ROW // 2


def _session_items():
    """The menu's system part, under the line: what is done to the
    session and the machine (kernel/desktop.c)"""
    full = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..",
                        "kernel", "desktop.c")
    with open(full) as fh:
        m = re.search(r"session_items\[SESS_N\] = \{([^}]*)\}", fh.read())
    return re.findall(r'"([^"]+)"', m.group(1))


SESSION_ITEMS = _session_items()


def session_y(name, top=None):
    """The middle of one of the system entries, below the programs and
    the line between them"""
    idx = SESSION_ITEMS.index(name)
    t = MENU_Y if top is None else top
    return (t + MENU_PAD + len(MENU_ITEMS) * MENU_ROW + MENU_SEP +
            idx * MENU_ROW + MENU_ROW // 2)


PAGE = b"<html>ThreadOS TCP works</html>\n"

# Files a test wants the guest to be able to download: path -> bytes.
# Filled in BEFORE start_http_server() is called - the handler reads
# this dictionary on every request, so a test that has something of its
# own to hand over (a ciphertext prepared on the host, say) does not
# have to add a branch to the server.
SERVED = {}
# A page with a link and enough text that it has to be scrolled
LINK_PAGE = (b"<html><body><h1>The first page</h1>"
             + b"<p>A line filling page number one.</p>" * 40
             + b'<p>Go on to <a href="/second">the second page</a>.</p>'
             + b"</body></html>\n")
LINK_PAGE2 = (b"<html><body><h1>The second page</h1>"
              b"<p>The link worked.</p></body></html>\n")
# Above the old ceiling of 268 KiB per file - it reaches into the
# doubly indirect block
HUGE_PAGE = b"<p>ThreadOS TFS3: an indirect block, then a doubly indirect one.</p>\n" * 6500
# A page well above the old TFS limit (16 KiB per file) - it checks
# block allocation, the indirect block and TCP throughput
BIG_PAGE = b"<p>ThreadOS TFS3: block after block.</p>\n" * 3000


def make_gpt_disk(path, mib=800):
    """A disk image with a GPT partition table on it: an EFI system
    partition, a Windows one and a Linux one.

    This is what a laptop's disk looks like, and the reason the test
    needs one is not the table itself - it is what must NOT happen to
    it. A system that formats whatever disk it does not recognise
    destroys exactly this, and the assertion that it did not is worth
    nothing without a real table to destroy."""
    import binascii
    import uuid
    sec = 512
    total = mib * 2048

    def guid(s):
        return uuid.UUID(s).bytes_le

    parts = [
        ("C12A7328-F81F-11D2-BA4B-00A0C93EC93B", 2048, 2048 + 204800 - 1,
         "EFI system partition"),
        ("EBD0A0A2-B9E5-4433-87C0-68B6B72699C7", 2048 + 204800,
         2048 + 204800 + 409600 - 1, "Windows"),
        ("0FC63DAF-8483-4772-8E79-3D69D8477DE4", 2048 + 204800 + 409600,
         total - 34, "linux-root"),
    ]
    ent = bytearray(128 * 128)
    for i, (t, a, b, name) in enumerate(parts):
        e = bytearray(128)
        e[0:16] = guid(t)
        e[16:32] = guid("11111111-2222-3333-4444-%012d" % i)
        e[32:40] = struct.pack("<Q", a)
        e[40:48] = struct.pack("<Q", b)
        nm = name.encode("utf-16-le")[:72]
        e[56:56 + len(nm)] = nm
        ent[i * 128:(i + 1) * 128] = e

    hdr = bytearray(92)
    hdr[0:8] = b"EFI PART"
    hdr[8:12] = struct.pack("<I", 0x00010000)
    hdr[12:16] = struct.pack("<I", 92)
    hdr[24:32] = struct.pack("<Q", 1)
    hdr[32:40] = struct.pack("<Q", total - 1)
    hdr[40:48] = struct.pack("<Q", 34)
    hdr[48:56] = struct.pack("<Q", total - 34)
    hdr[56:72] = guid("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee")
    hdr[72:80] = struct.pack("<Q", 2)
    hdr[80:84] = struct.pack("<I", 128)
    hdr[84:88] = struct.pack("<I", 128)
    hdr[88:92] = struct.pack("<I", binascii.crc32(bytes(ent)) & 0xFFFFFFFF)
    hdr[16:20] = struct.pack("<I", binascii.crc32(bytes(hdr)) & 0xFFFFFFFF)

    # The protective MBR: one entry of type 0xEE covering everything, so
    # that a program which knows only the old format sees a full disk
    mbr = bytearray(sec)
    mbr[446 + 4] = 0xEE
    mbr[446 + 8:446 + 12] = struct.pack("<I", 1)
    mbr[446 + 12:446 + 16] = struct.pack("<I", min(total - 1, 0xFFFFFFFF))
    mbr[510] = 0x55
    mbr[511] = 0xAA

    # The copy at the end of the disk, which the format requires and a
    # real machine's disk has: the entries again, and behind them a
    # header pointing the other way. A test image without one is not
    # the shape of the thing being imitated.
    back = bytearray(92)
    back[0:8] = b"EFI PART"
    back[8:12] = struct.pack("<I", 0x00010000)
    back[12:16] = struct.pack("<I", 92)
    back[24:32] = struct.pack("<Q", total - 1)
    back[32:40] = struct.pack("<Q", 1)
    back[40:48] = struct.pack("<Q", 34)
    back[48:56] = struct.pack("<Q", total - 34)
    back[56:72] = guid("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee")
    back[72:80] = struct.pack("<Q", total - 33)
    back[80:84] = struct.pack("<I", 128)
    back[84:88] = struct.pack("<I", 128)
    back[88:92] = struct.pack("<I", binascii.crc32(bytes(ent)) & 0xFFFFFFFF)
    back[16:20] = struct.pack("<I", binascii.crc32(bytes(back)) & 0xFFFFFFFF)

    head = bytearray(sec * 2048)
    head[0:sec] = mbr
    head[sec:sec + 92] = hdr
    head[2 * sec:2 * sec + len(ent)] = ent
    with open(path, "wb") as f:
        f.write(bytes(head))
        f.truncate(total * sec)
    with open(path, "r+b") as f:
        f.seek((total - 33) * sec)
        f.write(bytes(ent))
        f.seek((total - 1) * sec)
        f.write(bytes(back))

    # Something recognisable inside the first two partitions. A test
    # that formats the third one and then says "the table survived" has
    # proved less than it sounds: the table is 34 sectors and the
    # neighbours are hundreds of megabytes. These say whether the
    # neighbours survived too.
    with open(path, "r+b") as f:
        for i, (_, start, _, _) in enumerate(parts[:2]):
            f.seek(start * sec)
            f.write(NEIGHBOUR_MARK % i)


NEIGHBOUR_MARK = b"SOMEBODY-ELSES-DATA-%d-DO-NOT-TOUCH" + b"\0" * 8


# --- FAT, made and read back by somebody else ------------------------
#
# The EFI partition is FAT, and FAT is the one filesystem in this
# project that is not ours. So the images the tests read are made by
# mtools, and what the system writes into them is read back by mtools
# too - an independent implementation on the other side of every
# assertion. A driver that agrees only with itself has proved nothing.

MTOOLS_ENV = dict(os.environ, MTOOLS_SKIP_CHECK="1")


def mtools_ok():
    return shutil.which("mformat") is not None


def _mt(args, quiet=True):
    return subprocess.run(
        args, env=MTOOLS_ENV,
        stdout=subprocess.DEVNULL if quiet else subprocess.PIPE,
        stderr=subprocess.DEVNULL)


def _at(path, offset_sectors):
    return f"{path}@@{offset_sectors * 512}"


def fat_format(path, offset_sectors, sectors, fat32=True, label="ESPTEST"):
    """Puts a FAT filesystem into an image at an offset. Whether it
    comes out as FAT12, FAT16 or FAT32 follows from the size, which is
    the only thing that decides it - so the test asks the bytes
    afterwards rather than assuming."""
    args = ["mformat", "-i", _at(path, offset_sectors),
            "-T", str(sectors), "-v", label]
    if fat32:
        args.insert(1, "-F")
    return _mt(args + ["::"]).returncode == 0


def fat_mkdirs(path, offset_sectors, dirs):
    for d in dirs:
        if _mt(["mmd", "-i", _at(path, offset_sectors), "::" + d]).returncode:
            return False
    return True


def fat_put(path, offset_sectors, data, dest):
    """Puts bytes into the image under the given name."""
    tmp = tempfile.mktemp()
    with open(tmp, "wb") as fh:
        fh.write(data)
    try:
        return _mt(["mcopy", "-i", _at(path, offset_sectors), tmp,
                    "::" + dest]).returncode == 0
    finally:
        os.unlink(tmp)


def fat_get(path, offset_sectors, name):
    """The contents of a file, read out of the image by mtools.
    None when it is not there."""
    r = subprocess.run(["mtype", "-i", _at(path, offset_sectors), "::" + name],
                       env=MTOOLS_ENV, stdout=subprocess.PIPE,
                       stderr=subprocess.DEVNULL)
    return r.stdout if r.returncode == 0 else None


def fat_dir(path, offset_sectors, where="::"):
    """What mtools sees in the image - the whole tree, long names and
    all."""
    r = subprocess.run(["mdir", "-i", _at(path, offset_sectors), "-/", where],
                       env=MTOOLS_ENV, stdout=subprocess.PIPE,
                       stderr=subprocess.DEVNULL)
    return r.stdout.decode(errors="replace") if r.returncode == 0 else ""


def fat_minfo(path, offset_sectors):
    """What mtools says about the volume - somebody else's reading of
    the same boot sector, used where our own would be no evidence."""
    r = subprocess.run(["minfo", "-i", _at(path, offset_sectors)],
                       env=MTOOLS_ENV, stdout=subprocess.PIPE,
                       stderr=subprocess.DEVNULL)
    return r.stdout.decode(errors="replace") if r.returncode == 0 else ""


def fat_kind(path, offset_sectors):
    """Which of the three it is, counted from the boot sector the same
    way the driver counts it: from the number of clusters, not from the
    string somebody wrote into the sector - with the one exception the
    driver also makes, for a volume that was formatted as FAT32 while
    being too small to need it (its sixteen-bit 'sectors per FAT' is
    zero, and that leaves no doubt)."""
    with open(path, "rb") as fh:
        fh.seek(offset_sectors * 512)
        bs = fh.read(512)
    if len(bs) < 512 or bs[510] != 0x55 or bs[511] != 0xAA:
        return None
    spc = bs[13]
    reserved = struct.unpack_from("<H", bs, 14)[0]
    nfats = bs[16]
    root_entries = struct.unpack_from("<H", bs, 17)[0]
    total = struct.unpack_from("<H", bs, 19)[0] or \
        struct.unpack_from("<I", bs, 32)[0]
    fat_secs = struct.unpack_from("<H", bs, 22)[0] or \
        struct.unpack_from("<I", bs, 36)[0]
    if not spc or not total or not fat_secs:
        return None
    root_secs = (root_entries * 32 + 511) // 512
    clusters = (total - (reserved + nfats * fat_secs + root_secs)) // spc
    if not struct.unpack_from("<H", bs, 22)[0] and not root_entries:
        return "FAT32"
    return "FAT12" if clusters < 4085 else \
           "FAT16" if clusters < 65525 else "FAT32"


def wav_tone(path, since=0):
    """What is in the sound QEMU wrote, from byte 'since' on.

    QEMU only fills in the sizes in the header when it closes the file,
    and a test kills the machine rather than waiting for it - so the
    header is read for the format and the rest is taken as samples.

    Returns (peak, milliseconds, hertz) of the loud part, or None when
    there is no sound in it. The frequency is the median distance
    between upward zero crossings rather than their count over the
    whole span: one crossing missed at an edge shifts a count, and
    shifts nothing in a median."""
    with open(path, "rb") as fh:
        head = fh.read(44)
        if len(head) < 44 or head[:4] != b"RIFF":
            return None
        fh.seek(44 + since)
        body = fh.read()
    rate = struct.unpack_from("<I", head, 24)[0]
    ch = struct.unpack_from("<H", head, 22)[0]
    if not rate or not ch or len(body) < 4:
        return None
    n = len(body) // 2
    s = struct.unpack("<%dh" % n, body[:n * 2])
    left = s[0::ch]
    peak = max(abs(v) for v in left)
    if peak < 200:
        return None
    loud = [i for i, v in enumerate(left) if abs(v) > peak // 3]
    a, b = loud[0], loud[-1]
    seg = left[a:b + 1]
    cross = [i for i in range(1, len(seg)) if seg[i - 1] <= 0 < seg[i]]
    ms = (b - a + 1) * 1000.0 / rate
    if len(cross) < 3:
        return (peak, ms, 0.0)
    gaps = sorted(cross[i] - cross[i - 1] for i in range(1, len(cross)))
    return (peak, ms, rate / float(gaps[len(gaps) // 2]))


def gpt_read(path):
    """Reads a GPT the way a firmware would, and says whether it holds
    together: the protective MBR, both headers, both CRCs.

    Written out here rather than asked of the system, because a table
    the system wrote and the system reads back proves only that it is
    consistent with itself. A firmware checks the CRCs and quietly
    decides a table is damaged; a test that does not check them would
    pass on a disk no machine will boot.

    Returns (ok, [(type_guid_first4, first_lba, last_lba, name), ...],
    problem) - the list is empty when the table does not hold."""
    import binascii
    with open(path, "rb") as fh:
        data = fh.read(34 * 512)
        fh.seek(0, 2)
        total = fh.tell() // 512
        fh.seek((total - 33) * 512)
        tail = fh.read(33 * 512)

    if data[510] != 0x55 or data[511] != 0xAA:
        return False, [], "no MBR signature"
    if data[446 + 4] != 0xEE:
        return False, [], "the MBR is not protective (type %#x)" % data[446 + 4]

    def header(buf, where):
        if buf[:8] != b"EFI PART":
            return None, "no 'EFI PART' in %s" % where
        size = struct.unpack_from("<I", buf, 12)[0]
        if size < 92 or size > 512:
            return None, "an odd header size in %s" % where
        want = struct.unpack_from("<I", buf, 16)[0]
        got = binascii.crc32(buf[:16] + b"\0\0\0\0" + buf[20:size]) & 0xFFFFFFFF
        if want != got:
            return None, "a bad header checksum in %s" % where
        return struct.unpack_from("<QQQQ", buf, 24), None

    prim, why = header(data[512:1024], "the main header")
    if prim is None:
        return False, [], why
    back, why = header(tail[32 * 512:33 * 512], "the copy at the end")
    if back is None:
        return False, [], why
    if prim[0] != 1 or prim[1] != total - 1:
        return False, [], "the main header points somewhere else"
    if back[0] != total - 1 or back[1] != 1:
        return False, [], "the copy at the end points somewhere else"

    n = struct.unpack_from("<I", data, 512 + 80)[0]
    size = struct.unpack_from("<I", data, 512 + 84)[0]
    crc = struct.unpack_from("<I", data, 512 + 88)[0]
    ents = data[2 * 512:2 * 512 + n * size]
    if (binascii.crc32(ents) & 0xFFFFFFFF) != crc:
        return False, [], "a bad checksum of the entry table"
    if ents != tail[:n * size]:
        return False, [], "the copy of the table at the end is different"

    out = []
    for i in range(n):
        e = ents[i * size:(i + 1) * size]
        if e[:16] == b"\0" * 16:
            continue
        first, last = struct.unpack_from("<QQ", e, 32)
        name = e[56:128].decode("utf-16-le").split("\0")[0]
        out.append((e[:4], first, last, name))
    return True, out, ""


GUID_ESP4 = bytes([0x28, 0x73, 0x2A, 0xC1])       # C12A7328-...
GUID_TFS4 = b"THRE"                                # ours, and it says so


def make_mbr_fat_disk(path, mib=64, fat32=False, label="SMALLFAT"):
    """A disk with the old table on it and one FAT partition - the
    shape of a memory stick, and the second of the two tables the
    system has to find its way around."""
    sec = 512
    total = mib * 2048
    start = 2048
    count = total - start
    mbr = bytearray(sec)
    mbr[446 + 4] = 0x0C if fat32 else 0x0E     # FAT32 LBA / FAT16 LBA
    mbr[446 + 8:446 + 12] = struct.pack("<I", start)
    mbr[446 + 12:446 + 16] = struct.pack("<I", count)
    mbr[510] = 0x55
    mbr[511] = 0xAA
    with open(path, "wb") as fh:
        fh.write(bytes(mbr))
        fh.truncate(total * sec)
    return fat_format(path, start, count, fat32=fat32, label=label), start


def neighbours_intact(path, mib=800, which=(0, 1)):
    """Are the marked partitions still carrying what was put in them?
    Read from the host, not asked of the system.

    'which' picks the ones to look at: a test that deliberately writes
    into one of them - putting a FAT filesystem on the EFI partition,
    say - has to leave that one out, or it will find its own doing and
    call it damage."""
    sec = 512
    starts = [2048, 2048 + 204800]
    with open(path, "rb") as f:
        for i, start in enumerate(starts):
            if i not in which:
                continue
            f.seek(start * sec)
            if f.read(len(NEIGHBOUR_MARK % i)) != NEIGHBOUR_MARK % i:
                return False
    return True


def gpt_intact(path):
    """Is the table still there? The signature of the protective MBR
    and the header that follows it."""
    with open(path, "rb") as f:
        d = f.read(1024)
    return (len(d) >= 1024 and d[510] == 0x55 and d[511] == 0xAA and
            d[512:520] == b"EFI PART")


def make_bmp(w=64, h=48):
    """A 24-bit BMP with four quarters: red, green, blue and white.
    Those four are in the screen's palette exactly, so a picture drawn
    from this file has to come out in exactly those colours - which is
    what tells a working decoder from a decoder that draws something."""
    stride = (w * 3 + 3) & ~3
    pixels = bytearray()
    for y in range(h):                      # BMP rows run bottom-up
        row = bytearray()
        for x in range(w):
            top = (h - 1 - y) < h // 2
            left = x < w // 2
            if top and left:
                bgr = (0, 0, 255)           # red, as BGR
            elif top:
                bgr = (0, 255, 0)           # green
            elif left:
                bgr = (255, 0, 0)           # blue
            else:
                bgr = (255, 255, 255)       # white
            row += bytes(bgr)
        row += b"\0" * (stride - len(row))
        pixels += row

    dib = bytes([40, 0, 0, 0]) + w.to_bytes(4, "little") \
        + h.to_bytes(4, "little") + (1).to_bytes(2, "little") \
        + (24).to_bytes(2, "little") + (0).to_bytes(4, "little") \
        + len(pixels).to_bytes(4, "little") + b"\0" * 16
    head = b"BM" + (14 + len(dib) + len(pixels)).to_bytes(4, "little") \
        + b"\0\0\0\0" + (14 + len(dib)).to_bytes(4, "little")
    return head + dib + bytes(pixels)


BMP_IMAGE = make_bmp()


def make_png(w=64, h=48):
    """The same four quarters as the BMP, as a PNG: truecolour, eight
    bits a channel, one filter byte per row and the whole thing in a
    zlib stream. Written by hand (zlib is in the standard library, an
    image library is not), so the test brings its own picture."""
    import struct

    def colour(x, y):
        top, left = y < h // 2, x < w // 2
        if top and left:
            return (255, 0, 0)
        if top:
            return (0, 255, 0)
        if left:
            return (0, 0, 255)
        return (255, 255, 255)

    def paeth(a, b, c):
        p = a + b - c
        pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
        return a if pa <= pb and pa <= pc else (b if pb <= pc else c)

    # A different filter on every row, going round all five. They are
    # where a decoder usually breaks, and a picture whose rows are all
    # unfiltered says nothing about the other four.
    rows = [[v for x in range(w) for v in colour(x, y)] for y in range(h)]
    raw = bytearray()
    for y in range(h):
        f = y % 5
        raw.append(f)
        line, above = rows[y], (rows[y - 1] if y else [0] * (w * 3))
        for i in range(w * 3):
            a = line[i - 3] if i >= 3 else 0
            b = above[i]
            c = above[i - 3] if i >= 3 else 0
            v = line[i]
            if f == 1:
                v -= a
            elif f == 2:
                v -= b
            elif f == 3:
                v -= (a + b) // 2
            elif f == 4:
                v -= paeth(a, b, c)
            raw.append(v & 0xFF)

    def chunk(tag, body):
        return (struct.pack(">I", len(body)) + tag + body
                + struct.pack(">I", zlib.crc32(tag + body) & 0xffffffff))

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr)
            + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
            + chunk(b"IEND", b""))


PNG_IMAGE = make_png()


# The same four quarters again, this time as a baseline JPEG.
#
# This one is not written here: a JPEG encoder is a second half of the
# work the decoder does, and a picture shaped by our own arithmetic
# would flatter it. These bytes came out of an outside encoder (Go's
# image/jpeg, quality 90, 4:2:0), so the test asks the decoder to read
# somebody else's file - which is the only kind that matters.
#
# 4:2:0 means the colour is stored at half resolution and put back by
# the decoder, so the quarters are read well inside their corners,
# away from the boundaries where a lossy format is allowed to smudge.
JPEG_IMAGE = base64.b64decode(
    "/9j/2wCEAAMCAgMCAgMDAwMEAwMEBQgFBQQEBQoHBwYIDAoMDAsKCwsNDhIQDQ4RDgsL"
    "EBYQERMUFRUVDA8XGBYUGBIUFRQBAwQEBQQFCQUFCRQNCw0UFBQUFBQUFBQUFBQUFBQU"
    "FBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFP/AABEIADAAQAMBIgACEQED"
    "EQH/xAGiAAABBQEBAQEBAQAAAAAAAAAAAQIDBAUGBwgJCgsQAAIBAwMCBAMFBQQEAAAB"
    "fQECAwAEEQUSITFBBhNRYQcicRQygZGhCCNCscEVUtHwJDNicoIJChYXGBkaJSYnKCkq"
    "NDU2Nzg5OkNERUZHSElKU1RVVldYWVpjZGVmZ2hpanN0dXZ3eHl6g4SFhoeIiYqSk5SV"
    "lpeYmZqio6Slpqeoqaqys7S1tre4ubrCw8TFxsfIycrS09TV1tfY2drh4uPk5ebn6Onq"
    "8fLz9PX29/j5+gEAAwEBAQEBAQEBAQAAAAAAAAECAwQFBgcICQoLEQACAQIEBAMEBwUE"
    "BAABAncAAQIDEQQFITEGEkFRB2FxEyIygQgUQpGhscEJIzNS8BVictEKFiQ04SXxFxgZ"
    "GiYnKCkqNTY3ODk6Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqCg4SFhoeI"
    "iYqSk5SVlpeYmZqio6Slpqeoqaqys7S1tre4ubrCw8TFxsfIycrS09TV1tfY2dri4+Tl"
    "5ufo6ery8/T19vf4+fr/2gAMAwEAAhEDEQA/APnSiiivww/1TCiiigD7Gooor8SP+Y8K"
    "KKKAPjmivAqK/wBUP+JZf+px/wCUP/ux/tV/xMN/1Kv/ACt/9yPfaK8Coo/4ll/6nH/l"
    "D/7sH/Ew3/Uq/wDK3/3I/UOiv0Yor+H/APiGf/UZ/wCU/wD7c/zj/wCIPf8AUf8A+Uv/"
    "ALofnPRX6MUUf8Qz/wCoz/yn/wDbh/xB7/qP/wDKX/3Q/mkooor/AHDP1cKKKKAP6W6K"
    "KK/ysPuAooooA//Z")

# A gradient, as a baseline JPEG - the picture that shows what a
# palette costs.
#
# The four quarters above are four flat fields, and a palette of two
# hundred and fifty six holds those perfectly well; a decoder could
# lose almost all its colours and still pass. This one has red rising
# left to right and green top to bottom, so it is three thousand
# distinct colours in three thousand pixels - and a screen speaking in
# indices cannot show more than a few dozen of them whatever it does.
#
# From the same outside encoder (Go's image/jpeg, quality 95).
JPEG_GRADIENT = base64.b64decode(
    "/9j/2wCEAAIBAQEBAQIBAQECAgICAgQDAgICAgUEBAMEBgUGBgYFBgYGBwkIBgcJBwYG"
    "CAsICQoKCgoKBggLDAsKDAkKCgoBAgICAgICBQMDBQoHBgcKCgoKCgoKCgoKCgoKCgoK"
    "CgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCv/AABEIADAAQAMBIgACEQED"
    "EQH/xAGiAAABBQEBAQEBAQAAAAAAAAAAAQIDBAUGBwgJCgsQAAIBAwMCBAMFBQQEAAAB"
    "fQECAwAEEQUSITFBBhNRYQcicRQygZGhCCNCscEVUtHwJDNicoIJChYXGBkaJSYnKCkq"
    "NDU2Nzg5OkNERUZHSElKU1RVVldYWVpjZGVmZ2hpanN0dXZ3eHl6g4SFhoeIiYqSk5SV"
    "lpeYmZqio6Slpqeoqaqys7S1tre4ubrCw8TFxsfIycrS09TV1tfY2drh4uPk5ebn6Onq"
    "8fLz9PX29/j5+gEAAwEBAQEBAQEBAQAAAAAAAAECAwQFBgcICQoLEQACAQIEBAMEBwUE"
    "BAABAncAAQIDEQQFITEGEkFRB2FxEyIygQgUQpGhscEJIzNS8BVictEKFiQ04SXxFxgZ"
    "GiYnKCkqNTY3ODk6Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqCg4SFhoeI"
    "iYqSk5SVlpeYmZqio6Slpqeoqaqys7S1tre4ubrCw8TFxsfIycrS09TV1tfY2dri4+Tl"
    "5ufo6ery8/T19vf4+fr/2gAMAwEAAhEDEQA/APxY0nwMOP3X6V0+k+Beg8r68V3+k+Be"
    "n7n9K6bSfAo4/cfpX2+M4h31PD4a4q+H3jgNI8DdP3X6V0+k+Bhx+6/Su/0nwKMj9z+l"
    "dPpPgXp+5/SvlMbxD5n9BcNcU/D7xwGk+Beg8r68V0+keBuBiL9K77SfAo4/cfpXUaT4"
    "F6fuf0r5PG8Q+Z/QPDXFO3vHn+k+Bhx+6/Sun0nwL0/dfpXf6T4F6fuf0rp9J8C8j9z+"
    "lfKYziHfU/oLhrir4fePlbSfAo4/c/pXTaT4F6fuP0r0DSfAp4/c/pXTaT4F6fuP0r0M"
    "bxDvqf8APTw1xS/d944DSPAvT9x+ldPpPgUcfuf0rv8ASPAvI/cfpXT6T4FPH7n9K+Tx"
    "nEO+p/QPDXFXw+8ef6T4F6fuP0rqNI8CjAxB+ld/pPgXp+4/Sun0nwKeP3P6V8njeId9"
    "T+guGeKtV7x5/pPgUcfuf0rp9J8C9P3H6V3+k+BTx+5/Sun0nwLyP3H6V8njOIfM/oLh"
    "ril+77x8raT4F6fuf0rptJ8C9P3P6V6BpPgU8Hyf0rptJ8Cnj9yfyr0MZxDvqf8APTw1"
    "xT8PvHAaR4Fzj9z+ldPpPgXp+5/Su/0nwIcj9z+ldPpPgU8Hyf0r5TG8Q+Z/QPDXFPw+"
    "8ef6T4F6fuf0rqNI8C9P3P6V32k+BTx+5P5V1Gk+BSQB5P6V8njeIfM/oHhninb3jz/S"
    "fAvT9z+ldPpPgXp+5/Su/wBJ8Cng+T+ldPpPgQ8fuf0r5PGcQ76n9BcNcU/D7x//2Q==")


_FONT8X8 = None


def font8x8():
    """The console font read straight from the kernel's header: a
    pattern of eight bytes -> a character. A single source of truth -
    when the font changes, the tests read the new one."""
    global _FONT8X8
    if _FONT8X8 is None:
        path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                               "..", "kernel", "include", "font8x8.h")
        with open(path) as fh:
            src = fh.read()
        rows = re.findall(r"\{((?:0x[0-9A-Fa-f]{2},){7}0x[0-9A-Fa-f]{2})\}",
                             src)
        _FONT8X8 = {}
        for i, w in enumerate(rows):
            pattern = tuple(int(v, 16) for v in w.split(","))
            _FONT8X8.setdefault(pattern, chr(0x20 + i))
    return _FONT8X8


def _kernel_define(path, name):
    """A #define straight out of a kernel source file - so a number the
    desktop is built with and the number a test computes positions with
    cannot drift apart."""
    full = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..",
                        path)
    with open(full) as fh:
        m = re.search(r"#define\s+%s\s+(\d+)" % name, fh.read())
    return int(m.group(1))


def _look():
    """Every number in kernel/include/look.h - the desktop's sizes and
    both themes' colours - plus the aurora's clouds, read straight from
    the file the kernel is built with."""
    full = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..",
                        "kernel", "include", "look.h")
    with open(full) as fh:
        src = fh.read()
    vals = {}
    for m in re.finditer(r"^#define\s+(\w+)\s+([^\\\n]+)$", src, re.M):
        expr = m.group(2).split("/*")[0].strip()
        try:
            vals[m.group(1)] = int(eval(expr, {}, dict(vals)))
        except Exception:
            pass
    for theme in ("DARK", "LIGHT"):
        m = re.search(r"#define AURORA_%s_CLOUDS((?:[^\n]*\\\n)*[^\n]*)"
                      % theme, src)
        vals["AURORA_%s_CLOUDS" % theme] = [
            tuple(int(v, 0) for v in c.split(","))
            for c in re.findall(r"\{([^}]*)\}", m.group(1))]
    return vals


LOOK = _look()
globals().update({k: v for k, v in LOOK.items() if not k.startswith("_")})

# The size of the desktop's lettering
UI_PX = _kernel_define("kernel/include/uitext.h", "UI_PX")
# ... and of the programs', which draw through user/udraw.h
UD_PX = _kernel_define("user/udraw.h", "UD_PX")

# The settings window (user/gsettings.c): its size, its rows, where a
# setting's value starts, and the line at its bottom that says what
# became of a change
GSET_W = _kernel_define("user/gsettings.c", "W0")
GSET_H = _kernel_define("user/gsettings.c", "H0")
GSET_ROW_Y = _kernel_define("user/gsettings.c", "ROW_Y")
GSET_ROW_H = _kernel_define("user/gsettings.c", "ROW_H")
GSET_VAL_X = _kernel_define("user/gsettings.c", "VAL_X")
GSET_NOTE_DY = _kernel_define("user/gsettings.c", "NOTE_DY")


# The clock (user/gclock.c): its size and the time on it
GCLOCK_W = _kernel_define("user/gclock.c", "W")
GCLOCK_H = _kernel_define("user/gclock.c", "H")
GCLOCK_PX = _kernel_define("user/gclock.c", "TIME_PX")
GCLOCK_TIME_Y = _kernel_define("user/gclock.c", "TIME_Y")
GCLOCK_TIME_H = _kernel_define("user/gclock.c", "TIME_H")


# The file manager (user/gfiles.c): its size, its rows and the status
# line along its bottom
GFILES_W = _kernel_define("user/gfiles.c", "W0")
GFILES_H = _kernel_define("user/gfiles.c", "H0")
GFILES_HEAD_H = _kernel_define("user/gfiles.c", "HEAD_H")
GFILES_ROW_Y0 = _kernel_define("user/gfiles.c", "ROW_Y0")
GFILES_ROW_H = _kernel_define("user/gfiles.c", "ROW_H")
GFILES_FOOT_H = _kernel_define("user/gfiles.c", "FOOT_H")
GFILES_NAME_X = _kernel_define("user/gfiles.c", "NAME_X")


# The kit's question panel (user/ugui.h): a band across the middle of
# a window, a bold question at its top and two answers at its bottom
UG_MODAL_H = _kernel_define("user/ugui.h", "UG_MODAL_H")
UG_BTN_W = _kernel_define("user/ugui.h", "UG_BTN_W")
UG_BTN_H = _kernel_define("user/ugui.h", "UG_BTN_H")


def ug_modal(ch):
    """Where the question panel of a window ch tall is, in its content:
    its top 'y', the baseline of its question, and the middles of its
    two answers"""
    y = max((ch - UG_MODAL_H) // 2, 0)
    by = y + UG_MODAL_H - UG_BTN_H - 12
    yes_x = 10 + 12
    no_x = yes_x + UG_BTN_W + 10
    return {"y": y, "title": y + 22,
            "yes": (yes_x + UG_BTN_W // 2, by + UG_BTN_H // 2),
            "no": (no_x + UG_BTN_W // 2, by + UG_BTN_H // 2)}


# ... and its picker: a title, a list of names, a line to type in and
# the two answers
UG_ROW = _kernel_define("user/ugui.h", "UG_ROW")
UG_PICK_ROWS = _kernel_define("user/ugui.h", "UG_PICK_ROWS")
UG_PICK_H = 30 + UG_PICK_ROWS * UG_ROW + 8 + 22 + 10 + UG_BTN_H + 12


def ug_pick(ch):
    """Where the picker of a window ch tall is, in its content: the
    baseline of its title, the top of its list, the middle of its row
    r (row_y(r)) and the middles of its two answers"""
    y = max((ch - UG_PICK_H) // 2, 2)
    list_y = y + 30
    by = y + UG_PICK_H - UG_BTN_H - 12
    yes_x = 10 + 12
    no_x = yes_x + UG_BTN_W + 10
    return {"y": y, "title": y + 20, "list_y": list_y,
            "row_y": lambda r: list_y + r * UG_ROW + UG_ROW // 2,
            "yes": (yes_x + UG_BTN_W // 2, by + UG_BTN_H // 2),
            "no": (no_x + UG_BTN_W // 2, by + UG_BTN_H // 2)}


# The file manager's three buttons, held against the right edge of its
# heading: Remove, then Open, then Rename
GFILES_DEL_W = _kernel_define("user/gfiles.c", "DEL_W")
GFILES_EDIT_W = _kernel_define("user/gfiles.c", "EDIT_W")
GFILES_RENAME_W = _kernel_define("user/gfiles.c", "NAME_W")


def gfiles_button(which, cw=None):
    """The middle of the file manager's button 'remove', 'open' or
    'rename', in its content"""
    cw = cw or GFILES_W
    del_x = cw - 10 - GFILES_DEL_W
    edit_x = del_x - 6 - GFILES_EDIT_W
    name_x = edit_x - 6 - GFILES_RENAME_W
    x = {"remove": del_x + GFILES_DEL_W // 2,
         "open": edit_x + GFILES_EDIT_W // 2,
         "rename": name_x + GFILES_RENAME_W // 2}[which]
    return x, GFILES_HEAD_H // 2


# The picture viewer (user/gimg.c): its status line along the bottom
GIMG_STATUS_H = _kernel_define("user/gimg.c", "STATUS_H")
GIMG_STATUS_PX = _kernel_define("user/gimg.c", "STATUS_PX")


def gfiles_row_y(r):
    """The middle of the file manager's row r (counted from the top of
    what is shown), in its content"""
    return GFILES_ROW_Y0 + r * GFILES_ROW_H + GFILES_ROW_H // 2


# The terminal (user/gterm.c): its size, and the grid its letters sit on
TERM_W = _kernel_define("user/gterm.c", "W0")
TERM_H = _kernel_define("user/gterm.c", "H0")
TERM_PAD = _kernel_define("user/gterm.c", "PAD")
TERM_ROW_H = _kernel_define("user/gterm.c", "ROW_H")
TERM_CELL = _kernel_define("user/gterm.c", "CELL")
TERM_PX = _kernel_define("user/gterm.c", "TERM_PX")
TERM_BASE = _kernel_define("user/gterm.c", "TERM_BASE")
TERM_COLS = (TERM_W - 2 * TERM_PAD) // TERM_CELL
TERM_ROWS = (TERM_H - 2 * TERM_PAD) // TERM_ROW_H


def gset_row_y(r):
    """The middle of the settings window's row r, in its content"""
    return GSET_ROW_Y + r * GSET_ROW_H + GSET_ROW_H // 2
# Where a window's content starts below its top edge: the frame line
# and the title bar
CONTENT_DY = 1 + TITLE_H


def rgb(v):
    return ((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF)


def theme(name="dark"):
    """One theme's colours from look.h, by their lower-case names:
    theme("light")["title_bg"] is LIGHT_TITLE_BG as (r, g, b); the
    alphas (..._a) stay numbers."""
    pre = name.upper() + "_"
    out = {}
    for k, v in LOOK.items():
        if k.startswith(pre) and not k.startswith("AURORA"):
            key = k[len(pre):].lower()
            out[key] = v if key.endswith("_a") else rgb(v)
    return out


def _tdiv(a, b):
    """C's division: towards zero"""
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b >= 0) else -q


def aurora_px(x, y, w=640, h=480, dark=True):
    """One pixel of the aurora, by the arithmetic kernel/aurora.c spells
    out - whole numbers, in the same order, so the very same value."""
    t = "DARK" if dark else "LIGHT"
    top, bottom = LOOK["AURORA_%s_TOP" % t], LOOK["AURORA_%s_BOTTOM" % t]
    p = []
    for ch in range(3):
        a = (top >> (16 - 8 * ch)) & 0xFF
        b = (bottom >> (16 - 8 * ch)) & 0xFF
        p.append(a + _tdiv((b - a) * y, h - 1) if h > 1 else a)
    X, Y = 2 * x + 1, 2 * y + 1
    for cx, cy, r, colour, strength in LOOK["AURORA_%s_CLOUDS" % t]:
        cx2, cy2 = 2 * (cx * w // 1000), 2 * (cy * h // 1000)
        rr = (2 * (r * w // 1000)) ** 2
        d2 = (X - cx2) ** 2 + (Y - cy2) ** 2
        if d2 >= rr:
            continue
        tt = (rr - d2) * 256 // rr
        a = (((tt * tt) >> 8) * strength) >> 8
        for ch in range(3):
            col = (colour >> (16 - 8 * ch)) & 0xFF
            p[ch] = (p[ch] * (256 - a) + col * a) >> 8
    return tuple(p)


def near(px, want, tol=40):
    return all(abs(px[i] - want[i]) <= tol for i in range(3))


def _box(vals, r):
    """One pass of kernel/gfx.c's box: len(vals) - 2r averages, each the
    sum times 65536/n rounded, plus a half, over 65536"""
    n = 2 * r + 1
    inv = (65536 + n // 2) // n
    out, s = [], sum(vals[:n])
    for i in range(len(vals) - 2 * r):
        out.append((s * inv + 32768) >> 16)
        if i + n < len(vals):
            s += vals[i + n] - vals[i]
    return out


def gfx_blur_ref(img, x, y, w, h, r):
    """gfx_blur() on a picture held as rows of (r, g, b): two passes of
    the box each way over the rectangle, reading what lies around it
    (the nearest pixel past the picture's edge), writing only inside.
    Returns a new picture."""
    ph, pw = len(img), len(img[0])
    e = 2 * r
    out = [list(row) for row in img]
    chans = []
    for ch in range(3):
        ext = []
        for j in range(h + 2 * e):
            sy = min(max(y - e + j, 0), ph - 1)
            ext.append([img[sy][min(max(x - e + i, 0), pw - 1)][ch]
                        for i in range(w + 2 * e)])
        a = [_box(row, r) for row in ext]
        a = list(zip(*[_box(list(c), r) for c in zip(*a)]))
        a = [_box(list(row), r) for row in a]
        a = list(zip(*[_box(list(c), r) for c in zip(*a)]))
        chans.append(a)
    for j in range(h):
        for i in range(w):
            out[y + j][x + i] = (chans[0][j][i], chans[1][j][i], chans[2][j][i])
    return out


def gfx_mix(over, under, a):
    """kernel/gfx.c's mix, channel by channel"""
    return tuple((o * a + u * (255 - a) + 127) // 255
                 for o, u in zip(over, under))


def frost_ref(x, y, w, h, blur, tint, a, sw, sh, dark=True):
    """gfx_frost() over the bare aurora, as a function of (x, y): what
    lies under text written on glass that is not the bar's - the
    kiosk's button, say. Exact away from the round corners."""
    e = 2 * blur
    px0, py0 = max(x - e, 0), max(y - e, 0)
    px1, py1 = min(x + w + e, sw), min(y + h + e, sh)
    img = [[aurora_px(xx, yy, sw, sh, dark) for xx in range(px0, px1)]
           for yy in range(py0, py1)]
    img = gfx_blur_ref(img, x - px0, y - py0, w, h, blur)
    tint = rgb(tint) if isinstance(tint, int) else tint

    def under(xx, yy):
        return gfx_mix(tint, img[yy - py0][xx - px0], a)
    return under


_BAR_GLASS = {}


def bar_glass(dark=True, w=SCREEN_W if "SCREEN_W" in globals() else 640,
              h=480):
    """The menu bar's glass as kernel/desktop.c draws it once: the strip
    of the aurora under it, blurred, then the theme's tint laid over -
    as rows of (r, g, b), BAR_H of them. Exact, so a pixel of the bar
    that nothing is drawn on can be held to it."""
    key = (dark, w, h)
    if key not in _BAR_GLASS:
        strip = min(BAR_H + 4 * BAR_BLUR, h)
        img = [[aurora_px(x, y, w, h, dark) for x in range(w)]
               for y in range(strip)]
        img = gfx_blur_ref(img, 0, 0, w, BAR_H, BAR_BLUR)
        t = theme("dark" if dark else "light")
        _BAR_GLASS[key] = [[gfx_mix(t["bar_tint"], img[y][x], t["bar_a"])
                            for x in range(w)] for y in range(BAR_H)]
    return _BAR_GLASS[key]


def aurora_samples(at, w, h, scale=1, step=8, tol=8, dark=True):
    """How many of the points on a grid 'step' apart show the bare aurora
    - out of how many. A screen of w x h physical pixels, each logical
    pixel 'scale' of them across."""
    lw, lh = w // scale, h // scale
    hit = total = 0
    for y in range(0, h, step):
        for x in range(0, w, step):
            total += 1
            if near(at(x, y), aurora_px(x // scale, y // scale, lw, lh, dark),
                    tol):
                hit += 1
    return hit, total


def on_bar(x, y):
    """What lies under the menu bar's text: its glass (dark theme)"""
    return bar_glass()[y][x]


def ui_font_baseline_in(px, top, height):
    """The baseline the kernel gives text of size px centred in a band -
    the same arithmetic as ui_font_baseline_in() in kernel/uitext.c."""
    cap = (px * 73 + 50) // 100
    return top + (height + cap) // 2


def ui_baseline_in(top, height):
    return ui_font_baseline_in(UI_PX, top, height)


# --- where things are on a 640 x 480 desktop (kernel/desktop.c) ---------

SCREEN_W, SCREEN_H = 640, 480
MAX_DOCK_WINDOWS = 12           # MAX_WIN in kernel/desktop.c
DOCK_N = 9                      # the programs in a dock nobody configured


def dock_geom(n_l=DOCK_N, n_w=2, want=None, w=SCREEN_W, h=SCREEN_H):
    """The dock as dock_geom() lays it out: its glass, its tile size and
    where every tile is - programs first, then the windows on this
    desktop. 'tiles' holds each tile's (x, y) top left corner."""
    want = want or DOCK_TILE_M
    n = n_l + n_w
    fixed = (2 * DOCK_PAD + (n - 1) * DOCK_GAP if n else 2 * DOCK_PAD) + \
        (DOCK_SEP - DOCK_GAP if n_l and n_w else 0)
    room = w - 2 * DOCK_MARGIN
    t = want
    if n and fixed + n * t > room:
        t = (room - fixed) // n
    t = max(t, DOCK_TILE_MIN)
    dw, dh = fixed + n * t, want + 2 * DOCK_PAD
    x, y = (w - dw) // 2, h - DOCK_BOTTOM - dh
    ty = y + DOCK_PAD + (want - t) // 2
    tiles = []
    for k in range(n):
        tx = x + DOCK_PAD + k * (t + DOCK_GAP)
        if n_l and k >= n_l:
            tx += DOCK_SEP - DOCK_GAP
        tiles.append((tx, ty))
    return dict(x=x, y=y, w=dw, h=dh, tile=t, ty=ty, tiles=tiles,
                n_l=n_l, n_w=n_w)


def dock_tile(k, n_l=DOCK_N, n_w=2, **kw):
    """The middle of the k-th tile - the place to click"""
    g = dock_geom(n_l, n_w, **kw)
    tx, ty = g["tiles"][k]
    return tx + g["tile"] // 2, ty + g["tile"] // 2


def dock_window(j, n_w, n_l=DOCK_N, **kw):
    """The middle of the tile of the j-th window on this desktop"""
    return dock_tile(n_l + j, n_l, n_w, **kw)


def work_bottom(want=None, h=SCREEN_H):
    """Where windows end: above the dock (work_bottom())"""
    return dock_geom(want=want, h=h)["y"] - DOCK_BOTTOM


def cascade(slot, ww, wh, w=SCREEN_W, h=SCREEN_H):
    """Where a new window of ww x wh (its frame) opens, the way
    desktop_win_create() places it: a step of twelve down and right per
    slot, and three steps back up for as long as it would not fit
    between the bar and the dock"""
    k = slot
    while k > 0 and (70 + k * 12 + ww > w or
                     30 + k * 12 + wh > work_bottom(h=h)):
        k -= 3
    k = max(k, 0)
    return 70 + k * 12, 30 + k * 12


def bar_clock_x(w=SCREEN_W):
    return w - BAR_RIGHT - BAR_CLOCK_W


def bar_desk_x(d=0, w=SCREEN_W):
    """The left edge of desktop d's cell on the bar"""
    return bar_clock_x(w) - BAR_ITEM_GAP - 4 * DESK_CELL + d * DESK_CELL


def bar_load_x(w=SCREEN_W):
    return bar_desk_x(0, w) - BAR_ITEM_GAP - LOAD_W


def bar_show_x(w=SCREEN_W):
    return bar_load_x(w) - BAR_ITEM_GAP - SHOW_W


BAR_MID = BAR_H // 2
# The mark at the left end of the bar, which opens the menu
LOGO_XY = (BAR_LOGO_X + BAR_LOGO_D // 2, BAR_MID)


def light_xy(wx, wy, k):
    """The middle of a window's light: 0 close, 1 minimise, 2 zoom"""
    return (wx + LIGHT_X + k * LIGHT_STEP + LIGHT_D // 2,
            wy + (TITLE_H - LIGHT_D) // 2 + 1 + LIGHT_D // 2)


def title_baseline(wy):
    return ui_font_baseline_in(TITLE_PX, wy, TITLE_H)


class UiFont:
    """The desktop's TrueType lettering, as FreeType draws it.

    The kernel's glyphs equal FreeType's to within 2/255 of coverage
    (scripts/tests/t_ttf.py holds it to that), so FreeType's glyphs are
    what the screen reader matches the screen against. Placed exactly as
    the kernel places them: the pen moves in 1/64 px by the font's
    advances, and each glyph goes to the pen's nearest whole pixel."""

    _shared = {}
    CHARSET = ("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
               "0123456789 .,:;-_()[]/*!?'\"+=<>@#%&")

    def __init__(self, px=None, face="sans"):
        import freetype
        self.px = px or UI_PX
        path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                            "..", "fonts", face + ".ttf")
        self.face = freetype.Face(path)
        self.face.set_pixel_sizes(0, self.px)
        self.ascent = (self.face.size.ascender + 63) >> 6
        self.descent = (-self.face.size.descender + 63) >> 6
        self.cache = {}

    @classmethod
    def shared(cls, px=None, face="sans"):
        key = (px or UI_PX, face)
        if key not in cls._shared:
            cls._shared[key] = UiFont(px, face)
        return cls._shared[key]

    def glyph(self, ch):
        g = self.cache.get(ch)
        if g is None:
            import freetype
            self.face.load_char(ch, freetype.FT_LOAD_RENDER |
                                freetype.FT_LOAD_NO_HINTING)
            gl = self.face.glyph
            b = gl.bitmap
            a = bytes(v for r in range(b.rows)
                      for v in b.buffer[r * b.pitch:r * b.pitch + b.width])
            g = (gl.advance.x, gl.bitmap_left, gl.bitmap_top, b.width,
                 b.rows, a)
            self.cache[ch] = g
        return g


# --- shapes, by their true areas ------------------------------------------

SHAPE_SUB = 16                  # samples per pixel, each way


def in_rrect(x, y, rx, ry, rw, rh, r):
    """Whether (x, y) is inside a rectangle with corners of radius r"""
    r = min(r, rw / 2, rh / 2)
    cx, cy = rx + rw / 2, ry + rh / 2
    qx = abs(x - cx) - (rw / 2 - r)
    qy = abs(y - cy) - (rh / 2 - r)
    if qx <= 0 or qy <= 0:
        return qx <= r and qy <= r
    return qx * qx + qy * qy <= r * r


def in_line(x, y, x0, y0, x1, y1, t):
    """Whether (x, y) is inside a line t thick with round ends"""
    dx, dy = x1 - x0, y1 - y0
    l2 = dx * dx + dy * dy
    u = 0 if l2 == 0 else max(0.0, min(1.0, ((x - x0) * dx + (y - y0) * dy) / l2))
    px, py = x0 + u * dx, y0 + u * dy
    return (x - px) ** 2 + (y - py) ** 2 <= (t / 2) ** 2


def shape_area(inside, pw, ph):
    """How much of every pixel of a pw x ph picture the shape covers,
    out of 255: each pixel cut into SHAPE_SUB x SHAPE_SUB pieces and
    the ones whose middles are 'inside' counted"""
    out = []
    for py in range(ph):
        row = []
        for px in range(pw):
            n = 0
            for j in range(SHAPE_SUB):
                y = py + (j + 0.5) / SHAPE_SUB
                for i in range(SHAPE_SUB):
                    if inside(px + (i + 0.5) / SHAPE_SUB, y):
                        n += 1
            row.append(n * 255 // (SHAPE_SUB * SHAPE_SUB))
        out.append(row)
    return out



def check_area(label, ours, ref, max_diff=20, mean_diff=1.0):
    """Holds a drawn shape (rows of 0..255) up against its true area:
    every pixel within max_diff, and on average within mean_diff"""
    if ours is None:
        check("%s: the system printed the shape" % label, False)
        return
    worst, total, n = 0, 0, 0
    for ro, rr in zip(ours, ref):
        for a, b in zip(ro, rr):
            d = abs(a - b)
            worst = max(worst, d)
            total += d
            n += 1
    lit = sum(1 for r in ours for v in r if v)
    check("%s: within %d of the exact area - worst %d, mean %.2f over %d "
          "pixels (%d lit)" % (label, max_diff, worst, total / n, n, lit),
          lit > 0 and worst <= max_diff and total / n <= mean_diff)


def ui_text_colour(bg, inkiest, colours):
    """Which of 'colours' text is in, when no pixel of it is wholly
    covered - red on a dark surface may never reach its own red. Every
    pixel of the text lies on the way from the background to the text's
    colour, the inkiest one furthest along it: the colour whose way
    passes closest to that pixel is the one."""
    best, best_err = None, None
    for c in colours:
        d = [c[i] - bg[i] for i in range(3)]
        n = sum(v * v for v in d)
        if not n:
            continue
        t = sum((inkiest[i] - bg[i]) * d[i] for i in range(3)) / n
        if t < 0.5 or t > 1.02:
            continue
        err = sum(abs(bg[i] + t * d[i] - inkiest[i]) for i in range(3))
        if best_err is None or err < best_err:
            best, best_err = c, err
    return best if best_err is not None and best_err < 12 else None


def ui_mix(text, under, a):
    """The kernel's mixing of a glyph's coverage with what is under it
    (kernel/uitext.c), channel by channel."""
    return tuple((t * a + u * (255 - a) + 127) // 255
                 for t, u in zip(text, under))


DNS_HINT = ("port 53 is taken, or out of reach - run as root, or "
            "once: sudo sysctl -w net.ipv4.ip_unprivileged_port_start=53")


def start_dns_server(answer_ip=(10, 0, 2, 2)):
    """A very simple DNS server on the host: to every question about an A
    record it answers with the same address. Thanks to that the path
    'name -> address' can be checked with no network outside at all.

    It has to be port 53 and no other: the guest asks 10.0.2.2:53 and
    QEMU's user network sends that at the host's port 53, with no way to
    remap it. Binding a port below 1024 needs root, which a CI runner
    has not got - so a refusal comes back as (stop, False) and the group
    says why it is giving up, instead of a traceback that looks like a
    bug in the system.

    Returns (stop, ok)."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        sock.bind(("0.0.0.0", 53))
    except OSError:
        sock.close()
        return (lambda: None), False
    stop = threading.Event()

    def serve():
        sock.settimeout(0.5)
        while not stop.is_set():
            try:
                data, addr = sock.recvfrom(512)
            except socket.timeout:
                continue
            except OSError:
                break
            if len(data) < 12:
                continue
            # a question ends with a zero name byte + QTYPE + QCLASS
            i = 12
            while i < len(data) and data[i]:
                i += data[i] + 1
            qend = i + 5
            header = data[:2] + b"\x81\x80" + data[4:6] + b"\x00\x01" \
                     + b"\x00\x00\x00\x00"
            answer = (b"\xc0\x0c"            # a pointer to the name from the question
                      + b"\x00\x01\x00\x01"  # type A, class IN
                      + b"\x00\x00\x00\x3c"  # TTL 60 s
                      + b"\x00\x04" + bytes(answer_ip))
            sock.sendto(header + data[12:qend] + answer, addr)
        sock.close()

    threading.Thread(target=serve, daemon=True).start()
    return stop.set, True


CERT_DER = None          # the HTTPS server's certificate (for the trust store)
CERT_DER_EC = None       # the same, but on the P-256 curve
CERT_DER_P384 = None     # and on the P-384 curve (a SHA-384 signature)


def make_test_cert(dirpath, ec=False, p384=False):
    """A self-signed certificate for threados.test: a SubjectAltName with
    the name and a wildcard, the CA flag (so it can serve as a trust
    root too) and a ten-year validity, so that it covers the guest's
    fixed clock. We do it with the openssl tool - it is everywhere TLS
    is tested."""
    import subprocess

    tag = "p384" if p384 else ("ec" if ec else "rsa")
    certfile = os.path.join(dirpath, "cert-%s.pem" % tag)
    keyfile = os.path.join(dirpath, "key-%s.pem" % tag)
    derfile = os.path.join(dirpath, "cert-%s.der" % tag)

    if p384:
        keyargs = ["-newkey", "ec", "-pkeyopt",
                   "ec_paramgen_curve:secp384r1", "-sha384"]
    elif ec:
        keyargs = ["-newkey", "ec", "-pkeyopt",
                   "ec_paramgen_curve:prime256v1"]
    else:
        keyargs = ["-newkey", "rsa:2048"]

    subprocess.run(
        ["openssl", "req", "-x509"] + keyargs + ["-keyout", keyfile,
         "-out", certfile, "-days", "3650", "-nodes",
         "-subj", "/CN=threados.test",
         "-addext", "subjectAltName=DNS:threados.test,DNS:*.threados.test"],
        check=True, capture_output=True, timeout=120)
    subprocess.run(
        ["openssl", "x509", "-in", certfile, "-outform", "DER", "-out", derfile],
        check=True, capture_output=True, timeout=60)

    with open(derfile, "rb") as f:
        return certfile, keyfile, f.read()


def make_chain_certs(dirpath):
    """A real chain: root -> intermediate -> the server's certificate.
    We trust only the root, so ThreadOS has to walk the whole path of
    signatures rather than merely compare one certificate."""
    import subprocess

    def run(args, **kw):
        subprocess.run(args, check=True, capture_output=True, timeout=120, **kw)

    j = lambda name: os.path.join(dirpath, name)

    with open(j("ca.ext"), "w") as f:
        f.write("basicConstraints=critical,CA:TRUE\n"
                "keyUsage=critical,keyCertSign,cRLSign\n")
    with open(j("leaf.ext"), "w") as f:
        f.write("basicConstraints=critical,CA:FALSE\n"
                "subjectAltName=DNS:threados.test\n")

    # the root
    run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
         "-keyout", j("root.key"), "-out", j("root.pem"), "-days", "3650",
         "-subj", "/CN=ThreadOS Test Root",
         "-addext", "basicConstraints=critical,CA:TRUE"])

    # the intermediate signed by the root
    run(["openssl", "req", "-new", "-newkey", "rsa:2048", "-nodes",
         "-keyout", j("int.key"), "-out", j("int.csr"),
         "-subj", "/CN=ThreadOS Test Intermediate"])
    run(["openssl", "x509", "-req", "-in", j("int.csr"),
         "-CA", j("root.pem"), "-CAkey", j("root.key"), "-CAcreateserial",
         "-out", j("int.pem"), "-days", "3650", "-extfile", j("ca.ext")])

    # the server's certificate signed by the intermediate
    run(["openssl", "req", "-new", "-newkey", "rsa:2048", "-nodes",
         "-keyout", j("leaf.key"), "-out", j("leaf.csr"),
         "-subj", "/CN=threados.test"])
    run(["openssl", "x509", "-req", "-in", j("leaf.csr"),
         "-CA", j("int.pem"), "-CAkey", j("int.key"), "-CAcreateserial",
         "-out", j("leaf.pem"), "-days", "3650", "-extfile", j("leaf.ext")])

    # the server sends its certificate together with the intermediate
    with open(j("chain.pem"), "w") as out:
        out.write(open(j("leaf.pem")).read())
        out.write(open(j("int.pem")).read())

    run(["openssl", "x509", "-in", j("root.pem"), "-outform", "DER",
         "-out", j("root.der")])
    with open(j("root.der"), "rb") as f:
        return j("chain.pem"), j("leaf.key"), f.read()


CERT_DER_ROOT = None     # the root of the chain (for the trust store)


def start_https_chain_server():
    """An HTTPS server presenting a two-level chain."""
    global CERT_DER_ROOT
    import ssl

    tmpdir = tempfile.mkdtemp(prefix="threados-chain-")
    try:
        chainfile, keyfile, root_der = make_chain_certs(tmpdir)
    except BaseException:
        return None, lambda: None
    CERT_DER_ROOT = root_der

    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def do_GET(self):
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(PAGE)))
            self.end_headers()
            self.wfile.write(PAGE)

        def log_message(self, *args):
            pass

    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(chainfile, keyfile)
    ctx.minimum_version = ssl.TLSVersion.TLSv1_3
    srv = socketserver.TCPServer(("0.0.0.0", 0), Handler)
    srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv.server_address[1], srv.shutdown


def start_https_server(ec=False, p384=False):
    """An HTTPS server (TLS 1.3) on the host with a certificate generated
    on the fly - with an RSA key or on the P-256 curve. Thanks to that
    the test needs nothing from outside.
    Returns (port, stop), or (None, stop) when openssl is missing."""
    global CERT_DER, CERT_DER_EC, CERT_DER_P384
    import ssl

    tmpdir = tempfile.mkdtemp(prefix="threados-tls-")
    try:
        certfile, keyfile, der = make_test_cert(tmpdir, ec, p384)
    except BaseException:                # a missing openssl is no reason to
        return None, lambda: None        # bring the whole test down
    if p384:
        CERT_DER_P384 = der
    elif ec:
        CERT_DER_EC = der
    else:
        CERT_DER = der

    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def do_GET(self):
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(PAGE)))
            self.end_headers()
            self.wfile.write(PAGE)

        def log_message(self, *args):
            pass

    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(certfile, keyfile)
    ctx.minimum_version = ssl.TLSVersion.TLSv1_3
    srv = socketserver.TCPServer(("0.0.0.0", 0), Handler)
    srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv.server_address[1], srv.shutdown


_AESREF = None          # the compiled helper, False when it cannot be built


def aesref(mode, key, nonce, aad, plain, taglen=16):
    """What OpenSSL makes of the same input - or None when the helper
    cannot be built here (no compiler, no libcrypto headers).

    The AES test needs an opinion that shares no line of code with ours;
    scripts/aesref.c is that opinion, compiled once per run."""
    global _AESREF

    if _AESREF is None:
        src = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "aesref.c")
        out = tempfile.mktemp(suffix="-aesref")
        r = subprocess.run(["cc", "-O2", "-o", out, src, "-lcrypto"],
                           capture_output=True)
        _AESREF = out if r.returncode == 0 else False

    if not _AESREF:
        return None

    r = subprocess.run([_AESREF, mode, key.hex(), nonce.hex(), aad.hex(),
                        plain.hex(), str(taglen)], capture_output=True)
    if r.returncode != 0:
        return None
    parts = r.stdout.decode().split()
    ct = bytes.fromhex(parts[0]) if parts and parts[0] else b""
    tag = bytes.fromhex(parts[1]) if len(parts) > 1 else b""
    return ct, tag


def start_suite_server(suite, dirpath=None):
    """An HTTPS server that will agree to ONE cipher suite.

    Python's ssl module cannot narrow the TLS 1.3 suites (set_ciphers
    reaches only the older ones), so this one is OpenSSL's own
    's_server'. It also says, in its log, what it agreed on - so the
    test does not have to take the client's word for which suite was
    used.

    Returns (port, stop, log) - log() gives back everything the server
    has printed so far. (None, stop, log) when openssl is missing.
    """
    tmpdir = dirpath or tempfile.mkdtemp(prefix="threados-suite-")
    try:
        certfile, keyfile, _ = make_test_cert(tmpdir)
    except BaseException:
        return None, lambda: None, lambda: ""

    sock = socket.socket()
    sock.bind(("0.0.0.0", 0))
    port = sock.getsockname()[1]
    sock.close()

    out = tempfile.mktemp(suffix="-sserver.log")
    fh = open(out, "wb")
    proc = subprocess.Popen(
        ["openssl", "s_server", "-tls1_3", "-ciphersuites", suite,
         "-cert", certfile, "-key", keyfile, "-accept", str(port),
         "-naccept", "8", "-WWW"],
        stdout=fh, stderr=subprocess.STDOUT, cwd=tmpdir)
    time.sleep(1.0)

    def log():
        fh.flush()
        try:
            with open(out, "rb") as r:
                return r.read().decode(errors="replace")
        except OSError:
            return ""

    def stop():
        proc.kill()
        fh.close()

    return port, stop, log


def start_http_server():
    """An HTTP server on the host - the guest reaches it through the slirp
    gateway (10.0.2.2), so the TCP test needs no network outside."""

    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def do_GET(self):
            blob = SERVED.get(self.path)
            if blob is not None:
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(len(blob)))
                self.end_headers()
                self.wfile.write(blob)
                return

            if self.path.startswith("/old"):      # a redirect
                self.send_response(302)
                self.send_header("Location", "/chunks")
                self.send_header("Content-Length", "0")
                self.end_headers()
                return

            if self.path.startswith("/chunks"):    # chunked encoding
                self.send_response(200)
                self.send_header("Content-Type", "text/html")
                self.send_header("Transfer-Encoding", "chunked")
                self.end_headers()
                for i in range(0, len(PAGE), 7):    # deliberately small pieces
                    part = PAGE[i:i + 7]
                    self.wfile.write(b"%x\r\n" % len(part) + part + b"\r\n")
                self.wfile.write(b"0\r\n\r\n")
                return

            if self.path.startswith("/huge"):
                self.send_response(200)
                self.send_header("Content-Type", "text/html")
                self.send_header("Content-Length", str(len(HUGE_PAGE)))
                self.end_headers()
                self.wfile.write(HUGE_PAGE)
                return

            if self.path.startswith("/links") or self.path.startswith("/second"):
                body = (LINK_PAGE2 if self.path.startswith("/second")
                        else LINK_PAGE)
                self.send_response(200)
                self.send_header("Content-Type", "text/html")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return

            if self.path.endswith(".png"):
                self.send_response(200)
                self.send_header("Content-Type", "image/png")
                self.send_header("Content-Length", str(len(PNG_IMAGE)))
                self.end_headers()
                self.wfile.write(PNG_IMAGE)
                return

            if self.path.endswith(".jpg") or self.path.endswith(".jpeg"):
                body = (JPEG_GRADIENT if "gradient" in self.path
                        else JPEG_IMAGE)
                self.send_response(200)
                self.send_header("Content-Type", "image/jpeg")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return

            if self.path.endswith(".bmp"):
                self.send_response(200)
                self.send_header("Content-Type", "image/bmp")
                self.send_header("Content-Length", str(len(BMP_IMAGE)))
                self.end_headers()
                self.wfile.write(BMP_IMAGE)
                return

            if self.path.startswith("/rootca") and CERT_DER_ROOT:
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(len(CERT_DER_ROOT)))
                self.end_headers()
                self.wfile.write(CERT_DER_ROOT)
                return

            if self.path.startswith("/p384cert") and CERT_DER_P384:
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(len(CERT_DER_P384)))
                self.end_headers()
                self.wfile.write(CERT_DER_P384)
                return

            if self.path.startswith("/eccert") and CERT_DER_EC:
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(len(CERT_DER_EC)))
                self.end_headers()
                self.wfile.write(CERT_DER_EC)
                return

            if self.path.endswith(".der") and CERT_DER:
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(len(CERT_DER)))
                self.end_headers()
                self.wfile.write(CERT_DER)
                return

            body = BIG_PAGE if self.path.startswith("/big") else PAGE
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *args):
            pass

    srv = socketserver.TCPServer(("0.0.0.0", 0), Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv.server_address[1], srv.shutdown


class Qemu:
    def __init__(self, kernel, initrd=None, disk=None, hostfwd_port=None,
                 append=None, ram_mib=None, sata=False, nvme=False,
                 usbkbd=False, usbmouse=False, no_i8042=False,
                 cdrom=None, serial=None, qemu="qemu-system-i386",
                 uefi=None, esp=None, disk2=None, audio=None, usbdisk=None,
                 usbhub=False, behind_hub=False, usbnic=False,
                 nic="rtl8139", cpus=None, devices=None, edid=None):
        self.qmp_path = tempfile.mktemp(suffix=".qmp")
        self.dump_path = tempfile.mktemp(suffix=".vga")
        # The 64-bit kernel needs a processor with long mode, and
        # qemu-system-i386 does not pretend to be one
        cmd = [qemu,
               "-display", "none", "-no-reboot",
               # The screen tells no resolution of its own (no EDID):
               # the desktop then starts at 640x480, the size every
               # click and every reading in these tests is laid out
               # for. The tests of the panel's own resolution turn it
               # back on with a size of their choosing (edid=).
               # The card is the standard VGA in its usual slot either
               # way - only what it says about the screen differs.
               "-vga", "none", "-device",
               "VGA,addr=02.0,edid=off" if edid is None else
               "VGA,addr=02.0,edid=on,xres=%d,yres=%d%s"
               % (edid[0], edid[1],
                  ",vgamem_mb=%d" % edid[2] if len(edid) > 2 else ""),
               # A fixed RTC date -> a deterministic test of the 'date' command.
               # It has to be later than the moment the test certificates
               # were issued, otherwise TLS will rightly reject them as
               # "not yet valid".
               "-rtc", "base=" + RTC_BASE,
               # The network card in user mode: the gateway 10.0.2.2
               # answers ARP and ICMP echo, so 'ping' can be tested with
               # no network on the host at all
               "-netdev",
               "user,id=n0" + (f",hostfwd=tcp::{hostfwd_port}-:{hostfwd_port}"
                               if hostfwd_port else ""),
               # The network card: RTL8139 by default, but the system
               # knows e1000 too - and that is the only difference in the
               # QEMU invocation, because which card sits in the socket
               # is decided by the netdev layer, not by the network code
               *(["-device", nic + ",netdev=n0"] if nic else []),
               "-qmp", f"unix:{self.qmp_path},server,nowait"]
        if ram_mib:
            cmd += ["-m", str(ram_mib)]
        if cpus:
            # More than one core. The firmware lists them in an ACPI
            # table, and the system wakes them with an INIT-SIPI signal
            cmd += ["-smp", str(cpus)]
        if usbkbd == "hub":
            # The keyboard NOT in the machine's socket but behind a hub
            # plugged into it - a docking station, seen from the
            # machine's side. Everything behind a hub was invisible
            # until the hubs themselves were understood.
            cmd += ["-device", "qemu-xhci,id=xhci",
                    "-device", "usb-hub,id=hub,bus=xhci.0,port=2",
                    "-device", "usb-kbd,bus=xhci.0,port=2.1"]
        elif usbkbd == "xhci":
            # An xHCI controller: the only one newer boards have. It is
            # the same keyboard, only on a different bus - and whether
            # the system sees it the same way is for the test to settle,
            # not for an assumption
            cmd += ["-device", "qemu-xhci,id=xhci",
                    "-device", "usb-kbd,bus=xhci.0"]
        elif usbkbd:
            # A USB keyboard instead of PS/2 - that is what every
            # machine newer than roughly 2010 looks like
            cmd += ["-usb", "-device", "usb-kbd"]
        if usbmouse == "xhci":
            # A pointer on the same bus. Newer laptops have no PS/2
            # port at all, and then this is the only pointer there is.
            if usbkbd != "xhci":
                cmd += ["-device", "qemu-xhci,id=xhci"]
            cmd += ["-device", "usb-mouse,bus=xhci.0"]
        elif usbmouse:
            cmd += ["-usb", "-device", "usb-mouse"]
        if usbnic:
            # The Ethernet card in a docking station: the same slirp
            # network, reached through the USB bus instead of the PCI
            # one. The card speaks RNDIS, which is what nearly every
            # USB Ethernet adapter offers.
            if usbkbd not in ("xhci", "hub") and usbmouse != "xhci":
                cmd += ["-device", "qemu-xhci,id=xhci"]
            cmd += ["-device", "usb-net,bus=xhci.0,netdev=n0"]
        if usbhub:
            # A hub in the socket, and the keyboard behind it. This is
            # what a docking station looks like from the machine's
            # side, and until hubs were understood everything behind
            # one was simply invisible.
            if usbkbd not in ("xhci", "hub") and usbmouse != "xhci":
                cmd += ["-device", "qemu-xhci,id=xhci"]
            if usbkbd != "hub":
                cmd += ["-device", "usb-hub,id=hub,bus=xhci.0,port=2"]
        if usbdisk:
            # A stick in the socket: a file on the host, a disk on the
            # bus. Whatever the system writes to it lands in that file,
            # which is how the test reads the result with somebody
            # else's tools.
            if (usbkbd not in ("xhci", "hub") and usbmouse != "xhci"
                    and not usbhub):
                cmd += ["-device", "qemu-xhci,id=xhci"]
            place = ",bus=xhci.0" + (",port=2.1" if behind_hub else "")
            cmd += ["-drive", "if=none,id=pendrive,format=raw,file=" + usbdisk,
                    "-device", "usb-storage,drive=pendrive" + place]
        if audio:
            # An HD Audio controller with a codec on it, and the sound
            # written to a file instead of a speaker. That file is the
            # whole point: what the machine plays can then be read back
            # here, sample by sample, instead of being taken on trust.
            cmd += ["-audiodev", "wav,id=snd0,path=" + audio,
                    "-device", "intel-hda",
                    "-device", "hda-output,audiodev=snd0"]
        if no_i8042:
            # A machine with no 8042 controller: no PS/2 keyboard and
            # no PS/2 mouse, which is what an ultrabook made in the
            # last few years actually is. Everything has to arrive over
            # USB or not at all.
            cmd += ["-machine", "pc,i8042=off"]
        self.serial_path = serial
        if serial:
            # When the console draws on the framebuffer, there is nothing
            # left to read in the VGA character buffer - we collect the
            # text from the serial port
            cmd += ["-serial", "file:" + serial]
        if uefi:
            # Instead of a BIOS, flash memory with UEFI firmware (OVMF):
            # the code separately (read-only) and separately the
            # variables the firmware writes - hence a copy of the
            # variables file for every run
            code, nvram = uefi
            cmd += ["-drive", "if=pflash,format=raw,unit=0,readonly=on,"
                              "file=" + code,
                    "-drive", "if=pflash,format=raw,unit=1,file=" + nvram]
        if esp:
            # A directory served as a FAT disk: QEMU builds the
            # filesystem out of it as it goes, and the firmware sees
            # exactly what it sees on a stick - an EFI system partition
            # with \\EFI\\BOOT\\BOOTX64.EFI in it. Nothing is loaded from
            # the command line here: whatever the loader does not find
            # by itself, the machine will not get.
            cmd += ["-drive", f"file=fat:rw:{esp},format=raw,media=disk"]
        if cdrom:
            # Booting as from a real board: the kernel is loaded by GRUB, not by
# QEMU
            cmd += ["-cdrom", cdrom, "-boot", "d"]
        elif not esp:
            cmd += ["-kernel", kernel]
            if append:
                cmd += ["-append", append]
            if initrd:
                cmd += ["-initrd", initrd]
        if disk and nvme:
            # An NVMe disk: with no controller in between - the device
            # sits straight on PCI Express
            cmd += ["-drive", f"id=nvm0,file={disk},if=none,format=raw",
                    "-device", "nvme,drive=nvm0,serial=threados"]
        elif disk and sata:
            # A disk behind an AHCI controller - that is what every newer board
# looks like
            cmd += ["-device", "ich9-ahci,id=ahci",
                    "-drive", f"id=sata0,file={disk},if=none,format=raw",
                    "-device", "ide-hd,drive=sata0,bus=ahci.0"]
        elif disk:
            cmd += ["-hda", disk]
        if devices:
            # Anything else, word for word. A machine that exists to
            # test one controller says exactly which controller and
            # what hangs off which of its ports; a keyword for every
            # such layout would be a second, worse copy of QEMU's own
            # syntax.
            cmd += list(devices)
        if disk2:
            # A second disk on the same controller, which has to be
            # declared by then - hence the position. A machine with
            # more than one disk is the only place where "which disk"
            # is a question at all, and a test that never asks it is a
            # test that never sees the answer.
            cmd += ["-drive", f"id=sata1,file={disk2},if=none,format=raw",
                    "-device", "ide-hd,drive=sata1,bus=ahci.1"]
        self.proc = subprocess.Popen(
            cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)

        # Wait until QEMU puts up the QMP socket (the start is sometimes slow)
        sock = socket.socket(socket.AF_UNIX)
        deadline = time.time() + 10
        while True:
            if self.proc.poll() is not None:
                raise RuntimeError("QEMU ended too early:\n"
                                   + self.proc.stderr.read().decode())
            try:
                sock.connect(self.qmp_path)
                break
            except (FileNotFoundError, ConnectionRefusedError):
                if time.time() > deadline:
                    raise
                time.sleep(0.2)
        # A time limit on an answer. Without it a read from the socket
        # waits forever: when QEMU dies or hangs in the middle of a
        # command, the whole test stops for good instead of reporting an
        # error - and there is nobody to wake it from that.
        sock.settimeout(self.QMP_TIMEOUT)
        self.sock = sock
        self.f = sock.makefile("rwb")
        self.f.readline()  # the QMP banner
        self.cmd({"execute": "qmp_capabilities"})
        _LIVE.append(self)

    # The longest command is a screenshot at high resolution -
    # seconds are enough with enormous room to spare
    QMP_TIMEOUT = 60

    def cmd(self, msg):
        self.f.write((json.dumps(msg) + "\n").encode())
        self.f.flush()
        while True:
            try:
                raw = self.f.readline()
            except (socket.timeout, TimeoutError):
                raise RuntimeError(
                    f"QMP did not answer in {self.QMP_TIMEOUT} s to: {msg}")
            if not raw:                  # a closed socket = QEMU died
                raise RuntimeError(f"QEMU disconnected during: {msg}")
            resp = json.loads(raw)
            if "return" in resp or "error" in resp:
                if "error" in resp:
                    raise RuntimeError(f"QMP error: {resp}")
                return resp

    def hmp(self, command):
        return self.cmd({"execute": "human-monitor-command",
                         "arguments": {"command-line": command}})

    def screen(self):
        self.hmp(f'pmemsave {VGA_ADDR} {VGA_BYTES} "{self.dump_path}"')
        raw = open(self.dump_path, "rb").read()
        lines = []
        for y in range(25):
            chars = raw[y * 160:(y + 1) * 160:2]
            lines.append(chars.decode("cp437", "replace").rstrip())
        return "\n".join(lines).rstrip()

    def wait_for(self, text, timeout=30):
        """Waits until the given text appears on the VGA screen.

        Thirty seconds, not fifteen: a full run starts four machines at
        once, and four kernels booting on one host take longer than one
        does. The wait is for a THING TO HAPPEN, so waiting longer
        weakens nothing - it only stops a busy host from being reported
        as a broken kernel."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            screen = self.screen()
            if text in screen:
                return screen
            time.sleep(0.3)
        raise RuntimeError(f"timeout: '{text}' never appeared on the screen")

    # The desktop's background where the tests look for it: the aurora
    # of the dark theme, which is what a machine nobody configured has
    DESK_BG = aurora_px(600, 300)

    def wait_desktop(self, timeout=25):
        """Waits until the compositor draws the desktop. In graphics mode
        the VGA text buffer (0xB8000) stops being a window onto what can
        be seen, so we look for readiness in pixels rather than in
        text."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                w, h, at = self.screen_raw()
                want = aurora_px(600, 300, w, h) if w >= 640 else self.DESK_BG
                if w >= 640 and all(abs(at(600, 300)[i] - want[i]) <= 40
                                    for i in range(3)):
                    self._size = (w, h)
                    return True
            except Exception:
                pass
            time.sleep(0.3)
        raise RuntimeError("timeout: the desktop never appeared")

    def sendkey(self, key):
        self.hmp(f"sendkey {key}")
        time.sleep(0.1)

    def mouse_move(self, dx, dy):
        """A PS/2 mouse carries only small deltas in one packet, so we
        split larger jumps into steps - otherwise the cursor lands
        somewhere other than where the test wanted."""
        while dx or dy:
            sx = max(-100, min(100, dx))
            sy = max(-100, min(100, dy))
            self.cmd({"execute": "input-send-event", "arguments": {"events": [
                {"type": "rel", "data": {"axis": "x", "value": sx}},
                {"type": "rel", "data": {"axis": "y", "value": sy}}]}})
            dx -= sx
            dy -= sy
            time.sleep(0.05)
        time.sleep(0.15)

    def mouse_home(self):
        """Pushes the cursor into the top left corner (the compositor holds
        it within the screen's bounds) and zeroes the tracked position -
        a calibration before clicking on particular places."""
        self.mouse_move(-800, -600)
        self.mx, self.my = 0, 0

    def mouse_button(self, down, button="left"):
        self.cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "btn", "data": {"button": button, "down": down}}]}})
        time.sleep(0.2)

    def mouse_wheel(self, notches=1):
        """Turns the wheel. Positive is away from the person, which is
        what two fingers sliding UP a touchpad send. QEMU's PS/2 mouse
        only sends this once the guest has asked for the IntelliMouse
        packet, so a test that sees it has proved the asking worked."""
        button = "wheel-up" if notches > 0 else "wheel-down"
        for _ in range(abs(notches)):
            self.cmd({"execute": "input-send-event", "arguments": {"events": [
                {"type": "btn", "data": {"button": button, "down": True}}]}})
            self.cmd({"execute": "input-send-event", "arguments": {"events": [
                {"type": "btn", "data": {"button": button, "down": False}}]}})
            time.sleep(0.15)
        time.sleep(0.3)

    # Tracking the cursor's position (the desktop starts in the middle of
# 320x200)
    mx, my = 160, 100

    def mouse_goto(self, tx, ty):
        self.mouse_move(tx - self.mx, ty - self.my)
        self.mx, self.my = tx, ty

    def mouse_click(self):
        self.mouse_button(True)
        self.mouse_button(False)

    def mouse_right_click(self):
        self.mouse_button(True, "right")
        self.mouse_button(False, "right")

    def screen_pixels(self):
        """A screenshot (QMP screendump) -> (scale_x, scale_y, a pixel
        function in framebuffer coordinates 320x200). QEMU doubles mode
        13h when displaying it, hence the scaling."""
        ppm = tempfile.mktemp(suffix=".ppm")
        self.cmd({"execute": "screendump", "arguments": {"filename": ppm}})
        data = open(ppm, "rb").read()
        os.unlink(ppm)
        idx, fields = 2, []
        while len(fields) < 3:
            while data[idx] in b" \t\n\r":
                idx += 1
            s = idx
            while data[idx] not in b" \t\n\r":
                idx += 1
            fields.append(int(data[s:idx]))
        idx += 1
        w, h, _ = fields
        px = data[idx:]
        sx, sy = w // VIDEO_W, h // VIDEO_H

        def at(fx, fy):
            x, y = fx * sx + 1, fy * sy + 1
            o = (y * w + x) * 3
            return (px[o], px[o + 1], px[o + 2])
        return sx, sy, at

    # --- the desktop, the way a person uses it -----------------------

    def click(self, x, y):
        """The pointer first, and the button only once it is there: the
        desktop decides what a press is by where the pointer is on the
        frame the button goes down, and a move and a press sent in one
        breath can land in one frame with the press first."""
        self.mouse_goto(x, y)
        time.sleep(0.4)
        self.mouse_click()

    def click_desktop(self):
        """A click on the bare desktop, where no window and no tile ever
        stands in these tests: nobody has the focus then, and the keys
        go to the kernel's shell"""
        self.click(self.size()[0] - 8, 34)
        time.sleep(0.3)

    def open_menu(self):
        """A click on the mark at the left end of the bar"""
        self.click(*LOGO_XY)
        time.sleep(0.6)

    # The size of the screen the desktop draws on: 640x480 under the
    # 32-bit kernel, what the loader set (800x600 in QEMU) under the
    # 64-bit one. Measured the first time something is placed by it.
    _size = None

    def size(self):
        if not self._size:
            w, h, _ = self.screen_raw()
            if w >= 640:
                self._size = (w, h)
        return self._size or (SCREEN_W, SCREEN_H)

    def _wh(self, kw):
        w, h = self.size()
        kw.setdefault("w", w)
        kw.setdefault("h", h)
        return kw

    # How many windows the dock holds: the kernel's two, and one more
    # for every program started through launch_dock()/launch_menu(). A
    # test that opens or closes windows some other way says so here.
    n_win = 2

    def launch_menu(self, name):
        """Starts a program from the menu, by the name it runs under"""
        self.open_menu()
        self.click(MENU_X0 + 60, menu_y(name))
        self.n_win += 1

    def launch_dock(self, k, n_w=None, **kw):
        """Starts the k-th program in the dock. Where its tile is depends
        on how many windows the dock holds as well (n_w, by default what
        this object has counted) - the dock is as wide as what is in it."""
        self.click(*dock_tile(k, n_w=self.n_win if n_w is None else n_w,
                              **self._wh(kw)))
        self.n_win += 1

    def dock_windows(self, want=None, n_l=DOCK_N):
        """How many window tiles the dock holds, read off the screen:
        every window's tile has a dot under it, and the dock is as wide
        as what is in it - so for the right count, and only for it,
        there is a dot under every place the layout gives a window."""
        _, _, at = self.screen_raw()
        t = theme("dark")
        for n in range(MAX_DOCK_WINDOWS, 0, -1):
            g = dock_geom(n_l=n_l, n_w=n, want=want, **self._wh({}))
            ok = True
            for j in range(n):
                tx, ty = g["tiles"][n_l + j]
                p = at(tx + g["tile"] // 2, ty + g["tile"] + 1 + DOCK_DOT // 2)
                if not (near(p, t["dot"], 60) or near(p, t["accent"], 60)):
                    ok = False
                    break
            if ok:
                return n
        return 0

    def dock_focus(self, n_w, n_l=DOCK_N, want=None):
        """Which window's tile has the accent dot under it - the window
        with the focus, counted among the windows on this desktop; -1
        when none has"""
        _, _, at = self.screen_raw()
        g = dock_geom(n_l=n_l, n_w=n_w, want=want, **self._wh({}))
        acc = theme("dark")["accent"]
        for j in range(n_w):
            tx, ty = g["tiles"][n_l + j]
            if near(at(tx + g["tile"] // 2,
                       ty + g["tile"] + 1 + DOCK_DOT // 2), acc, 50):
                return j
        return -1

    def read_hint(self, top):
        """The hint whose box has its top edge at 'top': the box is found
        by its colour - the longest run of it across the screen - and
        the text read from where the desktop puts it, eight pixels into
        the box"""
        w, _, at = self.screen_raw()
        t = theme("dark")
        y = top + 3                       # above the letters
        best, run = (0, 0), 0
        for x in range(w + 1):
            if x < w and near(at(x, y), t["hint_bg"], 12):
                run += 1
                if run > best[0]:
                    best = (run, x - run + 1)
            else:
                run = 0
        if best[0] < 20:
            return ""
        x0 = best[1] - 1                  # the box's edge, under its line
        return self.read_ui(x0 + 8, ui_baseline_in(top, HINT_H),
                            width=best[0], fg=t["hint_text"])

    def dock_hint(self, k, n_l=DOCK_N, n_w=2, want=None):
        """Points at the k-th tile and reads the name that comes up over
        it"""
        g = dock_geom(n_l=n_l, n_w=n_w, want=want, **self._wh({}))
        tx, ty = g["tiles"][k]
        cx = tx + g["tile"] // 2
        self.mouse_goto(cx, ty + 5)
        time.sleep(1.5)
        return self.read_hint(g["y"] - 6 - HINT_H)

    def is_background(self, x, y, tol=6, dark=True):
        """Is (x, y) the bare aurora - no window, no shadow over it?"""
        w, h, at = self.screen_raw()
        return near(at(x, y), aurora_px(x, y, w, h, dark=dark), tol)

    def wait_background(self, x, y, tol=6, timeout=4, dark=True):
        w, h = self.size()
        return self.wait_pixel(x, y, aurora_px(x, y, w, h, dark=dark), tol,
                               timeout)

    def has_focus(self, wx, wy):
        """Does the window at (wx, wy) have the focus? Its close light is
        red then, and grey when it has not - so the pointer must not be
        over the lights when this is asked."""
        _, _, at = self.screen_raw()
        return near(at(*light_xy(wx, wy, 0)), rgb(LIGHT_CLOSE), 30)

    def wait_focus(self, wx, wy, want=True, timeout=4):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.has_focus(wx, wy) == want:
                return True
            time.sleep(0.2)
        return False

    def wait_pixel(self, x, y, want, tol=40, timeout=4):
        """Waits until the pixel (x,y) reaches the colour 'want' (with a
        tolerance). It removes flakiness: the hi-res compositor refreshes
        more slowly than events arrive. Returns True/False."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            _, _, at = self.screen_raw()
            p = at(x, y)
            if all(abs(p[i] - want[i]) <= tol for i in range(3)):
                return True
            time.sleep(0.2)
        return False

    def screen_raw(self):
        """A screenshot in native coordinates (with no scaling) -
        (width, height, a pixel function). For the VESA modes."""
        ppm = tempfile.mktemp(suffix=".ppm")
        self.cmd({"execute": "screendump", "arguments": {"filename": ppm}})
        data = open(ppm, "rb").read()
        os.unlink(ppm)
        idx, fields = 2, []
        while len(fields) < 3:
            while data[idx] in b" \t\n\r":
                idx += 1
            s = idx
            while data[idx] not in b" \t\n\r":
                idx += 1
            fields.append(int(data[s:idx]))
        idx += 1
        w, h, _ = fields
        px = data[idx:]
        self._last_pixels = px

        def at(x, y):
            o = (y * w + x) * 3
            return (px[o], px[o + 1], px[o + 2])
        return w, h, at


    def read_ui(self, x, baseline, width=320, charset=None, px=None,
                face="sans", under=None, fg=None, screen=None, colours=None):
        """Text the desktop drew in its TrueType lettering, starting at
        pen position x on the given baseline - what the kernel's
        ui_text() would have been called with.

        Read one glyph at a time, the way the kernel drew it: at the pen,
        every character is tried - its FreeType glyph mixed with the
        background exactly as the kernel mixes - and the one that best
        explains the pixels between this pen position and the next is
        taken; then the pen moves on by that character's advance. What
        has been read so far is painted into the expectation, so a glyph
        that leans into its neighbour does not count against it. The
        text ends where nothing but background follows.

        The background is the commonest colour in the band and the text
        colour the one furthest from it - a fully covered pixel is
        exactly the text colour, so the extreme is the right one.

        'px' and 'face' say which lettering: the bar, the titles and the
        menu are a size up from the rest, and titles are bold. 'under',
        when given, says what lies under the text pixel by pixel - glass
        is not one colour, and text on it is mixed with what is really
        there. 'fg' is the text's colour when it is known - a short
        text, a single '3', may not have two pixels wholly covered to
        learn it from. 'screen' is a (w, h, at) already taken, for
        reading many things off one picture. 'colours', when fg is not
        known, are the colours the text may be in: the one the inkiest
        pixel lies on the way to is taken."""
        font = UiFont.shared(px, face)
        w, h, at = screen or self.screen_raw()
        top, bot = baseline - font.ascent, baseline + font.descent
        x1 = min(w, x + width)
        if x < 0 or top < 0 or bot > h or x1 <= x:
            return ""
        # The colours come from the beginning of the band only: text
        # starts at the pen, while what stands further right - the
        # buttons of a title bar, a red close among them - is not text
        # and must not be taken for its colour
        counts = {}
        for yy in range(top, bot):
            for xx in range(x, min(x1, x + 120)):
                p = at(xx, yy)
                counts[p] = counts.get(p, 0) + 1
        bg = max(counts, key=counts.get)
        far = [(sum(abs(a - b) for a, b in zip(p, bg)), p)
               for p, c in counts.items() if c >= 2]
        if not far or max(far)[0] < 60:
            return ""                       # nothing but background
        if not fg and colours:
            fg = ui_text_colour(bg, max(far)[1], colours)
        fg = fg or max(far)[1]

        # The band once, as rows of tuples: everything below reads it
        # thousands of times
        # A few pixels to the left as well: a glyph may start before
        # the pen (DejaVu's 'T' does, by a pixel)
        bx = max(0, x - 4)
        band = [[at(xx, yy) for xx in range(bx, x1)] for yy in range(top, bot)]
        if under:
            painted = [[under(xx, yy) for xx in range(bx, x1)]
                       for yy in range(top, bot)]
        else:
            painted = [[bg] * (x1 - bx) for _ in range(bot - top)]
        chars = charset or UiFont.CHARSET
        pen = x << 6
        out = []
        blanks = 0
        while True:
            gx = (pen + 32) >> 6
            if gx >= x1:
                break
            best = None
            for ch in chars:
                adv, left, gtop, gw, gh, a = font.glyph(ch)
                nx = (pen + adv + 32) >> 6
                x_lo = min(gx, gx + left)
                x_hi = max(nx, gx + left + gw)
                if x_hi > x1 or x_hi <= x_lo or x_lo < bx:
                    continue
                err = 0
                gy0 = baseline - gtop - top      # the glyph's first row, in the band
                for r in range(bot - top):
                    prow, brow = painted[r], band[r]
                    gr = r - gy0
                    inside = 0 <= gr < gh
                    for xx in range(x_lo, x_hi):
                        exp = prow[xx - bx]
                        if inside:
                            k = xx - gx - left
                            if 0 <= k < gw:
                                v = a[gr * gw + k]
                                if v:
                                    exp = ui_mix(fg, exp, v)
                        got = brow[xx - bx]
                        err += (abs(exp[0] - got[0]) + abs(exp[1] - got[1])
                                + abs(exp[2] - got[2]))
                        if best is not None and err > best[0] * (x_hi - x_lo) * (bot - top):
                            break
                    if best is not None and err > best[0] * (x_hi - x_lo) * (bot - top):
                        break
                score = err / ((x_hi - x_lo) * (bot - top))
                if best is None or score < best[0]:
                    best = (score, ch, adv, left, gtop, gw, gh, a)
            if best is None or best[0] > 12:
                break
            score, ch, adv, left, gtop, gw, gh, a = best
            if ch == " ":
                blanks += 1
                if blanks > 2:
                    break
            else:
                blanks = 0
            gy0 = baseline - gtop - top
            for r in range(gh):
                for k in range(gw):
                    v = a[r * gw + k]
                    rr, cc = gy0 + r, gx + left + k - bx
                    if v and 0 <= rr < bot - top and 0 <= cc < x1 - bx:
                        painted[rr][cc] = ui_mix(fg, painted[rr][cc], v)
            out.append(ch)
            pen += adv
        return "".join(out).rstrip()

    def find_ui(self, x0, x1, baseline, charset=None, px=None, face="sans",
                under=None, fg=None, screen=None, colours=None):
        """Text whose start is not known - a title centred in its bar,
        the clock set against the end of the bar: the first column
        between x0 and x1 where anything stands out from the background
        is where the ink begins, and the pen is a pixel or two either
        side of it. Each of those is read and the longest reading wins."""
        font = UiFont.shared(px, face)
        w, h, at = screen = screen or self.screen_raw()
        top, bot = baseline - font.ascent, baseline + font.descent
        x1 = min(x1, w)
        if top < 0 or bot > h or x1 <= x0:
            return ""
        counts = {}
        for yy in range(top, bot):
            for xx in range(x0, x1):
                p = at(xx, yy)
                counts[p] = counts.get(p, 0) + 1
        bg = max(counts, key=counts.get)
        # The ground the ink stands out from: what lies under the text,
        # when that is known (an aurora is not one colour), otherwise
        # the band's commonest colour
        ground = under or (lambda xx, yy: bg)
        start = None
        for xx in range(x0, x1):
            if any(sum(abs(a - b) for a, b in zip(at(xx, yy), ground(xx, yy)))
                   > 90 for yy in range(top, bot)):
                start = xx
                break
        if start is None:
            return ""
        best = ""
        # A glyph may begin well right of its pen - a '1' in large type
        # by a sixth of the size - so the pen is looked for further left
        reach = 2 + font.px // 6
        for pen in range(start - reach, start + 2):
            got = self.read_ui(pen, baseline, width=x1 - pen,
                               charset=charset, px=px, face=face,
                               under=under, fg=fg, screen=screen,
                               colours=colours).lstrip()
            # A pen too far left reads a space first, and then the text
            # a little off - a reading that has to start with a space is
            # not where the text starts
            if len(got) > len(best):
                best = got
        return best

    def gset_row(self, cx, cy, r, chosen=False, dark=True, screen=None):
        """(name, value) on row r of the settings window whose content
        starts at (cx, cy). The chosen row wears the accent with its
        text on it; the others are text and a quieter value."""
        t = theme("dark" if dark else "light")
        scr = screen or self.screen_raw()
        base = cy + ui_font_baseline_in(UD_PX, GSET_ROW_Y + r * GSET_ROW_H,
                                        GSET_ROW_H)
        name = self.read_ui(cx + 14, base, GSET_VAL_X - 18, px=UD_PX,
                            fg=t["accent_text" if chosen else "app_text"],
                            screen=scr)
        value = self.read_ui(cx + GSET_VAL_X, base, GSET_W - GSET_VAL_X - 10,
                             px=UD_PX,
                             fg=t["accent_text" if chosen else "app_dim"],
                             screen=scr)
        return name, value

    def gclock_time(self, cx, cy, dark=True, screen=None):
        """The 'HH:MM:SS' on the clock whose content starts at (cx, cy):
        bold, centred in its band"""
        t = theme("dark" if dark else "light")
        base = cy + ui_font_baseline_in(GCLOCK_PX, GCLOCK_TIME_Y,
                                        GCLOCK_TIME_H)
        return self.find_ui(cx, cx + GCLOCK_W, base, charset="0123456789:",
                            px=GCLOCK_PX, face="bold", fg=t["app_text"],
                            screen=screen)

    def gfiles_rows(self, cx, cy, cw=None, ch=None, dark=True, screen=None):
        """The file manager's list as it stands: (name, size, picked) per
        row. A folder's name is bold, a file's plain, and its size - or
        the word 'folder' - is set against the right edge; the picked
        row wears the accent with its text on it."""
        t = theme("dark" if dark else "light")
        w, h, at = scr = screen or self.screen_raw()
        cw, ch = cw or GFILES_W, ch or GFILES_H
        out = []
        r = 0
        while GFILES_ROW_Y0 + (r + 1) * GFILES_ROW_H <= ch - GFILES_FOOT_H:
            y = GFILES_ROW_Y0 + r * GFILES_ROW_H
            picked = near(at(cx + 8, cy + gfiles_row_y(r)), t["accent"], 6)
            base = cy + ui_font_baseline_in(UD_PX, y, GFILES_ROW_H)
            ink = t["accent_text" if picked else "app_text"]
            name = ""
            for face in ("sans", "bold"):
                got = self.read_ui(cx + GFILES_NAME_X, base, 200, px=UD_PX,
                                   face=face, fg=ink, screen=scr)
                if len(got) > len(name):
                    name = got
            if not name:
                break
            size = self.find_ui(cx + cw - 110, cx + cw - 6, base, px=UD_PX,
                                fg=t["accent_text" if picked else "app_dim"],
                                screen=scr)
            out.append((name, size, picked))
            r += 1
        return out

    def gfiles_heading(self, cx, cy, dark=True):
        """Where the file manager is: the bold heading at its top left"""
        t = theme("dark" if dark else "light")
        return self.read_ui(cx + 12, cy + ui_font_baseline_in(
            13, 0, GFILES_HEAD_H), 150, px=13, face="bold", fg=t["app_text"])

    def ug_pick_read(self, cx, cy, ch, dark=True):
        """The picker of a program built on the kit: (its title, the
        names on its list)"""
        t = theme("dark" if dark else "light")
        p = ug_pick(ch)
        w, h, at = scr = self.screen_raw()
        title = self.read_ui(cx + 22, cy + p["title"], 300, px=UD_PX,
                             face="bold", fg=t["app_text"], screen=scr)
        names = []
        for r in range(UG_PICK_ROWS):
            chosen = near(at(cx + 24, cy + p["row_y"](r)), t["accent"], 6)
            base = cy + ui_font_baseline_in(UD_PX, p["list_y"] + r * UG_ROW,
                                            UG_ROW)
            names.append(self.read_ui(
                cx + 26, base, 300, px=UD_PX, screen=scr,
                fg=t["accent_text" if chosen else "app_text"]))
        return title, names

    def ug_question(self, cx, cy, ch, dark=True):
        """The question a program built on the kit is asking - the bold
        line at the top of its panel"""
        t = theme("dark" if dark else "light")
        return self.read_ui(cx + 22, cy + ug_modal(ch)["title"], 300,
                            px=UD_PX, face="bold", fg=t["app_text"])

    def gfiles_status(self, cx, cy, ch=None, dark=True):
        """The status line along the bottom of the file manager"""
        t = theme("dark" if dark else "light")
        base = cy + ui_font_baseline_in(11, (ch or GFILES_H) - GFILES_FOOT_H,
                                        GFILES_FOOT_H)
        return self.read_ui_lines(cx + 8, cx + GFILES_W - 4, [base], px=11,
                                  colours=[t["app_text"], t["app_dim"],
                                           t["accent"]])

    def gimg_status(self, cx, cy, cw, ch, dark=True):
        """What the picture viewer's status line says"""
        t = theme("dark" if dark else "light")
        base = cy + ui_font_baseline_in(GIMG_STATUS_PX, ch - GIMG_STATUS_H,
                                        GIMG_STATUS_H)
        return self.read_ui(cx + 8, base, cw - 12, px=GIMG_STATUS_PX,
                            fg=t["app_text"])

    def gset_note(self, cx, cy, ch=None, dark=True):
        """The line at the bottom of the settings window: whether the
        last change was saved (bold, good or bad news), or - while the
        password is asked for - what it is asked for (small and quiet)"""
        t = theme("dark" if dark else "light")
        base = cy + (ch or GSET_H) - GSET_NOTE_DY
        scr = self.screen_raw()
        news = self.read_ui(cx + 12, base, GSET_W - 24, px=UD_PX, face="bold",
                            colours=[t["app_good"], t["app_danger"]],
                            screen=scr)
        quiet = self.read_ui(cx + 12, base, GSET_W - 24, px=11,
                             fg=t["app_dim"], screen=scr)
        return news if len(news) >= len(quiet) else quiet

    def term_rows(self, cx, cy, cols=TERM_COLS, rows=TERM_ROWS,
                  screen=None):
        """The terminal whose content starts at (cx, cy), a line per row"""
        return self.read_cells(cx + TERM_PAD, cy + TERM_PAD, cols, rows,
                               cell=TERM_CELL, row_h=TERM_ROW_H,
                               base=TERM_BASE, px=TERM_PX, face="mono",
                               screen=screen)

    _cell_masks = {}

    def read_cells(self, ox, oy, cols, rows, cell=8, row_h=16, base=12,
                   px=12, face="mono", screen=None):
        """Text on a grid of cells - a terminal's, drawn with ud_cells():
        every character's pen at the left edge of its cell, the baseline
        'base' below the top of its row. One line per row, trailing
        blanks dropped.

        Each cell is read on its own, so text in any colour on any
        ground - a selection's accent, the prompt's green - reads the
        same: the ground is the cell's commonest colour, the ink its
        inkiest pixel, and how far every pixel is from one towards the
        other is held up against each character's FreeType coverage,
        placed where the kernel places it. The two bottom rows of a
        cell are left out, and its last column: that is where a cursor
        is drawn - under a letter, or between two - where no letter
        reaches."""
        w, h, at = screen or self.screen_raw()
        key = (cell, row_h, base, px, face)
        if key not in Qemu._cell_masks:
            font = UiFont.shared(px, face)
            masks = {}
            for code in range(33, 127):
                ch = chr(code)
                adv, left, top, gw, gh, a = font.glyph(ch)
                m = [0] * ((cell - 1) * (row_h - 2))
                for r in range(gh):
                    yy = base - top + r
                    if not 0 <= yy < row_h - 2:
                        continue
                    for k in range(gw):
                        xx = left + k
                        if 0 <= xx < cell - 1:
                            m[yy * (cell - 1) + xx] = a[r * gw + k]
                masks[ch] = m
            Qemu._cell_masks[key] = masks
        masks = Qemu._cell_masks[key]
        out = []
        for r in range(rows):
            line = ""
            for c in range(cols):
                x0, y0 = ox + c * cell, oy + r * row_h
                if x0 + cell > w or y0 + row_h > h:
                    line += " "
                    continue
                p = [at(x0 + i, y0 + j) for j in range(row_h - 2)
                     for i in range(cell - 1)]
                counts = {}
                for v in p:
                    counts[v] = counts.get(v, 0) + 1
                bg = max(counts, key=counts.get)
                far = max(p, key=lambda v: sum(abs(a - b)
                                               for a, b in zip(v, bg)))
                d = [far[i] - bg[i] for i in range(3)]
                n2 = sum(v * v for v in d)
                if sum(abs(v) for v in d) < 60 or not n2:
                    line += " "
                    continue
                cov = [max(0, min(255, 255 * sum((v[i] - bg[i]) * d[i]
                                                  for i in range(3)) // n2))
                       for v in p]
                best, best_err = "?", None
                for ch, m in masks.items():
                    err = sum(abs(a - b) for a, b in zip(cov, m))
                    if best_err is None or err < best_err:
                        best, best_err = ch, err
                line += best
            out.append(line.rstrip())
        return out

    def read_ui_lines(self, x0, x1, baselines, px=None,
                      faces=("sans", "bold"), screen=None, gap=14,
                      colours=None):
        """A page of TrueType text - what a program wrote into its
        window through user/udraw.h - one line per baseline, the lines
        joined with newlines.

        A line is not one piece of text: a name and a size in a column
        further right, a label and its value. So each line is cut where
        the ink stops for 'gap' pixels or more, every piece is read on
        its own - from where its ink starts, and in its own colour,
        since a line may be text with a quieter number after it - and
        the pieces are joined with a space. A piece is tried in each of
        'faces' - a heading is bold - and the longest reading wins."""
        w, h, at = screen = screen or self.screen_raw()
        font = UiFont.shared(px, faces[0])
        x1 = min(x1, w)
        lines = []
        for base in baselines:
            top, bot = base - font.ascent, base + font.descent
            if top < 0 or bot > h:
                lines.append("")
                continue
            counts = {}
            for yy in range(top, bot):
                for xx in range(x0, x1):
                    p = at(xx, yy)
                    counts[p] = counts.get(p, 0) + 1
            bg = max(counts, key=counts.get)
            ink = [any(sum(abs(a - b) for a, b in zip(at(xx, yy), bg)) > 40
                       for yy in range(top, bot)) for xx in range(x0, x1)]
            pieces, start, last = [], None, None
            for i, on in enumerate(ink):
                if on:
                    if start is None:
                        start = i
                    elif i - last > gap:
                        pieces.append((start, last))
                        start = i
                    last = i
            if start is not None:
                pieces.append((start, last))
            words = []
            for a, b in pieces:
                got = ""
                for f in faces:
                    one = self.find_ui(x0 + a, x0 + b + 8, base, px=px,
                                       face=f, screen=screen, colours=colours)
                    if len(one) > len(got):
                        got = one
                if got:
                    words.append(got)
            lines.append(" ".join(words))
        return "\n".join(lines)

    def window_title(self, wx, wy, ww=None):
        """A window's name as its title bar shows it: kernel/desktop.c
        draws it in bold, centred in the bar and clear of the three
        lights. Without the window's width it is read to a fixed
        distance."""
        x0 = wx + LIGHT_X + 2 * LIGHT_STEP + LIGHT_D + 6
        x1 = wx + ww - 4 if ww else x0 + 300
        return self.find_ui(x0, x1, title_baseline(wy), px=TITLE_PX,
                            face="bold")

    def read_text(self, x0, y0, cols, rows, line_h=8):
        """Text drawn in the 8x8 font ANYWHERE on the screen.

        The console is not the only thing in this system written with
        that font: every window is. So the reading is the same in both
        cases and lives in one place - a block of cols x rows
        characters starting at (x0, y0), with lines every line_h pixels
        (windows put them further apart than the console does).

        We read it the way a human would: the colour most common across
        the whole block is the background, the rest are glyphs - and
        the pattern of eight bytes is looked up in the same font the
        kernel drew with.

        The background is computed for the WHOLE block, not for a
        single cell: there are characters with more glyph pixels than
        background ones ('0', 'N', 'B', 'W'), and computed separately
        they would come out inverted.
        """
        w, h, _ = self.screen_raw()
        px = self._last_pixels
        if x0 < 0 or y0 < 0 or x0 + cols * 8 > w or y0 + rows * line_h > h:
            return ""

        counts = {}
        for y in range(rows * line_h):
            o = ((y0 + y) * w + x0) * 3
            band = px[o:o + cols * 8 * 3]
            for i in range(0, len(band), 3):
                p = band[i:i + 3]
                counts[p] = counts.get(p, 0) + 1
        if not counts:
            return ""
        background = max(counts, key=counts.get)

        font = font8x8()
        out = []
        for row in range(rows):
            chars = []
            for col in range(cols):
                pattern = []
                for y in range(8):
                    o = ((y0 + row * line_h + y) * w + x0 + col * 8) * 3
                    band = px[o:o + 24]
                    b = 0
                    for x in range(8):
                        if band[x * 3:x * 3 + 3] != background:
                            b |= 1 << x
                    pattern.append(b)
                chars.append(font.get(tuple(pattern), "?"))
            out.append("".join(chars).rstrip())
        return "\n".join(out).rstrip()

    def screen_fbcon(self):
        """The console text read from the FRAMEBUFFER.

        GRUB, seeing a request for a video mode in the kernel's header,
        always gives graphics - the character memory at 0xB8000 then
        stays empty and screen() reads nothing out of it. The console
        draws itself then, in an 8x8 font, in an 80x25 block in the
        middle of the screen (kernel/fbcon.c).
        """
        w, h, _ = self.screen_raw()
        return self.read_text((w - 80 * 8) // 2, (h - 25 * 8) // 2, 80, 25)

    def wait_fbcon(self, text, timeout=15):
        """Waits until the text appears on the console drawn in the
        framebuffer. Returns the whole screen (also after the time runs
        out - the assertion belongs to the caller)."""
        end = time.time() + timeout
        screen_text = ""
        while time.time() < end:
            screen_text = self.screen_fbcon()
            if text in screen_text:
                return screen_text
            time.sleep(0.4)
        return screen_text

    # What a character is called on the way to QEMU. Anything with a
    # name of its own is here; a capital letter is handled in
    # type_line, because there are twenty-six of those and one rule.
    KEYNAMES = {" ": "spc", "\n": "ret", "-": "minus", ".": "dot",
                ":": "shift-semicolon", ";": "semicolon",
                "/": "slash", ">": "shift-dot", "<": "shift-comma",
                "|": "shift-backslash", "&": "shift-7",
                "#": "shift-3", "!": "shift-1", "@": "shift-2",
                "$": "shift-4", "%": "shift-5", "^": "shift-6",
                "*": "shift-8", "(": "shift-9", ")": "shift-0",
                "_": "shift-minus", "+": "shift-equal", "=": "equal",
                "?": "shift-slash", "'": "apostrophe",
                '"': "shift-apostrophe", ",": "comma",
                "[": "bracket_left", "]": "bracket_right",
                "{": "shift-bracket_left", "}": "shift-bracket_right",
                "\\": "backslash", "`": "grave_accent",
                "~": "shift-grave_accent"}

    def type_line(self, text, enter=True):
        """Types the text, and Enter after it unless told otherwise -
        an editor is a place where what is typed does not end with
        pressing Enter."""
        for ch in text:
            if ch.isupper():
                # QEMU has no key called 'T' - a capital is the shift
                # key and the letter, and typing one without saying so
                # simply loses it
                self.sendkey("shift-" + ch.lower())
                continue
            self.sendkey(self.KEYNAMES.get(ch, ch))
        if enter:
            self.sendkey("ret")
        time.sleep(0.3)

    def serial(self):
        """Everything the kernel wrote to the serial port."""
        if not self.serial_path or not os.path.exists(self.serial_path):
            return ""
        with open(self.serial_path, "rb") as fh:
            return fh.read().decode("latin1")

    def command(self, cmd, text=None, timeout=20):
        """Types a command and hands back WHAT ARRIVED on the port after it.

        Without cutting off the older content a false alarm is easy: the
        shell echoes the typed line, so the file name is on the port even
        when the command found nothing - and the previous command's
        result lies there until the end of the run."""
        start = len(self.serial())
        self.type_line(cmd)
        if text is None:
            time.sleep(0.5)
            return self.serial()[start:]
        end = time.time() + timeout
        while time.time() < end:
            fresh = self.serial()[start:]
            if text in fresh:
                return fresh
            time.sleep(0.3)
        return self.serial()[start:]

    def wait_serial(self, text, timeout=20):
        deadline = time.time() + timeout
        while time.time() < deadline:
            out = self.serial()
            if text in out:
                return out
            time.sleep(0.3)
        raise RuntimeError(f"timeout: '{text}' did not appear on the serial port")

    def wait_exit(self, timeout=20):
        """Waits until QEMU ends by itself (a power off)."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.proc.poll() is not None:
                return True
            time.sleep(0.3)
        return False

    def close(self):
        if self in _LIVE:
            _LIVE.remove(self)
        self.proc.kill()
        for p in (self.qmp_path, self.dump_path):
            if os.path.exists(p):
                os.unlink(p)


# --- Counting the results -------------------------------------------
# Every group runs in its own process and prints its results to the
# output. The runner adds them up - thanks to that the groups do not
# have to care about one another or pass anything between themselves.
failures = []


def pmm_used(screen_text):
    """How many frames are taken according to the last PMM line on the
    'mem' screen."""
    m = re.findall(r"PMM:\s+(\d+)/", screen_text)
    return int(m[-1]) if m else None


# The machines running now - so that a failed check can show what was
# on their screens. Set PTEST_SHOTS to a directory and every FAIL leaves
# a picture there: a failure in a test that reads pixels is otherwise a
# sentence about a screen nobody saw.
_LIVE = []


def check(name, cond):
    print(("OK:   " if cond else "FAIL: ") + name)
    if not cond:
        failures.append(name)
        where = os.environ.get("PTEST_SHOTS")
        for i, q in enumerate(_LIVE if where else []):
            try:
                path = os.path.join(where, "%s-%d-%d.ppm" % (
                    os.path.basename(sys.argv[0]).replace(".py", ""),
                    len(failures), i))
                q.cmd({"execute": "screendump",
                       "arguments": {"filename": path}})
                print("      the screen: " + path)
            except Exception:
                pass


def summary():
    """Ends the group's process: 0 = everything passed."""
    if failures:
        print(f"FAILED: {len(failures)} assertion(s) did not pass")
        return 1
    return 0



def make_iso64(workdir, iso, graphics=False, text=False):
    """Assembles a disc with the 64-bit kernel and its initrd.

    The 64-bit kernel cannot be started through -kernel: QEMU loads only
    Multiboot 1 images that way, and this is Multiboot 2. So every test
    has to build a disc with GRUB, and until now every one did it on its
    own. 'graphics' turns graphics mode on from the loader (the desktop
    and the console on the framebuffer). 'text' demands text mode: GRUB
    gives a framebuffer of its own accord and the console then draws in
    it instead of in the VGA character memory the tests look for at
    0xB8000.

    Returns True when the disc was created.
    """
    os.makedirs(workdir + "/boot/grub", exist_ok=True)
    shutil.copy(KERNEL64, workdir + "/boot/threados64.bin")
    mod = ""
    if INITRD64 and os.path.exists(INITRD64):
        shutil.copy(INITRD64, workdir + "/boot/initrd64.img")
        mod = "    module2 /boot/initrd64.img initrd64\n"
    header = ("insmod all_video\nset gfxpayload=keep\n" if graphics
                else "set gfxpayload=text\n" if text else "")
    with open(workdir + "/boot/grub/grub.cfg", "w") as fh:
        fh.write(header + "set timeout=0\nset default=0\n"
                 'menuentry "ThreadOS 64" {\n'
                 "    multiboot2 /boot/threados64.bin\n" + mod + "}\n")
    return subprocess.run(["grub-mkrescue", "-o", iso, workdir],
                          stdout=subprocess.DEVNULL,
                          stderr=subprocess.DEVNULL).returncode == 0


def no64():
    """The reason a 64-bit test cannot be run, or None when it can. Every
    64-bit group asks the same question."""
    if KERNEL64 is None or not os.path.exists(KERNEL64):
        return "brak obrazu 64-bitowego (make kernel64)"
    if shutil.which("grub-mkrescue") is None:
        return "brak grub-mkrescue"
    if shutil.which("qemu-system-x86_64") is None:
        return "brak qemu-system-x86_64"
    return None
