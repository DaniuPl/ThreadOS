#!/usr/bin/env python3
"""ThreadOS smoke test: a disk that holds TFS3 is moved to TFS4.

TFS3, the 64-file file system before TFS4, is no longer run on: the
first start of a kernel with TFS4 moves a disk off it, in place
(kernel/tfs3to4.c, docs/tfs4-plan.md stage 5). This is people's data,
so everything is checked from outside:

  an installed system - a GPT disk with a FAT32 EFI partition and TFS3
  in a partition of the THREADOS-TFS3 type - with two accounts and their
  files, written by the system itself ('format3', 'useradd', 'write', a
  2.4 MB file made by a program); the host reads it with TfsImage
  the next start says it moves it, keeps a copy on the EFI partition
  and moves every file: on the host, Linux's debugfs finds every
  directory and file TfsImage found, the same bytes and the same
  owners, the homes 0700; e2fsck -fn finds nothing to fix; the copy on
  the EFI partition (read by mtools) is the TFS3's bytes exactly, and
  the note that a move is under way is gone; the partition table is
  whole and its type unchanged (THREADOS-TFS3 is ours too)
  the start after that does not move anything again, and alice signs
  in with the password she had under TFS3
  a move cut short: the same TFS3, and the state a stop half way
  leaves - the copy and the note on the EFI partition, the partition's
  first 4 MiB overwritten - and the next start finishes it from the
  copy: every file again, e2fsck clean, the note gone
  TFS3 in a partition of Linux's type, on a disk whose EFI partition
  holds no file system: moved from memory alone (and said so), and the
  partition becomes of ThreadOS's type, in both copies of the table

Run on its own or through scripts/run_tests.py.
"""

import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import (check, summary, no64, make_gpt_disk, mtools_ok,
                   fat_format, fat_put, fat_get, fat_dir, fat_mkdirs,
                   gpt_read,
                   TfsImage, Ext2Image, e2fsprogs_missing, GUID_TFS4,
                   neighbours_intact)

PROMPT = "thread64"                  # ">" for a user, "#" for admin
MIB = 400
ESP_MIB = 100
ESP_START = 2048
P3_START = 2048 + ESP_MIB * 2048 + 409600       # make_gpt_disk's layout
P3_SECTORS = MIB * 2048 - 34 - P3_START + 1
TFS3_TYPE = b"THREADOS-TFS3\0\0\x01"            # kernel/gpt.c before TFS4
TFS4_TYPE = b"THREADOS-TFS4\0\0\x01"
BIG_KIB = 2400                                  # mkmap64: 2.4 MB


def boot(disk, log):
    q = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE, serial=log)
    return q, q.wait_serial(PROMPT, timeout=120)


def login(q, name, password):
    q.command("login " + name, "password:", timeout=20)
    out = q.command(password, "logged in as", timeout=30)
    return "logged in as " + name in out


def make_tfs3_system(disk, log, partition):
    """TFS3 in the partition, with admin, alice and bob and their files,
    all written by the system"""
    q, _ = boot(disk, log)
    ok = True
    try:
        q.command("sudo format3 %d" % partition, "password:", timeout=20)
        q.command("thread", "Type YES", timeout=20)
        out = q.command("YES", PROMPT, timeout=120)
        ok &= "this partition is a ThreadOS TFS3 now" in out
        ok &= login(q, "admin", "thread")
        for name in ("alice", "bob"):
            q.command("useradd " + name, "admin password:", timeout=20)
            q.command("thread", "new account password:", timeout=20)
            q.command(name + "pass", "again:", timeout=20)
            ok &= "account created" in q.command(name + "pass", "account",
                                                 timeout=30)
        q.command("write admin-note.txt kept by the administrator", PROMPT)
        ok &= login(q, "alice", "alicepass")
        q.command("write alice.txt alice-wrote-this-under-tfs3", PROMPT)
        q.command("mkdir letters", PROMPT)
        q.command("cd letters", PROMPT)
        q.command("write deep.txt a-file-two-levels-down", PROMPT)
        out = q.command("run mkmap64 big.bin %d" % BIG_KIB, "exit code",
                        timeout=120)
        ok &= "exit code 0" in out
        ok &= login(q, "bob", "bobpass")
        q.command("write bob.txt bobs-own", PROMPT)
    finally:
        q.close()
    return ok


def record(disk, base):
    """What TfsImage reads of the TFS3: {path: (is_dir, owner, bytes)}
    and the TFS3's own bytes, from its first block to its last"""
    img = TfsImage(disk, base)
    want = {}
    for i, ino in enumerate(img.inodes):
        if not ino["used"]:
            continue
        path = img.path(i)
        if path == "/swap.sys":          # made anew, not moved
            continue
        want[path] = (bool(ino["dir"]), ino["owner"],
                      None if ino["dir"] else img.read(path))
    return want, img.data[:img.nblocks * 1024]


def compare(label, want, img):
    """Every directory and file TFS3 held, as debugfs reads them"""
    wrong = []
    for path, (is_dir, owner, data) in sorted(want.items()):
        st = img.stat(path)
        if st is None:
            wrong.append(path + " missing")
            continue
        if st["type"] != ("directory" if is_dir else "regular"):
            wrong.append("%s is a %s" % (path, st["type"]))
        elif st["uid"] != owner:
            wrong.append("%s is uid %s, not %s" % (path, st["uid"], owner))
        elif not is_dir and img.read(path) != data:
            wrong.append(path + " differs")
    check("%s: all %d directories and files, the same bytes and owners "
          "(%s)" % (label, len(want), wrong[:4] or "none wrong"), not wrong)
    homes = [p for p, (d, _, _) in want.items()
             if d and p.count("/") == 2 and p.startswith("/home/")]
    modes = {p: (img.stat(p) or {}).get("mode") for p in homes}
    check("%s: the homes are their owners' alone (%s)"
          % (label, {p: oct(m or 0) for p, m in modes.items()}),
          homes and all(m == 0o700 for m in modes.values()))
    clean, problems = img.fsck()
    check("%s: e2fsck -fn finds nothing to fix (%s)" % (label, problems[:3]),
          clean)


def entry_type(disk, slot, backup=False):
    """The type GUID of a partition's entry, read raw from the table"""
    with open(disk, "rb") as fh:
        if backup:
            fh.seek(0, 2)
            fh.seek(fh.tell() - 33 * 512 + (slot - 1) * 128)
        else:
            fh.seek(2 * 512 + (slot - 1) * 128)
        return fh.read(16)


def installed(work):
    disk = os.path.join(work, "installed.img")
    log = os.path.join(work, "a.log")
    make_gpt_disk(disk, MIB, ESP_MIB, third_type=TFS3_TYPE)
    check("mtools puts a FAT32 on the EFI partition",
          fat_format(disk, ESP_START, ESP_MIB * 2048, fat32=True,
                     label="ESP"))
    check("the system writes TFS3, two accounts and their files",
          make_tfs3_system(disk, log, 3))
    want, original = record(disk, P3_START)
    big = want.get("/home/alice/letters/big.bin", (0, 0, b""))[2] or b""
    pattern = bytes((i * 7 + 13) & 0xFF for i in range(BIG_KIB * 1024))
    check("TfsImage reads them: %d entries, alice's 2.4 MB file among them, "
          "the pattern mkmap64 writes (%d bytes)" % (len(want), len(big)),
          big == pattern)
    cut = os.path.join(work, "cut.img")
    shutil.copyfile(disk, cut)

    q, said = boot(disk, log)
    q.close()
    check("the start says it moves TFS3 in partition 3",
          "this disk holds TFS3 (partition 3) - moving it to TFS4" in said)
    check("a copy goes onto the EFI partition first",
          "a copy of TFS3 kept on the EFI partition" in said)
    moved = next((l.strip() for l in said.splitlines()
                  if "moved over from TFS3" in l), "")
    check("and every file is moved over ('%s')" % moved,
          moved and "could not" not in said)
    img = Ext2Image(disk, P3_START, P3_SECTORS)
    compare("after the move", want, img)
    img.close()
    copy = fat_get(disk, ESP_START, "/EFI/ThreadOS/tfs3-backup.img")
    check("the copy on the EFI partition is TFS3's %d bytes exactly "
          "(%s bytes there)" % (len(original), len(copy or b"")),
          copy == original)
    check("the note that a move is under way is gone",
          "tfs3-moving" not in fat_dir(disk, ESP_START))
    ok, parts, why = gpt_read(disk)
    check("the partition table is whole (%s)" % (why or "both copies agree"),
          ok and len(parts) == 3)
    check("and partition 3 kept its type: THREADOS-TFS3 is ThreadOS's too",
          entry_type(disk, 3) == TFS3_TYPE)
    check("the Windows partition beside it still holds its data",
          neighbours_intact(disk, MIB, which=(1,)))

    q, said = boot(disk, log)
    try:
        check("the start after that finds TFS4 and moves nothing again",
              "TFS4: in partition 3" in said and
              "moving it to TFS4" not in said and
              "moved over from TFS3" not in said)
        check("alice signs in with the password she had under TFS3",
              login(q, "alice", "alicepass"))
        out = q.command("cat alice.txt", PROMPT)
        check("and finds her file", "alice-wrote-this-under-tfs3" in out)
    finally:
        q.close()
    return cut, want, original


def cut_short(work, cut, want, original):
    """The state a stop half way through leaves: the copy and the note on
    the EFI partition, the partition's start overwritten by the format"""
    log = os.path.join(work, "b.log")
    note = "%d %d %d %d\n" % (P3_START, P3_SECTORS, 3, len(original))
    ok = fat_mkdirs(cut, ESP_START, ["/EFI", "/EFI/ThreadOS"]) and \
        fat_put(cut, ESP_START, original, "/EFI/ThreadOS/tfs3-backup.img") \
        and fat_put(cut, ESP_START, note.encode(),
                    "/EFI/ThreadOS/tfs3-moving")
    with open(cut, "r+b") as fh:
        fh.seek(P3_START * 512)
        fh.write(b"\xA5" * (4 << 20))
    check("a move cut short: the copy and the note on the EFI partition, "
          "the partition's first 4 MiB overwritten", ok)
    q, said = boot(cut, log)
    q.close()
    check("the next start finishes it from the copy",
          "a move from TFS3 was cut short - finishing it from the copy"
          in said and "moved over from TFS3" in said)
    img = Ext2Image(cut, P3_START, P3_SECTORS)
    compare("after a move cut short", want, img)
    img.close()
    check("and the note is gone",
          "tfs3-moving" not in fat_dir(cut, ESP_START))


def linux_partition(work):
    """TFS3 in a partition of another system's type, no FAT on the EFI
    partition: nowhere to keep a copy"""
    disk = os.path.join(work, "linux.img")
    log = os.path.join(work, "c.log")
    make_gpt_disk(disk, MIB, ESP_MIB)
    q, _ = boot(disk, log)
    try:
        q.command("sudo format3 3", "password:", timeout=20)
        q.command("thread", "Type YES", timeout=20)
        out = q.command("YES", PROMPT, timeout=120)
        q.command("write plain.txt moved-from-memory", PROMPT)
    finally:
        q.close()
    check("TFS3 in a partition of Linux's type",
          "this partition is a ThreadOS TFS3 now" in out)
    want, _ = record(disk, P3_START)
    q, said = boot(disk, log)
    q.close()
    check("with no file system on the EFI partition it is moved from "
          "memory alone, and the start says so",
          "no EFI partition to keep a copy on - moving it from memory alone"
          in said and "moved over from TFS3" in said)
    img = Ext2Image(disk, P3_START, P3_SECTORS)
    check("the file is there, as debugfs reads it",
          img.read("/plain.txt") == want.get("/plain.txt", (0, 0, None))[2]
          and img.read("/plain.txt") is not None)
    clean, problems = img.fsck()
    check("e2fsck -fn finds nothing to fix (%s)" % problems[:3], clean)
    img.close()
    ok, parts, why = gpt_read(disk)
    check("the partition became ThreadOS's, in both copies of the table "
          "(%s)" % (why or [p[0] for p in parts]),
          ok and len(parts) == 3 and parts[2][0] == GUID_TFS4 and
          entry_type(disk, 3) == TFS4_TYPE and
          entry_type(disk, 3, backup=True) == TFS4_TYPE)


def main():
    print("--- a disk with TFS3 moved to TFS4 ---")
    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return summary()
    missing = e2fsprogs_missing()
    if missing or not mtools_ok():
        check("%s is installed - the test needs it"
              % (missing or "mtools"), False)
        return summary()
    work = tempfile.mkdtemp()
    try:
        cut, want, original = installed(work)
        cut_short(work, cut, want, original)
        linux_partition(work)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    return summary()


if __name__ == "__main__":
    sys.exit(main())
