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
| Settings | `system.cfg` on the disk — machine name, name server, prompt colour, desktop background |

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
writing*, a stack that grows on demand, processes **and the threads of
one process** running in ring 3 on every core — a changed mapping is
sent to the other cores and awaited before the frame is reused — TLS
1.3 with certificate verification, and a graphical desktop with
windows that can be moved, resized, maximised to fill the screen and
closed like anywhere else — the compositor asks an application to end
and insists if it will not — twelve of them at once, on a taskbar that
shares its width out among their buttons instead of letting them run
off the edge, with a clock at its right-hand end. Icons, a file
manager, a web browser and a settings window whose changes survive a
restart. The terminal in a window is a dark screen with light text and
a session in it: scrollback, history, a movable cursor, `cd` and pipes,
output that appears while the program is still writing it, Ctrl+C to
stop it, and a pipe into what it started — so the ring 3 shell runs
inside the terminal window, its children writing where it writes, and
Ctrl+D ends it. One program compiled for both kernels, differing only
in how a command is started. Alt+Tab moves the focus from the keyboard, the way
it does everywhere else. Two privilege levels: changing the system —
its settings and the files it keeps them in — needs an admin, and the
way up is a password, asked for by the shell, by the terminal and by
the settings window alike (`sudo`), for one command at a time.

**64-bit kernel** — long mode, four-level paging with NX, `syscall`/
`sysret`, processes with `fork`/`exec`/`wait` and copy-on-write,
signals, file descriptors, pipes, on-demand heap, SSE with FPU state
saved across context switches, a shell in the kernel *and* one in user
space, networking from ring 3, TLS 1.3 with certificate verification,
a graphical desktop whose six applications all run in ring 3, shared
memory, files mapped into memory for reading *and writing*, a stack
that grows on demand, swap to disk, processes that run in ring 3 on
every core, **threads sharing one address space across cores**, four
virtual consoles, USB keyboard (xHCI or UHCI), ACPI power-off, and
BIOS + UEFI boot from a single hybrid disc.

## Quick start

```sh
make run              # 32-bit kernel in QEMU
make kernel64         # build the long-mode kernel + its initrd
make run-iso64-uefi   # boot the hybrid disc under UEFI firmware (OVMF)
make test             # full test suite, groups in parallel (~5 min)
make test T=ush       # one group only
```

Requirements: `gcc` with `-m32` (`gcc-multilib`), `make`,
`qemu-system-x86`. For discs: `grub-pc-bin`, `grub-efi-amd64-bin`,
`xorriso`, `mtools`, `ovmf`.

## Testing

Nearly 800 assertions run real kernels in QEMU and check the result
through QMP: screen contents, pixel colours, serial output, injected
keystrokes. Nothing is mocked — a failing assertion means the system
misbehaved, not that a stub drifted.

The checks are split into groups under `scripts/tests/`, one file per
feature, running in parallel because each needs its own virtual machine
and those spend most of their time waiting. One file per feature costs
a few extra boots, and buys a failure that names the thing that broke
instead of a group that covers a third of the system. Several groups boot QEMU
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
  own clock and the kernel's descriptor tables, and `runw cores` forks
  and shows two processes finishing in the time of one. What a process
  does *inside* the kernel is still serialised by one lock, because the
  drivers, the filesystem and the swapper were written for a single
  core. The first syscalls are out of it already — the ones that read
  nothing but the calling task's own state are on an explicit, audited
  list, and `runw syscalls` shows a million syscalls per process
  running two-and-a-half times faster on four cores than on one. The
  page fault — the commonest way into the kernel there is — is off it
  too: heap, stack, anonymous memory and copy-on-write are served
  before the lock is taken, and the page tables have a lock of their
  own. Eighteen thousand faults, none of them taking the big lock. What
  is left under it are the drivers, the filesystem and the swapper, and
  each has to be read to the end before it can follow.

**Infrastructure**

- Keep an eye on the longest group. `ush` and `tls32` (a ring 3 shell
  driving pipes and redirections, and a dozen TLS 1.3 handshakes
  against a real OpenSSL) now decide how long a full run takes — about
  90 seconds each, out of some 385 for all 50 groups.
- Run on real hardware, not only under QEMU and OVMF.

## Documentation

The detailed development journal — in Polish, with the reasoning behind
each decision and the bugs that shaped it — is in
[DZIENNIK.md](DZIENNIK.md).
