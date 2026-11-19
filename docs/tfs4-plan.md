# TFS4: the plan

TFS4 replaces TFS3 (`kernel/fs.c`) as the PC's file system. This file
says why, what it is built from, in which order, how each step is
tested and roughly how long it takes. It covers the file system only;
the other gaps against Linux 1.0 come from the audit and get their own
plan.

## Where TFS3 stands

TFS3 was right for the disk images the system started on, and it is
what stops the system now:

- 64 inodes for the whole disk, files and directories together;
- a block bitmap of 4 KiB, so 32768 blocks of 1 KiB: 32 MiB of data
  however large the partition is;
- 64 MiB for one file (12 direct blocks, one indirect, one doubly
  indirect), and 32-bit offsets throughout `fs.h`;
- an owner and nothing else: no mode bits (`chmod` does nothing, the
  `umask` is always 022), no group;
- no times at all (`utimensat` does nothing, `ls -l` has no dates);
- no hard links and no symbolic links (`readlink` says EINVAL);
- names up to 63 bytes; a directory is not a file of entries but
  every inode that names it as its parent, so a listing reads the
  whole table;
- every change written through at once with interrupts off, and
  nothing that makes a change survive a power cut half done, apart
  from orphans being freed at the next mount.

## The decision

TFS4's base is the **ext2 on-disk format** (revision 1), grown step by
step to **ext4's** features, and ThreadOS's own additions go on top,
each behind a feature flag of its own.

Why:
- ext2 is the file system of Linux 1.0, and everything TFS3 lacks above
  is in it;
- an independent implementation exists and is installed wherever the
  tests run: `mke2fs`, `e2fsck` and `debugfs` from e2fsprogs. AGENTS.md
  asks a test to compare against one where it exists; for a file system
  that means every disk ThreadOS writes can be checked by `e2fsck -fn`
  and read back by `debugfs`, and every disk `mke2fs` makes can be read
  by ThreadOS;
- Linux reads our disks and we read Linux's;
- the superblock's three sets of feature flags (compatible, read-only
  compatible, incompatible) were made for growing a format one feature
  at a time without breaking the disks already written.

What we do not take: no code from Linux or e2fsprogs (GPL); the
driver is ours, written from the format's documentation (the kernel's
`Documentation/filesystems/ext4/` and e2fsprogs' headers describe the
bytes, which is all we need).

## Stage 1: ext2 (TFS4.0) - about 4 to 6 days

1. **The format, behind the same `fs.h`.** `kernel/fs.c` keeps its
   interface; behind it: the superblock, block group descriptors, block
   and inode bitmaps, the inode table, direct, indirect, doubly and
   triply indirect blocks, directories as files of entries
   (`rec_len`), 4 KiB blocks (1 KiB on small disks, as `mke2fs` does).
   The `inodes[64]` array becomes an in-memory inode cache: a slot is
   a cache entry, held while a descriptor, a working directory or a
   mapping uses it, and loaded from disk on demand.
2. **Formatting** in the kernel (what `fs_format_now` does today), with
   the layout `mke2fs` would choose for the same size; the GPT
   partition type and the installer follow.
3. **Unix semantics.** Mode bits for user, group and others, checked in
   open, create, unlink, rename and mkdir instead of owner-only; uid
   and gid; a `umask` per process; atime, mtime and ctime; hard links
   (the link count); symbolic links (in the inode up to 60 bytes, in a
   block beyond); full `stat`. New system calls and their musl side:
   chmod, chown, link, symlink, readlink, utimensat, umask.
4. **64-bit sizes and offsets** in `fs.h`, the system calls and `lseek`
   (ext2's `large_file`): files past 4 GiB.
5. **Files unlinked while open** go on the superblock's orphan list,
   which `e2fsck` also understands, instead of TFS3's orphan parent.
6. **Moving off TFS3.** Installed systems (the laptop) have TFS3
   partitions. TFS3 holds at most 32 MiB of data, so the conversion
   reads all of it into memory, keeps a copy of the old metadata
   where TFS3 never wrote, formats and writes everything back; a
   conversion cut short is finished at the next boot from that copy.
   The TFS3 code stays only for this.
7. **Tests** (`t_tfs4`, new):
   - files, directories, modes, owners, times and links made in
     ThreadOS; then `e2fsck -fn` finds nothing and `debugfs` reads
     back the same bytes, modes, owners, times and link targets;
   - the reverse: an image made by `mke2fs` and filled by `debugfs`
     on the host, read in ThreadOS;
   - 10 000 files, deep directories, a file past 4 GiB (sparse),
     a full disk;
   - a TFS3 disk converted, every file the same afterwards, and a
     conversion stopped half way finished at the next boot;
   - the host's `TfsImage` in `scripts/ptest.py`, which every disk
     test uses, reads through `debugfs` instead;
   - a fuzz harness that mounts hostile images
     (`tests/fuzz/ext2_harness.c`, seeds from `mke2fs`).

About 3500 lines of C and 1000 of Python.

## Stage 2: ext4's features (TFS4.1) - about 7 to 10 days

1. **The journal** (JBD2, ordered mode: data written before the
   metadata that points at it commits), replayed at mount, with a
   write-back block cache beneath it. Test: QEMU killed at random
   moments during writes, many times; afterwards `e2fsck -fn` is clean
   and every file is either as before or as after. 2 to 3 days.
2. **Extents**, read and written, trees deeper than the inode. 1 to 2
   days.
3. **Metadata checksums** (crc32c on the superblock, group descriptors,
   bitmaps, inodes, directory blocks, extent blocks and the journal).
   1 to 2 days.
4. **Indexed directories** (htree), read and written. 1 to 2 days.
5. **The rest:** 64-bit block numbers, flex_bg, huge_file, nanosecond
   times and the creation time, dir_nlink. About 1 day.

Each one tested with `e2fsck` and `debugfs` as in stage 1, and with
images made by `mkfs.ext4` read in ThreadOS.

## Stage 3: ThreadOS's own additions (TFS4.2 and on) - about 2 to 3 weeks

Ideas taken from APFS (Apple publishes its format in the "Apple File
System Reference"; we take ideas, not code - the Linux APFS drivers
are GPL). Each addition has an incompatible feature flag of its own,
so `e2fsck` refuses a disk that uses one: these get a host checker of
their own (`scripts/tfs4check.py`, written apart from the driver) and
a fuzz harness, while stages 1 and 2 stay checked by `e2fsck` on disks
without them.

| Addition | From | How | Days |
|---|---|---|---|
| Sealed system volume: the system read-only, every block checked against a hash tree signed with the update key | APFS's signed system volume | like ext4's fs-verity, over the whole system | 2-3 |
| System and Data volumes sharing one partition's free space | APFS containers | a container superblock and volumes over one allocator | 3-5 |
| File clones: an instant copy that takes no space until written | APFS clones | XFS's way: a reference count on shared extents, copy on write of shared blocks only | 3-4 |
| The system before an update kept and returned to | APFS snapshots | two system volumes, A and B (Android, ChromeOS), not snapshots inside ext | 2-3 |
| Encryption per file for home directories (optional) | APFS, ext4's fscrypt | keys from the user's password | 3-4 |

What does not carry over: snapshots of everything and crash safety
without a journal come from copy-on-write everywhere, the opposite of
ext's writing in place. If they are ever needed, that is a new format
(TFS5), written knowing what stages 1 to 3 taught.

## The order and the whole

Stage 1 alone ends the 64-file and 32 MiB limits and brings Unix
semantics: about a week. Stages 1 and 2 give a file system at today's
Linux level: about two to three weeks. With stage 3, about five to
six weeks. These are estimates for work at the pace of the Wi-Fi and
Bluetooth stages, within half again either way; every step also waits
for its test groups and, before it is committed, the groups that read
or write the disk.

The phone's kernel (`kernelarm64/`) does not build `fs.c` today; once
it has a disk (virtio-blk), it takes TFS4 as shared code.
