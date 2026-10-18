# ThreadOS

[![license: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![release](https://img.shields.io/github/v/release/DaniuPl/ThreadOS)](https://github.com/DaniuPl/ThreadOS/releases/latest)

ThreadOS is a hobby operating system for the PC, written from scratch in C
and assembly. It has its own kernel, drivers, filesystem, network stack,
TLS, UEFI boot loader, installer and graphical desktop, and it runs on
real laptops as well as in QEMU.

There are **two kernels built from one set of sources**. One runs in 32-bit
protected mode. The other runs in 64-bit long mode and boots through UEFI.
Drivers, the filesystem, networking and the desktop are the *same files
compiled twice*. Only the parts that really depend on the CPU mode have
separate versions: paging, interrupts, context switching and the system
call entry.

![The desktop: a file manager, a clock and a memory monitor](docs/desktop.png)

<details>
<summary>More screenshots</summary>

![A 1920x1080 panel at 150%: the menu, a calendar and a calculator](docs/sharp.png)

The same desktop on a 1920x1080 panel at 150% scale. Every edge and every
letter is drawn at the panel's own resolution, not stretched.

![The installer](docs/installer.png)

The installer on a UEFI machine. It lists what it will write before it
writes anything.

</details>

All screenshots are taken by `scripts/screenshots.py`, which boots the
system in QEMU and saves the screen. The test suite uses the same tool, so
the pictures show what the kernel really drew.

## Try it

Every push to `main` is built by GitHub Actions and published as a
[release](https://github.com/DaniuPl/ThreadOS/releases/latest):

| File | What it is |
|---|---|
| `threados64.iso` | The live system and the installer. Boots through UEFI or BIOS. |
| `threados-stick.img.xz` | The same system as a USB stick image (UEFI only). |
| `update.txt` and three files | A signed system update, used by `update` (see below). |

**On a real PC:**

1. Write `threados64.iso` to a USB stick. You can use Rufus (choose
   *DD image* mode), balenaEtcher or `dd`.
2. Turn Secure Boot off in the firmware settings, then start the PC from
   the stick.
3. Choose *Install ThreadOS onto this computer's disk*. You can install it
   next to another system on the existing EFI partition, or give it the
   whole disk. The installer adds ThreadOS to the firmware's boot menu.

**In QEMU:** run `make run` (see [Building](#building)).

**Updating an installed system:** `run update check` tells you whether a
newer release exists, and `sudo run update` installs it. Updates are
signed with ECDSA P-256 and every file is checked against its SHA-256
before anything is written. [SECURITY.md](SECURITY.md) explains what is
trusted and why.

## What it can do

**Desktop and programs**
- A compositing desktop with a glass menu bar, a dock and windows with
  rounded corners and soft shadows. It has dark and light themes,
  wallpapers, animations, notifications, a search box (Ctrl+Space), four
  virtual desktops, window snapping and Alt+Tab.
- It uses the panel's native resolution and scales for HiDPI, for example
  1080p at 150%.
- TrueType fonts, anti-aliased and rendered inside the kernel. The glyphs
  match FreeType's.
- Programs, all running in ring 3:
  - **file manager**: folder tree, new folder, cut/copy/paste, rename,
    double click to open
  - **terminal**: Tab completion, history, pipes
  - **text editor**: search, undo, clipboard
  - **web browser**, **settings**, **task manager**, **calculator**,
    **calendar**, **paint**, **sound mixer**
  - **picture viewer** for PNG, JPEG and BMP
- Users, an admin password and `sudo`, a lock screen, log out, restart and
  shut down.
- Polish letters typed with AltGr, stored as UTF-8.

**Hardware**
- Storage: AHCI (SATA), NVMe and ATA disks. USB mass storage.
- USB: xHCI, EHCI and UHCI host controllers. Keyboards and mice (HID),
  RNDIS Ethernet and Bluetooth adapters (HCI).
- Network cards: RTL8139 and Intel e1000.
- Sound: Intel HD Audio.
- Also: PS/2 keyboard and touchpad (wheel and two-finger scrolling), ACPI
  power-off, SMP on all cores.

**System**
- Filesystems: TFS3, our own, with directories and files up to 64 MiB.
  FAT12/16/32 on the EFI partition, with long file names.
- Processes and threads on every core. `fork`/`exec`, copy-on-write,
  demand paging, swap, signals, pipes, shared memory and memory-mapped
  files.
- Shared libraries: `libthread.so` is loaded once and mapped into every
  process.
- Networking: ARP, IPv4, ICMP, UDP, TCP, DHCP and DNS.
- Security: TLS 1.3 with X.509 certificate checking. The system's own
  crypto covers AES-GCM/CCM, ChaCha20-Poly1305, X25519, ECDSA, RSA and
  SHA-1/2. WPA2 key derivation is included as well.
- Booting: **threadboot**, our own UEFI loader, needs no GRUB. It reads the
  kernel from the EFI partition, sets the screen mode and hands over the
  firmware's memory map. GRUB is used for BIOS boot and for the hybrid
  disc.
- Installing and updating: a full-screen installer (GPT, FAT32 EFI
  partition, TFS3 partition, firmware boot entry) and signed updates over
  HTTPS from GitHub Releases.

**Userland**
- Programs can be built against **musl** 1.2.5, unchanged. A thin layer
  turns its Linux system calls into ThreadOS ones.
- **QuickJS** runs on top of it as `js`. Its `console.log` prints exactly
  what Node.js prints.

## Building

You need `gcc` with 32-bit support (`gcc-multilib`), `make`, `python3` and
`qemu-system-x86`. Building discs and images also needs `grub-pc-bin`,
`grub-efi-amd64-bin`, `xorriso` and `mtools`. The EFI loader needs `clang`
and `lld`, and UEFI tests need `ovmf`.

```sh
make run              # the 64-bit system in QEMU, straight to the desktop
make run KVM=1        # the same with hardware virtualisation (/dev/kvm)
make run AUDIO=pa     # with sound (pa, pipewire or alsa)
make run32            # the 32-bit kernel in QEMU
make kernel64         # build the 64-bit kernel and its initrd
make efi              # build threadboot, the UEFI loader
make iso64            # the hybrid BIOS + UEFI disc
make stick            # a bootable USB image (GPT + EFI partition)
make run-stick        # boot that image in QEMU with OVMF
make test             # the whole test suite
make test T=files     # one test group
```

## Testing

The suite has 112 groups and more than 1800 assertions. Each group starts
a real kernel in QEMU and checks the result from the outside:

- the screen, read glyph by glyph
- the serial port
- keys and mouse clicks sent in
- disk images read back with `mtools`
- firmware that either boots the installed disk or does not

Nothing is mocked. Where outside tools can be compared against, they are:
OpenSSL for the ciphers and TLS, FreeType for fonts, Node.js for
JavaScript, glibc for libc output, and `mtools` for FAT.

Groups run in parallel (`make test J=4`). The network groups need to bind
port 53 on the host. Run them as root, or once run
`sudo sysctl -w net.ipv4.ip_unprivileged_port_start=53`. Without that they
skip and say why. `make fuzz` runs the parsers (certificates, HTML, HTTP,
network frames, fonts) under ASan/UBSan on the host.

## Repository layout

```
boot/          32-bit entry, GRUB menus (grub.cfg, grub64.cfg)
boot/efi/      threadboot, the UEFI boot loader (PE32+)
boot64/        long-mode entry, interrupt and syscall stubs
kernel/        shared kernel code and the 32-bit kernel
kernel64/      what only the 64-bit kernel needs
user/          ring 3 programs, built for both kernels
user64/        64-bit-only programs and the 64-bit system call layer
libc/          musl on ThreadOS: the replaced files and the linker script
apps/js/       the `js` host around QuickJS
third_party/   other people's code, unchanged (musl, QuickJS)
fonts/         DejaVu subsets
scripts/       build helpers, release signing and the test suite
tests/fuzz/    host-side fuzzing harnesses
docs/          screenshots and the development journal
```

## Roadmap

- **Wi-Fi.** The WPA2 cryptography is done. Still missing: a driver for a
  real wireless chip and the 802.11 layer.
- **More network cards for real laptops**, starting with Realtek
  RTL8111/8168.
- **A better browser engine**, by porting litehtml on top of the musl
  layer.
- **Retiring the big kernel lock.** Many system calls and page faults
  already avoid it. The drivers, the filesystem and the swapper still
  depend on it.

## Documentation and contributing

- [CONTRIBUTING.md](CONTRIBUTING.md): the rules a patch is reviewed by.
  The most important one is that every feature comes with a test that
  would fail without it.
- [SECURITY.md](SECURITY.md): how to report a vulnerability, and how
  updates are signed.
- [docs/DZIENNIK.md](docs/DZIENNIK.md): the development journal, with the
  reasoning behind each decision. Early entries are in Polish and newer
  ones in English.

## License

MIT, see [LICENSE](LICENSE). `third_party/` contains musl and QuickJS,
both under the MIT license (see their own files). `fonts/` contains
subsets of DejaVu under the Bitstream Vera license
([fonts/LICENSE-DejaVu.txt](fonts/LICENSE-DejaVu.txt)).
