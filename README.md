# ThreadOS

[![license: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![release](https://img.shields.io/github/v/release/DaniuPl/ThreadOS)](https://github.com/DaniuPl/ThreadOS/releases/latest)
[![Discord](https://img.shields.io/badge/chat-Discord-5865F2?logo=discord&logoColor=white)](https://discord.gg/AyaEyTSG6)

ThreadOS is an x86 operating system written from scratch in C and
assembly. It has its own kernel, drivers, filesystem, TCP/IP stack, TLS
1.3, UEFI boot loader, installer and compositing desktop. It runs in QEMU
and on real UEFI PCs.

![The desktop: a file manager, a clock and a memory monitor](docs/desktop.png)

- [Architecture](#architecture)
- [Installing on a PC](#installing-on-a-pc)
- [Building](#building)
- [Running in QEMU](#running-in-qemu)
- [Testing](#testing)
- [Repository layout](#repository-layout)
- [Maintenance](#maintenance)
- [Features](#features)
- [Roadmap](#roadmap)

## Architecture

**Two kernels, one source tree.**

| | 32-bit kernel | 64-bit kernel |
|---|---|---|
| Mode | protected mode, paging | long mode, 4-level paging with NX |
| Sources | `boot/`, `kernel/` | `boot64/`, `kernel64/` + the shared part of `kernel/` |
| Syscalls | `int 0x80` (`kernel/syscall.c`) | `syscall`/`sysret` (`kernel64/syscall64.c`) |
| Boots via | Multiboot 1 (QEMU `-kernel`) or Multiboot 2 (GRUB) | Multiboot 2 (GRUB) or threadboot (UEFI) |
| Output | `build/threados.bin` + `build/initrd.img` | `build/threados64.bin` + `build/initrd64.img` |

The two columns share more than they differ. Drivers, the filesystem, the
network stack, the window compositor and most of the kernel live in
`kernel/` and are compiled twice. The list is `SHARED64` in the
`Makefile`. Only the CPU-mode-specific parts have two implementations:
paging, interrupts, context switching, the syscall entry and process
setup. The 32-bit kernel is still built and tested, because a second
compilation catches width and pointer-size bugs in the shared code.

**Boot paths (64-bit).**

- BIOS or UEFI with GRUB: `build/threados64.iso` boots GRUB, and GRUB
  loads the kernel and initrd through Multiboot 2.
- UEFI with threadboot (`boot/efi/efiboot.c`), the project's own PE32+
  loader. It reads `threados64.bin` and `initrd64.img` from the EFI system
  partition, sets a GOP mode (the panel's native resolution, from EDID),
  builds the Multiboot 2 tags itself (memory map, framebuffer, ACPI RSDP,
  EFI system table), exits boot services and jumps to the kernel. The
  installer and the USB image use this path. The kernel keeps the UEFI
  runtime services, which it uses to write firmware boot entries.

**Userland.** Programs are freestanding ELF files packed into the initrd
(`scripts/mkinitrd.py`). They can also be run from disk. Sources in
`user/` build for both kernels: `user/uapi.h` includes either
`user/usys.h` (32-bit) or `user64/usys64.h` + `user64/compat64.h`
(64-bit). Crypto, TLS, HTTP and DNS are header-only code. The programs
that use them link against `libthread.so`, which the kernel maps into
every process once. On the 64-bit side, programs can also be built
against musl (`libc/`, with upstream musl in `third_party/musl`). `js`
(QuickJS) is built this way.
C++ programs use the host compiler's libstdc++ on top of musl, without
exceptions (`libc/cxx/`). The first is litehtml (`third_party/litehtml`),
an HTML/CSS layout engine, which lays pages out on ThreadOS exactly as
it does on Linux. The 64-bit web browser (`apps/browser/web.cpp`) draws
its pages.

**Graphics.** The compositor (`kernel/desktop.c`) runs in the kernel. It
draws with `kernel/gfx.c` (anti-aliased shapes, shadows, blur) and
`kernel/ttf.c` (a TrueType rasteriser that matches FreeType with hinting
off). Ring 3 programs draw into their windows through system calls, using
the same primitives (`user/udraw.h`) and a small widget kit
(`user/ugui.h`).

## Installing on a PC

Every push to `main` produces a
[release](https://github.com/DaniuPl/ThreadOS/releases/latest):

| Asset | Contents |
|---|---|
| `threados64.iso` | Hybrid BIOS/UEFI disc: live system and installer |
| `threados-stick.img.xz` | Raw GPT disk image for a USB stick (UEFI only) |
| `update.txt`, `threados64.bin`, `initrd64.img`, `BOOTX64.EFI` | A signed system update |

1. Write `threados64.iso` to a USB stick: Rufus in **DD image** mode,
   balenaEtcher, or `dd if=threados64.iso of=/dev/sdX bs=4M conv=fsync`.
2. Disable Secure Boot. threadboot is not signed.
3. Boot from the stick and choose **Install ThreadOS onto this computer's
   disk**. There are two modes:
   - *beside another system*: copies three files into `\EFI\ThreadOS\` on
     the existing EFI partition and adds a boot entry at the end of the
     boot order;
   - *whole disk*: writes a new GPT, a FAT32 EFI partition and a TFS3 data
     partition, and adds a boot entry first in the boot order.
4. Updating an installed system: `run update check`, then
   `sudo run update`. See [Releases and updates](#releases-and-updates).

## Building

### Prerequisites

Debian/Ubuntu:

```sh
sudo apt-get install build-essential gcc-multilib make python3 \
    qemu-system-x86 grub-pc-bin grub-efi-amd64-bin xorriso mtools \
    ovmf clang lld openssl
```

| Needed for | Packages |
|---|---|
| kernels and initrds | `gcc` with `-m32` support (`gcc-multilib`), `make`, `python3` |
| discs (`iso`, `iso64`) | `grub-pc-bin`, `grub-efi-amd64-bin`, `xorriso`, `mtools` |
| threadboot (`efi`, `stick`) | `clang`, `lld`, `mtools` |
| running and testing | `qemu-system-x86`, `ovmf`, `openssl` |
| optional tests and tools | Python `freetype-py` and `fonttools` (font tests), `Pillow` (screenshots), Node.js (JavaScript comparison tests) |

A test that is missing an optional tool prints `SKIPPED` and the reason.

### Targets

| Command | Produces |
|---|---|
| `make` | `build/threados.bin`, `build/initrd.img` (32-bit) |
| `make kernel64` | `build/threados64.bin`, `build/initrd64.img` |
| `make efi` | `build/BOOTX64.EFI` (threadboot) |
| `make iso64` | `build/threados64.iso` (builds the 64-bit kernel and threadboot first) |
| `make iso` | `build/threados.iso` (32-bit) |
| `make stick` | `build/threados-stick.img` |
| `make libc64` | musl for ThreadOS (built automatically when needed) |
| `make browser` | litehtml and its layout test, for ThreadOS and for the host |
| `make clean` | removes `build/` |

Variables:

| Variable | Meaning |
|---|---|
| `BUILD=<dir>` | build somewhere other than `build/` |
| `VERSION=<n>` | system version written into the initrd (default `0`) |
| `KVM=1` | use `/dev/kvm` in the `run` targets |
| `AUDIO=pa\|pipewire\|alsa` | attach an Intel HDA card to the `run` targets |
| `T=a,b` / `J=n` | test groups to run / how many at once (`make test`) |

`-j` works for all targets. Most of a clean build's time goes to musl
and QuickJS.

## Running in QEMU

```sh
make run              # 64-bit system from the hybrid disc, to the desktop
make run KVM=1        # with hardware virtualisation
make run32            # 32-bit kernel
make run-iso64-uefi   # the hybrid disc under OVMF
make run-stick        # the USB image under OVMF, exactly as a PC boots it
```

The serial console is on stdio. The kernel logs everything there, which
makes it the first place to look when something goes wrong. Inside the
system, `hardware` lists PCI devices and `disks` lists block devices.

## Testing

```sh
make test                   # every group, 4 at once
make test T=files,filetree  # selected groups
make test J=8               # more parallel VMs
make fuzz                   # parsers under ASan/UBSan on the host
```

The suite (`scripts/run_tests.py`) has more than 100 groups and over 1800
assertions. Each group is one file, `scripts/tests/t_<group>.py`. It
boots a real kernel in QEMU and checks the system from outside:

- the screen, through QMP screendumps. Text is read back glyph by glyph
  in the system's own fonts (`scripts/ptest.py`).
- the serial port, and keyboard and mouse input sent through QMP.
- disk images read back with `mtools` and a GPT parser.
- OVMF, which boots an installed disk or fails to.
- independent implementations used for comparison: OpenSSL (ciphers,
  TLS, signatures), FreeType (fonts), Node.js (JavaScript), glibc (libc
  output).

A full run takes 20–30 minutes on four cores with KVM.

To run tests against a build that is not in `build/`, pass the images
explicitly:

```sh
make BUILD=/tmp/b -j8 all kernel64 efi
python3 scripts/run_tests.py -tfiles /tmp/b/threados.bin /tmp/b/initrd.img \
    /tmp/b/threados64.bin /tmp/b/initrd64.img
```

Do not rebuild `build/` while a test run is using it.

The network groups run a DNS server on host port 53, because QEMU's user
network sends guest DNS queries there. Run them as root, or once run
`sudo sysctl -w net.ipv4.ip_unprivileged_port_start=53`.

CI: `.github/workflows/build.yml` builds everything and runs a
cross-section of groups. It is started by hand from the Actions tab.

## Repository layout

```
boot/            32-bit entry, GRUB menus (grub.cfg, grub64.cfg)
boot/efi/        threadboot, the UEFI loader
boot64/          long-mode entry, ISR and syscall stubs, AP startup
kernel/          shared kernel code + the 32-bit kernel; headers in kernel/include/
kernel64/        64-bit-only kernel code, linker script
user/            ring 3 programs and the header-only libraries (crypto, TLS, HTTP, GUI kit)
user64/          64-bit-only programs, the 64-bit syscall layer
libc/            musl port: replaced files (overlay/), linker script, tests
apps/js/         the QuickJS host (`js`)
apps/browser/    the litehtml browser: its test, and the patches to litehtml
third_party/     unmodified upstream code: musl 1.2.5, QuickJS, litehtml
fonts/           DejaVu subsets used by the kernel and programs
scripts/         build helpers, release signing, test runner and tests
tests/fuzz/      host-side fuzzing harnesses
docs/            screenshots and the development journal
```

## Maintenance

### Adding things

| What | Where |
|---|---|
| a ring 3 program | `user/<name>.c`; add it to `USER_ELFS` and/or `USER64_ELFS` and to the `mkinitrd.py` lines of `$(INITRD)` / `$(INITRD64)`. Initrd names are at most 15 characters. |
| a 32-bit system call | `kernel/syscall.c` and a wrapper in `user/usys.h` |
| a 64-bit system call | `kernel64/syscall64.h` (number), `kernel64/syscall64.c` (handler), `user64/usys64.h` (wrapper), plus `user64/compat64.h` if shared programs need it under the 32-bit name |
| a driver | `kernel/<name>.c`, `kernel/include/<name>.h`; add `$(BUILD)/<name>.o` to `OBJS` and `<name>` to `SHARED64` |
| a test group | `scripts/tests/t_<name>.py`, registered in `GROUPS` in `scripts/run_tests.py` |

Test helpers read layout constants directly from C sources
(`_kernel_define()` in `scripts/ptest.py`). When you change a window
layout, keep its `#define`s as plain integers and update the helpers.

### Releases and updates

`.github/workflows/release.yml` runs on every push to `main`:

1. Builds with `VERSION=<workflow run number>`. The version is stored in
   the initrd as the file `version`.
2. Runs `scripts/mkupdate.py` to write `update.txt`: the version, then the
   size and SHA-256 of the kernel, initrd and loader. The last line is an
   ECDSA P-256 signature over everything before it.
3. Publishes the release `v<n>` with the update files, the ISO and the
   compressed stick image.

On an installed system, `update` (`user/update.c`) downloads `update.txt`
from `https://github.com/DaniuPl/ThreadOS/releases/latest/download/` and
checks the signature against the public key compiled into the program.
It then checks every file's size and hash. Only after that does it write
the files to the EFI partition, through `SYS64_UPDATE_WRITE`, which is
admin only.

**The signing key.** The private key exists only in the repository secret
`THREADOS_UPDATE_KEY` (PEM, P-256). It must never be committed. To create
or rotate it:

```sh
openssl ecparam -name prime256v1 -genkey -noout -out update-key.pem
openssl ec -in update-key.pem -pubout -outform DER | tail -c 65 | xxd -p -c 65
```

1. Put the PEM in the secret.
2. Replace `official_key[]` in `user/update.c` with the 65 printed bytes.
3. Release once. Systems older than that release will refuse updates
   signed with the new key until they are updated by hand, or until an
   admin writes the new key in hex to `update.key`.

### Generated files

These files are committed. Regenerate them only when their inputs change:

| File | Generator |
|---|---|
| `fonts/*.ttf` | `scripts/mkfonts.py <dir with DejaVu fonts>` (needs fonttools) |
| `user/caroots.h` (built-in CA roots) | `scripts/mkcaroots.py` (reads the host CA bundle) |
| `user/certdata.h` (certificate test data) | `scripts/mkcertdata.py` (needs openssl) |
| `docs/*.png` | `scripts/screenshots.py` (needs a built system, OVMF and Pillow) |

### Third-party code

`third_party/` holds upstream releases, trimmed but not modified. See
`third_party/README.md` for versions and what was left out. To update
one, replace its directory. ThreadOS-specific changes to musl live in
`libc/overlay/`, changes to litehtml as patches in `apps/browser/patches/`.
QuickJS is wrapped by `apps/js/`. After an update, run
`make test T=libc`.

## Features

- **Desktop:** a compositor with a glass menu bar, a dock, rounded
  windows with shadows, dark and light themes, wallpapers, animations,
  notifications, search (Ctrl+Space), four virtual desktops, snapping
  and Alt+Tab. It runs at the panel's native resolution, with HiDPI
  scaling.
- **Programs:**
  - file manager (folder tree, cut/copy/paste)
  - terminal (Tab completion, pipes)
  - text editor
  - web browser (litehtml, HTML and CSS)
  - settings, task manager, calculator, calendar, paint, mixer
  - picture viewer (PNG, JPEG, BMP)
  - 64-bit media player (MP3, WAV, AAC, FLAC, Ogg, M4A, MP4/MOV, Matroska, AVI)
  - installer
  - `update`
  - `js` (QuickJS)
- **Hardware:**
  - storage: AHCI, NVMe and ATA
  - USB: xHCI, EHCI and UHCI host controllers; HID keyboard and mouse,
    mass storage, RNDIS Ethernet and Bluetooth HCI
  - network cards: RTL8139 and e1000
  - Intel HD Audio
  - PS/2 keyboard and touchpad with wheel
  - ACPI, SMP
- **Kernel:**
  - processes and threads on all cores; `fork`/`exec`, copy-on-write,
    demand paging and swap
  - signals, pipes, shared memory, memory-mapped files
  - shared libraries
  - users, an admin role and `sudo`
- **Storage:** TFS3, our own filesystem (directories, files up to
  64 MiB), and FAT12/16/32 with long names.
- **Network and security:**
  - ARP, IPv4, ICMP, UDP, TCP, DHCP and DNS
  - TLS 1.3 with X.509 validation
  - AES-GCM/CCM, ChaCha20-Poly1305, X25519, ECDSA, RSA, SHA-1/2
  - WPA2 key derivation

## Roadmap

- Wi-Fi: a driver for a common laptop chip and an 802.11 layer. The WPA2
  cryptography is already done.
- Ethernet chips found in laptops (Realtek RTL8111/8168).
- Images and external style sheets in the browser.
- Removing the big kernel lock from drivers, the filesystem and swap.

## Contributing, security, license

- **[Discord](https://discord.gg/AyaEyTSG6)**: questions, ideas, screenshots from real hardware, and
  help with getting ThreadOS to boot on your machine.
- [CONTRIBUTING.md](CONTRIBUTING.md): review rules. The first one is that
  every feature comes with a test that would fail without it.
- [AGENTS.md](AGENTS.md): instructions for AI coding assistants.
- [SECURITY.md](SECURITY.md): reporting vulnerabilities, and the update
  trust model.
- [docs/JOURNAL.md](docs/JOURNAL.md): the development journal, with the
  reasoning behind every major decision.

Licensed under the MIT license (see [LICENSE](LICENSE)). `third_party/`
keeps its upstream MIT licenses. `fonts/` is under the Bitstream Vera /
DejaVu license ([fonts/LICENSE-DejaVu.txt](fonts/LICENSE-DejaVu.txt)).
