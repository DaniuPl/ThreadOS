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
| Hardware | PIC, PIT, PS/2 keyboard, serial port, PCI, RTC, local APIC |
| USB | xHCI and UHCI behind one host-controller layer; HID boot keyboard |
| Storage | ATA PIO, AHCI (SATA, DMA) and NVMe behind one block layer |
| Filesystem | PFS3 — block bitmap, inodes, directories, files up to 64 MiB |
| Programs | ELF from the initrd **or** from disk (`cp`, then `run`) |
| Network | RTL8139 and e1000 behind one netdev layer; ARP, ICMP, UDP, DHCP, TCP, DNS |
| Ring 3 | The same programs built twice: crypto, TLS 1.3, HTTP, HTML, GUI apps |
| Linking | `libproton.so` — crypto, TLS, HTTP and DNS in one shared object, bound at load time |
| Display | VGA mode 13h and the loader's framebuffer (24/32 bpp) |

Programs that use crypto link against a real shared object: the kernel
loads `libproton.so` once at a fixed address, maps its code and
constants into every process as the *same physical frames*, gives each
process a private copy of its writable data, and fills in the jump
tables from the ELF relocations before the first instruction runs. The
four heaviest programs together are now smaller than the library they
share.

**32-bit kernel** — ring 3 processes with `fork`/`exec`, copy-on-write,
swap to disk, signals, pipes, four virtual consoles, USB keyboard
(xHCI or UHCI), ACPI, files mapped into memory for reading *and
writing*, a stack that grows on demand, processes that run in ring 3 on
every core, TLS 1.3 with certificate verification, and a
graphical desktop with windows, icons, a file manager and a web
browser.

**64-bit kernel** — long mode, four-level paging with NX, `syscall`/
`sysret`, processes with `fork`/`exec`/`wait` and copy-on-write,
signals, file descriptors, pipes, on-demand heap, SSE with FPU state
saved across context switches, a shell in the kernel *and* one in user
space, networking from ring 3, TLS 1.3 with certificate verification,
a graphical desktop whose six applications all run in ring 3, shared
memory, files mapped into memory for reading *and writing*, a stack
that grows on demand, swap to disk, processes that run in ring 3 on
every core, four virtual consoles, USB keyboard (xHCI or UHCI),
ACPI power-off, and BIOS + UEFI boot from a single hybrid disc.

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

The 64-bit kernel has caught up with the 32-bit one: everything the
older one does, the long-mode one does too. What follows is new
ground for both.

**Things neither kernel has yet**

- Getting rid of the big kernel lock. Ring 3 runs on every core: each
  one has its own task state segment, its own syscall entry stack, its
  own clock and the kernel's descriptor tables, and `runw rdzen` forks
  and shows two processes finishing in the time of one. What a process
  does *inside* the kernel is still serialised by one lock, because the
  drivers, the filesystem and the swapper were written for a single
  core. The first syscalls are out of it already — the ones that read
  nothing but the calling task's own state are on an explicit, audited
  list, and `runw wywolania` shows a million syscalls per process
  running two-and-a-half times faster on four cores than on one. Every
  further syscall taken off that lock has to be read to the end first;
  the subsystems come after.
- TLB shootdown. A process that changes its own page tables invalidates
  them on the core it happens to be running on; no other core is told.
  Today nothing suffers, because one process runs on one core at a
  time — threads sharing an address space would be the first to notice.

**Infrastructure**

- Keep an eye on the longest group. `rdzen2` (network, TCP, big files
  over HTTP, the filesystem and a second boot on the same disk) now
  decides how long a full run takes.
- Run on real hardware, not only under QEMU and OVMF.

## Documentation

The detailed development journal — in Polish, with the reasoning behind
each decision and the bugs that shaped it — is in
[DZIENNIK.md](DZIENNIK.md).
