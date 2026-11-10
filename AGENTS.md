# Instructions for AI coding agents

This file is for AI assistants (Claude Code, Codex, Copilot, Cursor and
others) working in this repository. Human contributors should read
[CONTRIBUTING.md](CONTRIBUTING.md). The rules are the same in both files.
This one is shorter and lists the commands.

## What this is

ThreadOS is a hobby operating system written in C and assembly. The
PC system is a **64-bit long-mode kernel**: the `kernel/` files listed in
`SHARED64` in the `Makefile`, plus `kernel64/` and `boot64/`. (There was
a 32-bit kernel too; it was removed, and its tests moved to this one.)

A second kernel for ARM64 (`bootarm64/` + `kernelarm64/`, QEMU's `virt`
machine, built with clang) is at its third stage: it boots, reads the
device tree, catches exceptions, runs with the MMU on, takes timer
interrupts through a GICv2, preempts kernel threads and runs them on
every core it wakes through PSCI. Of `kernel/` it builds `string.c` and
`heap.c` so far, on `kernel/include/io.h` and `spinlock.h`, which have
an AArch64 branch next to the x86 one; the rest follows as shared code
stops assuming x86. Its tests are `arm64boot`, `arm64core` and
`arm64threads`.

Code that does not depend on the processor lives in `kernel/`; paging,
interrupts, context switching and the syscall entry are in `kernel64/`.
Ring 3 programs are in `user/` (shared sources, which reach the kernel
through `user/uapi.h` = `user64/usys64.h` plus `user64/compat64.h`) and
`user64/`.

## Commands

```sh
make -j8                      # kernel + initrd (build/threados64.bin, build/initrd64.img)
make efi                      # threadboot, the UEFI loader (build/BOOTX64.EFI; clang + lld)
make kernelarm64              # ARM64 kernel for QEMU virt (build/threados-arm64.elf; clang + lld)
make run-arm64                # boot it (needs qemu-system-aarch64; serial console)
make iso64 stick              # hybrid disc and USB image
make run                      # boot the 64-bit desktop in QEMU
make test                     # full suite: 100+ QEMU groups, ~20-30 min with -j4
make test T=files,filetree    # selected groups
make fuzz                     # parsers under ASan/UBSan on the host
```

Build into another directory with `make BUILD=/some/dir ...`. Run tests
against that build by passing the two images as arguments:

```sh
python3 scripts/run_tests.py -tfiles $B/threados64.bin $B/initrd64.img
```

**Never rebuild `build/` while a test run is using it.** The tests boot the
images in `build/`, and replacing them mid-run produces failures that look
real. Use a separate `BUILD=` directory for experiments.

## Rules a change is judged by

1. **Every feature needs a test that would fail without it.** Tests live in
   `scripts/tests/t_<group>.py`. Each one boots a real kernel in QEMU and
   checks the result from outside: the screen through QMP, the serial port,
   or disk images read with `mtools`. Register each new group in `GROUPS`
   in `scripts/run_tests.py`. Assertions must be non-vacuous. Do not write
   `check(..., x or True)`, and do not check something the code under test
   could not get wrong. Where an independent implementation exists (OpenSSL,
   mtools, FreeType, Node.js, glibc), compare against it.
2. **Never weaken, skip or delete a test to get green.** If a test fails,
   find out why. Do not wait a fixed time and read the screen once. Poll
   until the expected thing appears, with a timeout.
3. **English everywhere:** identifiers, comments, commit messages,
   documentation, and everything the system prints or shows. The only
   non-English text is test data for non-ASCII input: the Polish keyboard
   layout (`kernel/kbdmap.c`) and the UTF-8 strings the tests type or
   compare (`t_polish`, `t_libc`, `scripts/tests/js/`).
4. **Comments explain *why*,** not what. Match the comment density and the
   tone of the surrounding code.
5. **Shared code must build where it is shared.** `kernel/string.c`,
   `kernel/heap.c`, `kernel/include/io.h` and `spinlock.h` are built for
   ARM64 too: when you change them, run `make kernelarm64` and the ARM64
   groups as well as the x86-64 build and tests.
6. **No new dependencies.** The kernel and programs are freestanding (no
   libc, except programs built against `libc/` musl). Third-party code goes
   into `third_party/` unmodified. Changes go into `libc/overlay/`, `apps/browser/patches/` or into
   our own wrapper code.
7. **Never commit secrets.** The update signing key belongs only in the
   repository secret `THREADOS_UPDATE_KEY`. Tests generate keys of their
   own.

## Where things are

| Task | Files |
|---|---|
| Add a system call | `kernel64/syscall64.c`, `kernel64/syscall64.h`, `user64/usys64.h` (and `user64/compat64.h` if shared programs use it) |
| Add a ring 3 program | `user/<name>.c` or `user64/<name>.c`; add it to `USER64_ELFS` and to the `mkinitrd.py` lines of `$(INITRD64)` in the `Makefile` (initrd names are at most 15 characters) |
| Add a driver | `kernel/<name>.c` + `kernel/include/<name>.h`; add `<name>` to `SHARED64` |
| Desktop / compositor | `kernel/desktop.c`, `kernel/gfx.c`, `kernel/uitext.c` |
| GUI widget kit for programs | `user/ugui.h`, `user/udraw.h` |
| Filesystem (TFS3) | `kernel/fs.c`; FAT in `kernel/fat.c` |
| Installer, boot entries | `kernel/install.c`, `kernel64/efirt.c`, `user/ginstal.c` |
| UEFI loader | `boot/efi/efiboot.c` |
| Updates | `user/update.c`, `scripts/mkupdate.py`, `.github/workflows/release.yml` |
| Test helpers (QEMU, QMP, screen reading) | `scripts/ptest.py` |

Test helpers in `scripts/ptest.py` read layout constants straight from the
C sources (`_kernel_define("user/gfiles.c", "ROW_H")`). Keep such
`#define`s as plain integer literals, and update the helpers when a layout
changes.

## Before you finish

- The kernel builds without new warnings (and ARM64 too, when its shared files changed).
- The test groups your change touches pass, plus any group that clicks on
  or reads the screen region you changed.
- Notable changes get an entry at the end of [docs/JOURNAL.md](docs/JOURNAL.md).
  Say what changed, why, and how it is tested.
- Commit messages: one summary line, then a paragraph on *why*.
