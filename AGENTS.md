# Instructions for AI coding agents

This file is for AI assistants (Claude Code, Codex, Copilot, Cursor and
others) working in this repository. Human contributors should read
[CONTRIBUTING.md](CONTRIBUTING.md). The rules are the same in both files.
This one is shorter and lists the commands.

## What this is

ThreadOS is a hobby operating system written in C and assembly. The PC
system is one 64-bit long-mode kernel: `kernel/` holds the code that does
not depend on the CPU mode (drivers, filesystems, network, desktop),
`kernel64/` and `boot64/` hold paging, interrupts, context switching,
the syscall entry and the entry into long mode. Ring 3 programs live in
`user/` and `user64/`; `user/uapi.h` gives them the system call layer
(`user64/usys64.h` plus `user64/compat64.h`). Programs built against
musl (`libc/`) have POSIX threads and sockets.

There used to be a 32-bit kernel as well. It was removed; names that
came with it (`kernel/include/task.h`, for one) are still used by the
shared code and answered by `kernel64/compat64.c`.

A second kernel for ARM64 (`bootarm64/` + `kernelarm64/`, QEMU's `virt`
machine, built with clang) is at its sixth stage: it boots, reads the
device tree, catches exceptions, runs with the MMU on, takes timer
interrupts through a GICv2, preempts kernel threads and runs them on
every core it wakes through PSCI, runs programs from its initrd at EL0
in address spaces of their own (`kernelarm64/proc.c`, the PC's system
call numbers) - freestanding ones in `userarm64/`, and musl programs:
the same `libc/` and overlay built for aarch64 by the cross gcc
(`gcc-aarch64-linux-gnu`), with threads, futexes, memory made on first
touch, fork (copy-on-write), exec and wait, and pipes, Unix socket
pairs and shared memory from `kernel/ipc.c`, which both kernels build
(each gives it what it needs through `kernel/include/ipchost.h`), and
the PC's network stack (`kernel/net.c`, `tcp.c`, `dhcp.c`) on a
virtio-net card (`kernelarm64/vnet.c`, `netglue.c`) - and, given a
phone's devices, is ThreadOS mobile: a ramfb screen set up through
fw_cfg, the initrd through fw_cfg, a PL031 clock, a virtio-input
touchscreen over virtio-mmio, the shell in `kernelarm64/mobile.c`, and
programs' windows through the PC's window calls (`kernelarm64/winmob.c`:
a window is the screen below the status bar, the finger the program's
mouse; `user64/usys64.h` builds for AArch64 too). Of `kernel/` it
builds `string.c`, `heap.c`, `gfx.c`, `ttf.c`, `uitext.c`, `mark.c`,
`aurora.c`, `fonts.c`, `initrd.c`, `locale.c`, `ipc.c`, `net.c`,
`tcp.c` and `dhcp.c` (`SHAREDARM` in the `Makefile`), on `kernel/include/io.h` and `spinlock.h`, which have an
AArch64 branch next to the x86 one, and `kernelarm64/video.c` in place
of the PC's video layer; the rest follows as shared code stops assuming
x86. Its tests are `arm64boot`, `arm64core`, `arm64threads`,
`arm64mobile`, `arm64user`, `arm64libc`, `arm64net` and `arm64win`. The overlay's C
layer (`libc/overlay/src/threados/`) is one file for both machines: a change
there is built and tested on both.

## Commands

```sh
make -j8                      # kernel + initrd (build/threados64.bin, build/initrd64.img)
make efi                      # threadboot, the UEFI loader (build/BOOTX64.EFI; clang + lld)
make kernelarm64              # ARM64 kernel for QEMU virt (build/threados-arm64.elf; clang + lld; musl by aarch64-linux-gnu-gcc)
make webkit                   # Weft on WebKit's WPE port: build/weft.tapp (hours; not in the default build)
make run-arm64                # boot it (needs qemu-system-aarch64; serial console)
make run-phone                # ThreadOS mobile: the ARM64 kernel with a screen and touch
make iso64 stick              # hybrid disc and USB image
make run                      # boot the desktop in QEMU
make test                     # full suite: 100+ QEMU groups, ~20-30 min with -j4
make test T=files,filetree    # selected groups
make fuzz                     # parsers under ASan/UBSan on the host
```

Build into another directory with `make BUILD=/some/dir ...`. Run tests
against that build by passing the two images as arguments:

```sh
python3 scripts/run_tests.py -tfiles $B/threados64.bin $B/initrd64.img
```

The screen reader in `scripts/ptest.py` needs `freetype-py` and
`fontTools` (`pip install freetype-py fonttools`), as CI installs them.
Without them every desktop test reports that the desktop never appeared.

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
5. **One PC kernel, the 64-bit one; shared code builds where it is
   shared.** Do not bring back 32-bit code paths, `#ifdef __x86_64__`
   branches for a 32-bit build, or `-m32` builds. `kernel/string.c`,
   `kernel/heap.c`, `kernel/include/io.h` and `spinlock.h` are built for
   ARM64 too: when you change them, run `make kernelarm64` and the ARM64
   groups as well as the x86-64 build and tests.
6. **Libraries are welcome; bring them in properly.** Do not rewrite what a
   good library already does. A new library must be:
   - **in the tree, built from source** by the `Makefile`: a pinned release
     in `third_party/<name>/` (or its upstream archive), never downloaded at
     build time and never a prebuilt binary (firmware a device runs itself,
     like `third_party/iwlwifi/`, is the one exception: it exists only as
     its vendor's binary);
   - **unmodified.** Changes go into `libc/overlay/`, a patch directory
     next to the code that uses it (like `apps/browser/patches/`), or our
     own wrapper code;
   - **recorded** in `third_party/README.md`: version, licence, source,
     and what was trimmed;
   - **licensed** MIT, BSD, Apache-2.0, zlib, ISC, LGPL or similar. GPL
     code would change the licence of what links it; ask the owner first;
   - **tested** like everything else: a QEMU test that exercises it on
     ThreadOS, and a fuzz harness if it parses untrusted input.

   Programs link libraries against `libc/` musl. The kernel can take only
   freestanding code (no libc), compiled with the kernel's flags.
7. **Never commit secrets.** The update signing key belongs only in the
   repository secret `THREADOS_UPDATE_KEY`. Tests generate keys of their
   own.

## Where things are

| Task | Files |
|---|---|
| Add a system call | `kernel64/syscall64.c`, `kernel64/syscall64.h`, `user64/usys64.h` (and `user64/compat64.h` if `user/` programs use it); for musl programs, `libc/overlay/src/threados/syscall.c` |
| Add a ring 3 program | `user/<name>.c` (or `libc/tests/`, `apps/` for musl programs); add it to `USER64_ELFS` and to the `mkinitrd.py` line of `$(INITRD64)` in the `Makefile` (initrd names are at most 15 characters) |
| Add a driver | `kernel/<name>.c` + `kernel/include/<name>.h`; add `<name>` to `SHARED64` in the `Makefile` |
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
