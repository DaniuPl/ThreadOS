# ProtonOS

A hobby operating system for PC, written from scratch in C and assembly.

There are **two kernels and one set of sources**. One runs in 32-bit
protected mode; the other enters long mode (x86-64) and boots on
machines without a BIOS, through UEFI. They are not two copies of the
same thing: drivers, filesystem, networking and console are the *same
files compiled twice*, and only what genuinely depends on the CPU mode —
paging, interrupts, context switching — has separate implementations.

Both kernels are Multiboot-compliant, so GRUB or QEMU can load them
directly.

## What works

Shared by both kernels (same files, compiled twice):

| | |
|---|---|
| Hardware | PIC, PIT, PS/2 keyboard, serial port, PCI, RTC |
| Storage | ATA PIO and AHCI (SATA, DMA) behind one block layer |
| Filesystem | PFS3 — block bitmap, inodes, directories, files up to 64 MiB |
| Programs | ELF from the initrd **or** from disk (`cp`, then `run`) |
| Network | RTL8139, ARP, ICMP, UDP, DHCP, TCP, DNS |
| Display | VGA mode 13h and the loader's framebuffer (24/32 bpp) |

**32-bit kernel** — ring 3 processes with `fork`/`exec`, copy-on-write,
swap to disk, signals, pipes, four virtual consoles, USB keyboard
(UHCI), ACPI, TLS 1.3 with certificate verification, and a graphical
desktop with windows, icons, a file manager and a web browser.

**64-bit kernel** — long mode, four-level paging with NX, `syscall`/
`sysret`, processes with `fork`/`exec`/`wait` and copy-on-write,
signals, file descriptors, pipes, on-demand heap, SSE with FPU state
saved across context switches, a shell in the kernel *and* one in user
space, networking from ring 3, a graphical desktop with ring 3
applications drawing in their own windows, swap to disk, four virtual
consoles, USB keyboard (UHCI), ACPI power-off, and BIOS + UEFI boot
from a single hybrid disc.

## Quick start

```sh
make run              # 32-bit kernel in QEMU
make kernel64         # build the long-mode kernel + its initrd
make run-iso64-uefi   # boot the hybrid disc under UEFI firmware (OVMF)
make test             # full test suite, groups in parallel (~5 min)
make test T=k64       # one group only (~80 s)
```

Requirements: `gcc` with `-m32` (`gcc-multilib`), `make`,
`qemu-system-x86`. For discs: `grub-pc-bin`, `grub-efi-amd64-bin`,
`xorriso`, `mtools`, `ovmf`.

## Testing

Over 400 assertions run real kernels in QEMU and check the result
through QMP: screen contents, pixel colours, serial output, injected
keystrokes. Nothing is mocked — a failing assertion means the system
misbehaved, not that a stub drifted.

The checks are split into groups under `scripts/testy/`, one file per
group, running in parallel because each needs its own virtual machine
and those spend most of their time waiting. Several groups boot QEMU
**twice against the same disk image**, which is the only conclusive
proof that files live on the disk rather than in memory.

`make fuzz` feeds corrupted input to the parsers (certificates, HTML,
HTTP, network frames) under ASan/UBSan on the host.

## Layout

```
boot/  boot64/     entry points: Multiboot header, long-mode entry
kernel/            shared code + 32-bit kernel
kernel64/          only what depends on the CPU mode being 64-bit
user/  user64/     ring 3 programs, packed into an initrd
scripts/           build helpers and the test suite
```

## What's next

Roughly in the order I would do them.

**64-bit kernel — catching up with the 32-bit one**

- More GUI applications in ring 3. The window syscalls work and one
  app ships in the initrd; the clock, file manager and browser icons
  still call 32-bit-only binaries.
- Shared memory for long-mode processes, so a program can hand a
  buffer to another one instead of drawing everything itself.
- TLS and the HTTP client in ring 3: the crypto already lives in user
  space, so this is mostly a matter of building those programs for
  64-bit.

**Things neither kernel has yet**

- `mmap` of files, and a shared library / dynamic linking story.
- More than one CPU: APIC, per-CPU state, real locking. This is the
  largest single item and it touches everything.
- Preemption inside the kernel, which SMP will force anyway.
- Newer hardware: virtio-net or e1000, NVMe, xHCI. The RTL8139 and
  AHCI drivers exist mainly because QEMU offers them.

**Infrastructure**

- Split the `rdzen` test group. It runs 166 assertions on a single
  machine and alone decides how long a full run takes; everything else
  already finishes sooner.
- Run on real hardware, not only under QEMU and OVMF.

## Documentation

The detailed development journal — in Polish, with the reasoning behind
each decision and the bugs that shaped it — is in
[DZIENNIK.md](DZIENNIK.md).
