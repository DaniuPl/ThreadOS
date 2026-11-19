# TFS4: replacing TFS3 - the plan

This is a work plan for whoever implements it (a person or an AI
assistant). It was written from a read of the code on 2026-11-19, at
commit `10d752b`; line numbers may have moved since. Read
[AGENTS.md](../AGENTS.md) first: every rule there applies, above all
"a test that would fail without the feature" and "never weaken a test".

## The decision

**TFS4 is the ext2 on-disk format (revision 1), in our own
implementation, under our own name.**

- The name in the code, the messages, the installer, the tests and the
  documentation is **TFS4** (`kernel/tfs4.c`, `[ OK ] TFS4: ...`,
  "ThreadOS partition (TFS4)", `t_tfs4read`, ...).
- The bytes on the disk are ext2's, unchanged: the superblock magic
  stays `0xEF53`. Changing it would lose the whole point - Linux's
  e2fsprogs as an independent checker, and disks that Linux can mount.
- The documentation says so plainly: "TFS4 - the ext2 format,
  implemented from its specification". README's Features line today
  calls TFS3 "our own filesystem"; TFS4 must not be sold as an
  invention of ours.
- **Written from the format's documentation, not from Linux's code**
  (Linux is GPL; the kernel is MIT). Good sources: the kernel.org
  "ext4 Data Structures and Algorithms" pages (they cover ext2's
  structures), Dave Poirier's "The Second Extended File System".
  No third-party library: the best-known one, lwext4, is reportedly
  largely GPLv2 - check before ever considering it (AGENTS.md rule 6).
- `s_creator_os` stays `0` (Linux): e2fsck reads the OS-dependent inode
  fields by it, and our layout of them is Linux's.

### Why ext2 and not a format of our own

- **An independent implementation to test against** (AGENTS.md rule 1):
  `mke2fs -d` builds images with a ready tree, `e2fsck -fn` checks every
  piece of metadata ThreadOS wrote, `debugfs` reads files and attributes
  back. A format of our own would need its own checker, which only
  checks what its author thought of (like `TfsImage` in
  `scripts/ptest.py` today).
- It has what TFS3 lacks: inodes by the million, directories as files,
  rwx/uid/gid, times, hard and symbolic links, files over 4 GiB
  (`large_file`), partitions of terabytes.
- A path forward: ext3 is ext2 plus a journal (JBD2), the same format,
  still checked by e2fsck.

## What TFS3 is today, and everything that leans on it

Limits (all in `kernel/fs.c` and `kernel/include/fs.h`):

- `FS_MAX_FILES 64` (`fs.h:11`): 64 inodes on the whole disk, files and
  directories together, the table held in memory.
- Directories are not files: each inode names its parent slot, so a
  lookup or a listing scans all inodes (`dir_find`, `fs.c:600`).
- The bitmap is 4 blocks (`BITMAP_BLOCKS`, `fs.c:44`), so the
  filesystem is cut to 32768 blocks = 32 MiB (`fit`, `fs.c:367`), even
  on a 500 GB whole-disk install. A file is at most ~64 MiB.
- Owner in 7 bits of the `type` byte (UID <= 127, `fs.c:66`); no rwx,
  groups, times or links. Access is "owner or admin" (`can_access`,
  `fs.c:559`) plus a list of system files in the root.
- No cache: every table change goes to the disk at once; operations run
  under `irq_save` (`fs_open`, `fs.c:704`) and the big kernel lock.
- Crash handling: orphans (unlinked while open) freed at mount
  (`load_tables`, `fs.c:375`). Nothing else.

What depends on TFS3's internals outside `fs.c`:

| Where | What |
|---|---|
| `kernel64/syscall64.c:216, 569, 628, 637, 652, 701, 1157, 1295, 1634` | **every path from a program is limited to `FS_NAME_MAX` = 63 bytes in total** (`name_from_ring3`, `SYS64_OPEN`, ...), not per component |
| `user64/usys64.h:130, 455` | ABI: `NAME_MAX64 63`, `struct dirent64 { char name[64]; uint32_t size; int is_dir; }` |
| `kernel64/syscall64.c:668` | `SYS64_READDIR` lists only the *working directory*, one entry per call by index; musl's `opendir` therefore changes directory (`libc/overlay/src/threados/socket.c:290`) |
| `kernel64/task64.h:244` | `struct fd_entry { int slot; uint32_t pos; }`: a 32-bit file position |
| `kernel/include/fs.h` | `fs_size` returns `int`, offsets are `uint32_t`: 2/4 GiB caps in the API |
| `kernel/swap.c:26-27, 119, 132` | swap reserves `swap.sys` through the FS, then writes raw sectors by block number; assumes `FS_BLOCK` = 1 KiB (4 blocks per page) |
| `kernel64/proc64.c:579-664`, `kernel64/isr64.c:181-187`, `kernel64/task64.c:1704` | file mappings keep block numbers and read/write raw sectors, again 1 KiB blocks |
| `kernel/desktop.c:5249`, `kernel64/shell64.c:241-265, 871-873` | buffers sized by `FS_NAME_MAX`, `df` in `FS_BLOCK` units |
| `fs_*` callers | `config.c`, `desktop.c`, `install.c`, `prog.c`, `swap.c`, `user.c`, `isr64.c`, `kmain64.c`, `proc64.c`, `shell64.c`, `syscall64.c`, `task64.c` |
| `kernel/fs.c:423` | the system's partition is found **by the superblock magic alone** |
| `kernel/gpt.c:98` | `GUID_TFS` = the bytes `THREADOS-TFS3\0\0\1`, written by the installer for the data partition |
| `kernel/user.c` | the accounts database, `/home/<user>/{Desktop,.Trash}`; `update.key` and `desktop.icons` are system files in the root |
| `scripts/ptest.py:3680` | `TfsImage`, a read-only host reader of TFS3, used by the disk tests |

Tests that mention TFS and must keep their assertions when moved to
TFS4 (check each): `t_tfs`, `t_fsbounds`, `t_disk`, `t_disks`,
`t_k64disk`, `t_k64core`, `t_mmap`, `t_mmapwrite`, `t_sqlite`,
`t_wholedisk`, `t_ginstal`, `t_foreigndisk`, `t_unlock`, `t_usbdisk`,
`t_nvme`, `t_http`, `t_tls64`, `t_boot`.

## Design decisions

1. **Which partition is ours.** Because the magic is ext2's, "an ext2
   superblock" no longer means "ThreadOS": a dual-boot laptop may have a
   Linux ext2/ext3 partition, and taking it as the system partition
   would put the accounts, `swap.sys` (a quarter of the free space) and
   everything else onto somebody else's data. Rule:
   - in a partition table: the GPT type GUID must be ThreadOS's
     (`THREADOS-TFS3...` on existing installs, `THREADOS-TFS4...` on new
     ones - recognise both) **and** the superblock must be ext2's;
   - a whole disk without a table (QEMU test images): the ext2
     superblock **and** the volume label `ThreadOS` (`s_volume_name`;
     tests run `mke2fs -L ThreadOS`).
   - Any other ext2 is not ours. (Mounting other people's disks is a
     separate feature, out of scope.)
2. **The handle is the inode number.** Today callers hold an `int slot`
   that is stable on the disk. An ext2 inode number is just as stable,
   so it can take the slot's place with no reference-counted cache:
   descriptors, `cwd` and mappings keep an `int`, and the semantics of
   "unlinked while open" stay as they are (`fs_slot_closed` asks
   `task64_slot_open`). `FS_ROOT` stays `-1` in the API and maps to
   inode 2 inside TFS4. A small inode cache inside `tfs4.c` is an
   optimisation, not part of the contract.
3. **Two filesystems behind one API.** `kernel/fs.c` becomes the front
   (the public `fs_*` functions, the choice of backend at mount);
   today's code moves to `kernel/tfs3.c`; the new one is
   `kernel/tfs4.c`. TFS3 stays, read-only, for migration.
4. **Block size.** Read 1, 2 and 4 KiB blocks; create 4 KiB ones (one
   block per page simplifies swap and mappings). Callers ask
   `fs_block_size()` instead of using `FS_BLOCK`.
5. **Features.** Mount read-write when `incompat` has only `filetype`
   and `ro_compat` only `sparse_super` / `large_file`; refuse to mount
   for any other `incompat` bit (ext4's `extents`, `64bit`, `flex_bg`,
   `journal_dev`, ...); mount read-only for other `ro_compat` bits.
   `compat` bits a modern `mke2fs -t ext2` sets:
   - `ext_attr`: ignore attributes, but when a file with `i_file_acl`
     is deleted, drop the reference on its attribute block (freeing it
     at zero) - e2fsck counts them;
   - `resize_inode`: never touch inode 7 or the reserved GDT blocks;
   - `dir_index`: read htree directories linearly (the index hides in
     empty entries) and clear `EXT2_INDEX_FL` on any change to such a
     directory, as old Linux kernels did.
   - Inodes: use `s_inode_size` (mke2fs makes 256-byte ones), never
     assume 128. Confirm with e2fsck what `i_extra_isize` must hold.
6. **Limits through the system-call layer.** Names up to 255 bytes,
   paths up to `FS_PATH_MAX` (1024 is a reasonable start), path
   buffers on the heap rather than the kernel stack. File sizes and
   positions 64-bit end to end (`fd_entry.pos`, `fs_size`, offsets).
7. **ABI.** `struct dirent64` is shared with programs already built
   (Weft's `.tapp` is built apart from the system). Add a new call
   (`SYS64_READDIR2`: a directory descriptor, a cursor, names up to 255,
   a 64-bit size, the inode number and the type) and keep
   `SYS64_READDIR` working for old programs; it hides entries whose
   names do not fit 63 bytes. musl's `opendir` moves to the new call and
   stops changing directory.
8. **Listing cost.** A listing by index rescans from the start each
   call (O(n^2) over 10 000 files). Keep a cursor: a byte offset into
   the directory, cached for the last (directory, index) pair, or the
   cookie of `SYS64_READDIR2`.
9. **Portability.** `tfs4.c` stays freestanding and uses only
   `string.c`, `heap.c` and the block API, so the ARM64 kernel can take
   it later (virtio-blk) - see AGENTS.md rule 5.
10. **Without a journal (stages 2-6)**, order the writes so a crash
    leaks space rather than corrupting: data, then the inode, then the
    directory entry when creating; the reverse when deleting. At mount,
    when `s_state` says "not clean": free the orphan list, recount free
    blocks and inodes from the bitmaps, mark it clean on unmount.

## Stages

Each stage is one commit with its test(s), a `docs/JOURNAL.md` entry
and, where the user can see it, README. Build with no new warnings;
run the groups the stage touches. Host tools: `e2fsprogs` (`mke2fs`,
`e2fsck`, `debugfs`; `mke2fs -d` needs 1.43 or newer) - add it to the
`apt-get install` lines in `.github/workflows/*.yml` if a runner lacks
it, and to README's prerequisites.

### Stage 1 - a kernel ready for a second filesystem (still only TFS3)

- Split `fs.c` into the front and `tfs3.c` (decision 3).
- Callers stop using TFS3's constants: `fs_block_size()` in swap and
  mappings (`swap.c`, `proc64.c`, `isr64.c`, `task64.c`), `FS_NAME_MAX`
  buffers replaced (`desktop.c`, `shell64.c`, `syscall64.c`).
- Paths from programs no longer capped at 63 bytes (decision 6).
- 64-bit sizes and positions in the API and `fd_entry`.
- `SYS64_READDIR2` and musl's `opendir` on it (decision 7).
- **Tests:** every existing disk group passes unchanged. New: a path
  longer than 63 bytes (directories nested deep enough) works through
  open, mkdir, chdir, rename and unlink from a ring 3 program and from
  a musl program - this fails today; a listing through `opendir` leaves
  the working directory where it was.

### Stage 2 - TFS4 read-only

- `kernel/tfs4.c`: superblock, group descriptors, inodes, direct,
  indirect, doubly and triply indirect blocks, directories, 1/2/4 KiB
  blocks, feature checks (decision 5), the rule for "ours"
  (decision 1).
- Every block and inode number read from the disk is checked against
  the filesystem's bounds before use, as `t_fsbounds` demands of TFS3.
- **Test `t_tfs4read`:** the host makes images with
  `mke2fs -t ext2 -L ThreadOS -d <tree>`, once with `-b 4096` and once
  with `-b 1024`. The tree holds 10 000 files in one directory, deep
  nesting, 255-byte UTF-8 names, a sparse file, and a file over 65 MiB
  (on the 1 KiB image that needs the triply indirect block). ThreadOS
  lists everything and prints a SHA-256 of every file; the host
  compares with its own.
- **Test:** an ext2 partition without ThreadOS's GUID (a "Linux"
  partition on a GPT disk) is not used as the system's partition, and
  its bytes are unchanged afterwards.
- **Fuzz:** `tests/fuzz/tfs4_harness.c` under ASan/UBSan - an image in
  memory behind a stub of `blk_read`, mount, walk the whole tree, read
  every file; seed corpus from `mke2fs`; registered in
  `scripts/fuzz_parsers.py`.

### Stage 3 - TFS4 read-write

- Block and inode allocation (preferring the parent's group), create,
  unlink, rename (also across directories, also replacing a target),
  truncate and holes, growing directories, free counts in the groups
  and the superblock, `s_state`, the orphan list (`s_last_orphan`,
  chained through `i_dtime`; check that e2fsck accepts what we leave).
- `mkfs` in the kernel for the installer and the `format` command:
  revision 1, 4 KiB blocks, `filetype`, `sparse_super`, `large_file`,
  label `ThreadOS`.
- **Test `t_tfs4write`:** a scenario in the system (create 5 000 files,
  delete every other one, rename across directories, grow and cut
  files, a SQLite database), a clean shutdown, then on the host
  `e2fsck -fn` exits 0 and `debugfs -R 'cat ...'` gives the same bytes.
  A second case: an image made by the kernel's `mkfs` passes
  `e2fsck -fn`.

### Stage 4 - real POSIX metadata

- rwx bits, 32-bit uid and gid (the UID <= 127 cap goes), access,
  modification and change times (from `rtc.c`), hard links, symbolic
  links (fast ones inside the inode too).
- System calls and the musl overlay: `stat`/`fstat` with the real mode,
  owner, inode number and times instead of the faked `0644`
  (`syscall.c:583`), and `chmod` (today it returns 0 and does nothing,
  `syscall.c:1753`), `chown`, `utimensat`, `link`, `symlink`,
  `readlink`, `access`.
- `can_access` becomes owner/group/other plus the admin role; the
  system files in the root become ordinary `root:root 0644` files.
- **Test `t_tfs4perm`:** two accounts; a `0600` file of one gives the
  other `EACCES`, `chmod` opens it; `stat` from ThreadOS matches
  `debugfs -R stat`; a symlink made in ThreadOS reads back in `debugfs`
  and one made by `mke2fs -d` resolves in ThreadOS.

### Stage 5 - the installer and migration

- The installer formats the data partition as TFS4 with the type GUID
  `THREADOS-TFS4...` (`kernel/gpt.c`), label "ThreadOS partition
  (TFS4)" in `install.c:324`.
- Migration of existing installs (whole-disk mode; "beside another
  system" has no data partition), at the first start of the new kernel
  that finds TFS3 in ThreadOS's partition:
  1. copy the raw 32 MiB TFS3 area to `\EFI\ThreadOS\tfs3-backup.img`
     on the EFI partition (256 MiB or 1 GiB, `install.c:81, 89`);
  2. read every file and directory into memory, with owners;
  3. `mkfs` TFS4 on the same partition;
  4. write them back (accounts, `update.key`, `desktop.icons`, homes);
     `swap.sys` is not copied but made anew;
  5. leave the partition table alone (fewer writes, less risk): the
     old `THREADOS-TFS3` GUID stays valid.
- **Test `t_tfs3migrate`:** the host writes a TFS3 image (add writing
  to `TfsImage`; the format is frozen), boots, and checks: the same
  files and owners, signing in with the old password works, the backup
  equals the original bytes, `e2fsck -fn` is clean.
- Move the existing disk tests from `TfsImage` to `debugfs`, keeping
  every assertion (AGENTS.md rule 2). `t_fsbounds` gets TFS4 cases: a
  block pointer past the end, a directory loop, `rec_len` 0, an inode
  number out of range.

### Stage 6 - a block cache and concurrency

- A block cache (LRU, write-back, `sync` every few seconds and at
  shutdown); a filesystem lock instead of disabling interrupts around
  disk I/O; the filesystem out from under the big kernel lock (README
  roadmap item).
- With 4 KiB blocks the same cache can serve as a page cache, which
  lets file mappings be shared (`MAP_SHARED` is refused today).
- Swap still writes raw sectors: its blocks must never sit in the cache
  (`swap.sys` unreadable through the filesystem).
- **Tests:** two processes on two cores write different files at once,
  then `e2fsck -fn` clean and the contents right; after `sync` and a
  hard power-off through QMP the data is there.

### Stage 7 - the journal (ext3, JBD2, ordered mode)

- Metadata through the journal, data written before the commit, the
  journal replayed at mount; `has_journal` accepted.
- **Test `t_tfs4crash`:** the system writes in a loop and the host kills
  QEMU at a random moment, a dozen times with different seeds; after
  each restart (replay by ThreadOS) and a clean shutdown `e2fsck -fn`
  is clean.

## Out of scope (later, separately)

- 64-bit LBAs in `kernel/blk.c` (`blk_read(uint32_t lba, ...)`,
  `blk.h:60`): disks over 2 TiB.
- Reading ext4 with extents, to read disks Linux formats today.
- Mount points: the EFI partition and USB sticks visible to programs.

## Risks

- Stage 1 touches a dozen kernel files and the program ABI.
- Stage 5 works on people's real data; the backup comes first, always.
- Stages 2-4 are mostly new code that e2fsprogs checks well.
