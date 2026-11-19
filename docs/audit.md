# ThreadOS audit: what stands between it and a system that runs on its own

Written on 2026-11-19 at commit `10d752b` from a read of the code; line
numbers may have moved since. Nothing was built or run for it. It is
meant as a work list: each section can go to a separate session or
contributor, and each fix still follows [AGENTS.md](../AGENTS.md) (a
test that fails without it, no weakened tests).

The filesystem is covered separately, with a full plan, in
[tfs4-plan.md](tfs4-plan.md).

How to read the tags:

- **(V)** - read in the code by the auditor.
- **(I)** - inferred (from the code plus knowledge of hardware or of
  other systems); worth confirming before acting on it.
- **re-checked** - the key lines were read a second time, independently,
  and say what the finding says.

Severity: **blocker** (stops standalone use or loses data), **major**,
**minor**.

## Summary

ThreadOS is ahead of Linux 1.0 (March 1994) in what did not exist in
1994: 64-bit long mode with NX and SMEP, SMP in name, UEFI boot and an
installer, AHCI/NVMe/xHCI, TLS 1.3, signed updates, a compositing
desktop, WebKit, a test suite. It is behind Linux 1.0 in the things
that made 1.0 a system people could live in:

1. **No way to build software on it** - no shell, no compiler, no
   environment variables, exec only by a short name (userland).
2. **No Unix terminal or process model** - no tty/pty/termios, no job
   control, unreliable `wait`, no SIGCHLD (kernel core).
3. **A filesystem of 64 files in 32 MiB** - [tfs4-plan.md](tfs4-plan.md).
4. **Modern laptops mostly do not come up usable** - a framebuffer
   above 4 GiB is dropped, one xHCI controller only, no MSI/IOAPIC,
   4 GiB of RAM and 8 cores at most, one Wi-Fi chip from 2013
   (hardware).
5. **A TCP stack for a client on a good link** - four packets disable
   any server, blind RST/FIN injection, stop-and-wait sending, no
   loopback (network).
6. **Data safety** - disk write caches are never flushed (hardware); an
   unprivileged program can halt the kernel (security).

## Suggested order

Tracks are independent of each other unless noted.

**A. Small fixes with a large payoff** (each is a day or two, each has an
obvious test):

1. TCP: free dead and never-accepted children of a listener; check RST,
   FIN and the SYN-RECEIVED ACK against the window (RFC 5961); random
   ephemeral ports (network 1, 2).
2. A kernel fault on a user address inside a system call returns
   `EFAULT` (an exception-fixup table) instead of halting the machine
   (security 1).
3. Flush disk caches: NVMe Flush and shutdown notification, ATA
   FLUSH CACHE EXT on AHCI, SYNCHRONIZE CACHE for USB disks - at
   shutdown and on `fsync` (hardware 5).
4. DNS: a random query ID and source port, and check that the reply
   came from the server asked (network 6).
5. The account salt from `random_bytes` instead of timer ticks
   (security 9).
6. AHCI: read the whole LBA48 capacity and clamp to 2 TiB, instead of
   showing a 3 TB disk as 746 GiB (hardware 11).

**B. The filesystem** - [tfs4-plan.md](tfs4-plan.md). Stage 4 there
(real `stat`, `chmod`, times) is a prerequisite of track D.

**C. Hardware for 2018-2026 PCs:** framebuffer above 4 GiB; every xHCI
controller; IOAPIC and MSI/MSI-X, then interrupts instead of polling;
memory above 4 GiB; more than 8 cores and x2APIC; Intel VMD; HDA
controllers with a DSP and the EAPD verb; I2C-HID touchpads; network
chips (Intel I219, RTL8125, USB CDC-NCM/ECM for phone tethering,
Intel's newer Wi-Fi); a timer that does not depend on the PIT.

**D. The Unix process and terminal model, then self-hosting:**
environment and auxiliary vector; exec by path, `PATH`, `#!`, a larger
argv; per-process exit status, reparenting, SIGCHLD, `wait4(-1,
WNOHANG)`; process groups, sessions, SIGSTOP/SIGCONT; a tty layer with
termios and ptys, and a VT100 emulator in the terminal; dirfd-correct
`*at` calls. Then a shell and a toolset, then a compiler and make.
License note (AGENTS.md rule 6): dash (BSD), toybox (0BSD), tcc
(LGPL), bmake (BSD) or pdpmake (public domain) fit; bash, busybox, GNU
make and gcc are GPL and need the owner's decision.

**E. Kernel scaling and robustness:** a larger (growable) kernel heap;
guard pages under kernel stacks and IST stacks for double fault and
NMI; a panic that leaves a log and can reboot; programs on every core
by default; wait queues instead of 10 ms polling; `munmap` that gives
memory back, a real `mprotect`; a larger swap; the big kernel lock out
of drivers, the filesystem and swap.

**F. A complete TCP/IP stack:** a sliding window with a retransmission
queue, partial ACKs, RTO estimation with backoff, the MSS option,
window scaling, an out-of-order queue, TIME_WAIT, keepalive; TCP
loopback; DHCP renewal; IP reassembly; ARP that expires and checks;
privileged ports; larger socket tables.

**G. Hardening:** stack protector; ASLR and PIE for programs; the
kernel image not writable and executable at once; SMAP; KPTI on CPUs
affected by Meltdown; WebKit's sandbox; fuzz harnesses for DHCP, USB
descriptors, GPT/MBR and Bluetooth.

---

## 1. Kernel core

### Findings

1. **Blocker: no terminal layer and no job control.** (V)
   - Keyboard input is a 128-byte queue per virtual console, polled every
     10 ms (`kernel64/keyboard64.c:28`, `:217`). No tty, pty or termios
     code exists in `kernel/` or `kernel64/`.
   - The overlay answers `TIOCGWINSZ` with a fixed 25x80
     (`libc/overlay/src/threados/syscall.c:750-756`); `setpgid` and
     `setsid` are no-ops (`syscall.c:1659-1660`); `SIGSTOP` ends the
     program (`kernel64/signal64.c:132`, re-checked); Ctrl+C reaches one
     "deepest descendant" (`task64_foreground`).
   - Consequence: no full-screen programs, no Ctrl+Z/bg/fg, no remote
     login.
2. **Major: the process life cycle is not POSIX.** (V)
   - Exit codes live in one global ring of 16 entries that overwrites
     the oldest (`kernel64/task64.c:492`, re-checked): a late `wait` can
     lose a child's status.
   - Orphans are never reparented; `wait` polls every 10 ms
     (`task64.c:580`); `waitpid(-1, WNOHANG)` returns `ENOSYS`
     (`syscall.c:1229-1230`); a child killed by any signal reports
     SIGKILL (`syscall.c:1239`); `getppid()` is always 1
     (`syscall.c:1651`).
   - SIGCHLD is never sent (only listed as ignored, `signal64.c:115`);
     no SIGPIPE.
3. **Major: exec is not Unix exec.** (V)
   - The program name is at most 15 characters (`kernel64/proc64.c:217`
     `char kname[16]`, re-checked), looked up in the initrd and then the
     working directory only (`kernel/prog.c:54`); the overlay strips the
     directory from the path (`syscall.c:1264-1269`).
   - No environment (`libc/overlay/crt/crt1.c:4`, re-checked); argv
     capped at 16 arguments of 256 bytes (`kernel64/proc64.h:18-19`,
     re-checked), extra ones dropped silently (`proc64.c:126`); no `#!`,
     no PATH.
4. **Major: a small fixed kernel heap backs nearly everything.** (V)
   - 1/16 of RAM, between 1 and 8 MiB (`kernel/heap.c:28-29`,
     re-checked); first-fit, merging only with the right-hand neighbour
     (`heap.c:149-153`).
   - It holds every 16 KiB kernel stack (`task64.c:40, 349`), task
     structures, fd tables, and all pipe and socket data (`ipc.c:166`,
     up to 256 KiB per end, `ipc.c:50`).
   - (I) A few hundred threads in total; a few full pipes make writes
     fail with `ENOMEM`; fragmentation over a long uptime; no rlimits,
     so a fork bomb starves the kernel.
5. **Major: SMP is mostly nominal.** (V)
   - Every system call but seven trivial ones takes the big lock
     (`kernel64/syscall64.c:315-330`); disk I/O is polled with it held
     (`kernel/ahci.c:21`).
   - Tasks run on core 0 unless marked `anycore` (`task64.c:695`);
     program threads are (`task64.c:1199`), but processes the desktop
     starts are not (`proc64.c:305-319`, `compat64.c:141`).
   - At most 8 cores (`kernel/include/smp.h:7`, re-checked); interrupts
     only through the 8259 PIC (`isr64.c:41` `irq_handlers[16]`,
     re-checked; no IOAPIC or MSI code anywhere); waking a task sends no
     IPI, so idle cores notice up to 10 ms late.
6. **Major: memory ceilings and reclaim.** (V)
   - Physical memory capped at 4 GiB (`kernel/pmm.c:30` `MANAGED_LIMIT`,
     re-checked); (I) about 3 GiB usable on a 16 GiB machine.
   - Swap at most 16 MiB (`kernel/swap.c:28`, re-checked), and it never
     takes shared or copy-on-write pages (`paging64.c:820`).
   - Out of memory: the process that faults dies; no OOM policy.
   - `munmap` never returns pages to the kernel (`syscall.c:1077-1090`;
     the comment at `:829-835` names an unfound race); after 256 holes
     ranges are forgotten (`:867`) - a leak.
7. **Major: kernel faults.**
   - (V) A kernel exception prints and stops the core for good
     (`kernel64/isr64.c:412-414`, re-checked): no reboot, no saved log.
   - (V) No IST stacks (`kernel64/idt64.c:45` `ist = 0`, re-checked) and
     no guard page under kernel stacks. (I) A kernel stack overflow
     corrupts the heap or triple-faults.
8. **Minor: memory API gaps.** (V) `mprotect` is a no-op
   (`syscall.c:1595-1596`), `MAP_FIXED` refused (`:1029`), shared file
   mappings `ENODEV` (`:1036`), `sigaltstack` a no-op (`:1701`), 4 file
   mappings per process (`task64.h:201`), a fixed layout with no ASLR
   (`elf64.h:30-62`), brk heap about 240 MiB.
9. **Minor: time.** (V) A 100 Hz PIT (`kmain64.c:348`): sleeps and
   `clock_gettime` resolve to 10 ms; wall clock is the CMOS reading
   plus ticks; no `clock_settime`, NTP or time zones; 32-bit tick
   comparisons wrap after 497 days (`task64.c:707`).
10. **Minor: scheduler and latency.** (V) One task list scanned on every
    tick on every core under one spinlock (`task64.c:700-719`); no
    priorities; interrupts off for each whole `printk` including UART
    busy-waits (`printk.c:76`, `serial.c:26`); page reclaim writes swap
    under `irq_save` (`task64.c:1518`).
11. **Minor: tables.** (V) 128 futex waiters machine-wide, overflow
    becomes busy-waiting (`futex64.c:23, 69-73`); 1024 IPC ends, 64 per
    poll, 256 memfds (`ipc.c:49-54`); `poll` reports regular files and
    the console as always ready (`socket.c:1183-1185`); only FXSAVE
    state, so no AVX (`fpu64.c:60`).
12. **Minor, possible race.** (I) Task-list walks under `irq_save` only
    (`has_children`, `task64.c:533`) while another core frees zombies
    (`task64.c:647-660`): a possible use-after-free.

### Solid already

- User pointers are checked at every page-table level for the USER bit,
  with copy-on-write split and paging-in, at about 70 call sites
  (`paging64_user_range` / `_writable`); SMEP and NX on.
- User faults become Linux-style signals with `siginfo`; only the
  program dies (`isr64.c:322-388`).
- Copy-on-write fork, demand-zero memory, anonymous page faults without
  the big lock (`isr64.c:89-127`).
- Threads: clone, futex, `exit_group`, `clear_tid` as in Linux; signal
  masks, `sigsuspend`, restarted calls.
- Pipes and socket pairs that block and wake, pass descriptors; memfd;
  256 descriptors per process with CLOEXEC; per-core APIC timers and
  TLB shootdown.

## 2. Userland and the program interface

### Findings

1. **Blocker: nothing on ThreadOS can build software.** (V) No compiler,
   assembler, linker, make or shell in the initrd (`Makefile:1946-2045`);
   the only interpreters are `js`, `jsc` and `sqlite3`. The IDE's C and
   C++ templates have no run command (`user64/gcode.c:1399`). Porting
   tcc also meets: an ELF loader that takes only `ET_EXEC`
   (`kernel64/elf64.c:75`) at a fixed address (`libc/libc64.ld`), exec
   by basename only, argv of 16, mtime always 0, no `#!`.
2. **Blocker: no POSIX shell.** (V) gterm's built-ins are
   `cd clear exit help whoami sudo` (`user/gterm.c:486-555`), with one
   `|` and no redirection, quoting, `;`, `&&`, `&`, variables, globbing
   or scripts; words capped at 8 arguments of 31 characters
   (`user/uterm.h:6-7`). `ush64` is a demonstration (80-character lines,
   `user64/ush64.c:19, 72`). No `sh`, so `system()` and `popen()` fail
   in every ported program.
3. **Blocker: no environment variables.** (V) `crt1.c` builds an empty
   `environ`; `execve` drops `envp`. No PATH, HOME, TERM, LANG.
4. **Major: the terminal is not a tty.** (V) gterm sends a child's
   output to a file `termout` in the working directory
   (`gterm.c:53`, re-checked) and polls it; (I) commands fail in a
   directory the user cannot write. No escape-sequence parser; `ioctl`
   answers only `TIOCGWINSZ`, so `tcgetattr` fails; no `/dev/tty`,
   `/dev/ptmx`. (I) No vi, less, top or curses.
5. **Major: the toolset is tiny.** (V) `user/core.c` has `ls pwd cat
   head wc cp mv rm mkdir touch echo ps kill free uptime date uname
   hostname set` with few flags, plus `curl`, `sqlite3`, `js`,
   `nslookup`, `httpget`, `ifconfig`, `lspci`, `tapp`, `update`.
   Missing: grep, sed, awk, sort, uniq, tail, find, xargs, diff, tr,
   cut, tee, chmod, ln, tar, gzip, unzip, less, vi, rmdir, du, df, env,
   test, sleep, which.
6. **Major: file metadata is faked.** (V) Times always 0; modes fixed at
   `0644`/`0755`; every directory has inode 2000; `chmod`, `fchmod`,
   `utimensat` return 0 and do nothing; `umask` returns 022; `readlink`
   `EINVAL`; `link`, `symlink`, `chown`, `statfs` `ENOSYS`. The kernel
   has no stat, owner or time call. (Fixed by TFS4 stage 4.)
7. **Major: `*at()` calls ignore the directory descriptor.** (V)
   `unlinkat`, `mkdirat`, `renameat` resolve against the working
   directory (`AT_REMOVEDIR` ignored); `openat` and `newfstatat` with a
   real dirfd return `ENOSYS`; `fchdir` `ENOSYS`; `opendir` and stat of
   a directory change the process's directory for a moment
   (`socket.c:584-599`), which threads do not survive. (I) A ported
   `rm -r` could delete in the wrong directory.
8. **Major: process control too thin for a shell.** See kernel core 2;
   also `alarm`, `setitimer`, `timer_create` `ENOSYS`.
9. **Major: software arrives only from the vendor or by hand.** (V)
   `tapp install` accepts only packages signed by the ThreadOS key
   (`user/tapp.c:120`); otherwise an ELF fetched with curl and run by
   name. One shared library, `libthread.so`, at a fixed address
   (`kernel/dynlink.c`); musl programs are static.
10. **Major/minor: users.** (V) `getuid` and friends return 0 for an
    admin and 1000 otherwise (`syscall.c:1652-1656`); no `/etc/passwd`
    or `/etc/group`; `setuid`, `setgid`, `getgroups` `ENOSYS`; at most
    24 accounts, 8-bit uid, 15-character names (`user/userctl.h`); two
    levels, admin and plain.
11. **Minor: memory.** See kernel core 8.
12. **Minor: locks and sync.** (V) `F_SETLK` returns 0 with the comment
    "one program at a time: no locks" (`syscall.c:736`, re-checked),
    although many programs run at once - (I) concurrent SQLite access
    is unsafe; `fsync` and `sync` do nothing; no named `AF_UNIX`
    sockets.
13. **Two program worlds.** (V) Freestanding programs (`core`, `gterm`,
    the `g*` apps) call `usys64.h` directly, with no errno, reads of
    4 KiB at most and no stat; musl programs get POSIX built over the
    same narrow interface. Most gaps above are gaps in the kernel
    interface that the overlay papers over.
14. **Desktop applications** where something is clearly missing: gedit
    holds 200 lines of 96 characters (`user/gedit.c:41-42`; gcode is
    the real editor); the calendar has no events; gweb is text-only
    (Weft is the real browser); no mail client, PDF viewer, archive
    tool or office suite.

### Solid already

- Copy-on-write fork, exec, pipes, `dup2`, close-on-exec in the kernel.
- pthreads and futexes; signals with `SA_SIGINFO` and `ucontext`,
  `sigprocmask`, `sigsuspend`.
- BSD sockets over TCP/UDP with poll/select and non-blocking mode; curl
  and TLS with Mozilla's roots.
- Large ports run: SQLite, QuickJS, JavaScriptCore, WebKit, GStreamer.
- Programs run from disk, large ones streamed in (`kernel/prog.c`).
- Signed `.tapp` packages (ECDSA P-256 over SHA-256), installed only by
  an admin; `sudo` with a recent-authentication window.

## 3. Hardware

Real hardware so far: a 2014 Broadwell Lenovo IdeaPad and a Dell
(`docs/JOURNAL.md` around lines 3707, 5265, 6397). What is proven is
2014-era.

### Findings

1. **Blocker: a framebuffer above 4 GiB is dropped - a black screen.**
   (V) `kernel/mb2.c:226` `if (f->type == 1 && (f->addr >> 32) == 0)`
   (re-checked); `video_adopt_lfb(uint32_t addr, ...)` (`video.h:23`).
   (I) Intel 11th-generation and later iGPUs, and desktops with
   Above-4G decoding or Resizable BAR, put it there; UEFI has no text
   mode to fall back to.
2. **Blocker: Intel VMD ("RAID On") hides the NVMe disk.** (V)
   `kernel/nvme.c:316` matches only class 01/08/02; no VMD code. (I) The
   factory setting on many 11th-gen+ laptops; switching to AHCI breaks
   Windows' boot.
3. **Blocker for networking: almost no current chip.** (V) Wi-Fi: the
   Intel 3160 only (`kernel/iwlwifi.c:2075`). Wired: RTL8168/8161
   (`rtl8168.c:243`) and the 82540EM QEMU emulates (`e1000.c:41`); no
   Intel I219, I225/I226 or RTL8125. USB: RNDIS only (`usbnet.c`), no
   CDC-ECM/NCM, RTL8153 or AX88179. (I) Ultrabooks without RJ45 are
   left without a network.
4. **Blocker/major: the pointer.** (V) Only the first xHCI controller
   (`kernel/xhci.c:760`, re-checked). (I) On Intel Ice Lake to Meteor
   Lake that is the Type-C controller at 00:0d.0, before the chipset's
   00:14.0 with the USB-A ports, Bluetooth and the webcam. No I2C
   controller or I2C-HID (`hwinfo.c:145`): (I) touchpads since about
   2016 do not work. Typical 2020+ Intel laptop: keyboard yes (PS/2
   through the EC), pointer no.
5. **Major, data safety: disk write caches are never flushed.** (V) NVMe
   defines no Flush and no shutdown notification (`nvme.c:57-63`); AHCI
   has only READ/WRITE DMA EXT and IDENTIFY (`ahci.c:96-98`, re-checked:
   no FLUSH in either file); USB disks get no SYNCHRONIZE CACHE;
   shutdown goes straight to `acpi_poweroff()` (`desktop.c:6127`). Only
   the legacy PIO driver flushes (`ata.c:168`).
6. **Major: Secure Boot must be off** (README: "threadboot is not
   signed"). (I) On a Windows 11 PC with BitLocker or Device Encryption
   that asks for the recovery key.
7. **Major: interrupts and the timer.** (V) No IOAPIC, MSI/MSI-X or
   x2APIC code; AHCI, NVMe, xHCI, HDA, Wi-Fi and RTL8168 poll. The boot
   core's clock is the PIT on IRQ0 (`pit.c:68`, `smp.c:378`). (I) Some
   Skylake+ products gate the PIT off: no tick there.
8. **Major: cores and memory.** (V) 8 logical CPUs at most
   (`smp.h:7`); only MADT type-0 entries (`smp.c:519`), xAPIC by MMIO
   only - (I) firmware that hands over in x2APIC mode leaves one core;
   4 GiB managed (`pmm.c:30`).
9. **Major: sound.** (V) `hda.c:685` requires prog-if 0x00 (re-checked);
   (I) Intel controllers with a DSP (prog-if 0x80, class 0x0401 with
   digital microphones) are skipped on many 2018+ laptops. When found,
   the first DAC and first output pin are taken (`hda.c:266-272`) with
   no path tracing, no EAPD verb, no jack detection, no HDMI audio.
10. **Major: power and display.** (V) No suspend (S3 or S0ix), lid,
    thermal zones, CPU frequency, AC adapter, backlight
    (`desktop.c:8045`); power button and battery work (`power.c:14-20`).
    Resolution changes only on QEMU's Bochs interface
    (`video.c:505-530`); no GPU driver, external monitor or acceleration.
11. **Minor: 2 TiB.** (V) 32-bit LBAs (`blk.h:60-61`); NVMe clamps
    (`nvme.c:286`); **AHCI reads only the low 32 bits of the LBA48
    count** (`ahci.c:242-244`, re-checked), so a 3 TB disk shows as
    about 746 GiB; USB disks over 2 TiB refused (`usbmass.c:227`);
    `fatcmd.c:40` and `install.c:278` cut a 64-bit partition start to
    32 bits.
12. **Minor: storage.** (V) 4 KiB-sector disks refused (`nvme.c:278`,
    `usbmass.c:222`); first NVMe and first AHCI controller only; two
    USB disks; NVMe one command at a time through a 4 KiB bounce buffer
    (`nvme.c:104`); AHCI never resets a port or spins a disk up.
13. **Minor: USB.** (V) Only the USB 2 hub descriptor (`usbhc.c:574`);
    (I) USB 3 hubs fall back; 4 hubs; no USB audio, serial adapters,
    printers, UAS or Thunderbolt; Bluetooth is discovery only, no L2CAP
    or pairing (`bt.c:12-14`).
14. **Minor: ACPI and PCI.** (V) The kernel's own table reader uses the
    RSDT, never the XSDT (`acpi.c:216`; ACPICA itself uses the XSDT);
    PCI configuration only through 0xCF8/0xCFC (no ECAM, 256 bytes,
    segment 0).

### Solid already

- UEFI boot through its own loader, a hybrid BIOS/UEFI GRUB disc and a
  USB image; firmware boot entries written (`kernel64/efirt.c`); the
  loader works above 4 GiB.
- The GOP framebuffer mapped write-combining; EDID-based HiDPI.
- PCI walked behind bridges; AHCI with DMA on several ports; NVMe with
  64-bit BARs; USB disks hot-plugged.
- xHCI taken over from the firmware, Intel's shared EHCI ports routed
  to it, devices behind hubs, a hotplug thread, a HID report parser, a
  UVC webcam.
- PS/2 keyboard and mouse with wheel; RTL8111/8168 including G/H on the
  IdeaPad; a WPA2-PSK supplicant; ACPICA for the power button and
  battery; reboot through the ACPI reset register.

## 4. Network

### Findings

1. **Blocker: four packets disable any TCP server.** (V, traced)
   `accept_syn` counts every unaccepted child as queued whatever its
   state (`kernel/tcp.c:297-303`, re-checked); a reset or timed-out
   child only becomes `TCP_CLOSED` and stays `used`
   (`tcp.c:394-397`, `777-780`); `tcp_accept` skips non-ESTABLISHED
   children (`617-618`) and `tcp_reap` frees only connections of dead
   processes (`151-157`, re-checked). One port scan, or spoofed SYNs,
   and the listener refuses everybody until its process exits. 32
   connections for the whole system (`tcp.c:36`), no per-process cap,
   no keepalive.
2. **Major: blind injection.** (V, traced) Any RST with a valid
   checksum kills a connection, its sequence number unchecked
   (`tcp.c:394-398`, re-checked); a FIN is taken if
   `seq_le(seq, c->rcv_nxt)` (`tcp.c:453`) - half the sequence space;
   in SYN-RECEIVED any ACK establishes the connection
   (`tcp.c:426-430`), so a spoofed connection can send data under
   another address; ephemeral ports are sequential (`tcp.c:506-510`).
3. **Major: the sender can corrupt the byte stream.** (V, traced; needs
   a peer that acknowledges part of a segment) The peer's window is
   stored but never read; any advancing ACK clears the whole
   outstanding chunk (`tcp.c:408-409`), so an unacknowledged tail is
   never resent; new data goes at `snd_nxt` (`:654`), retransmits at
   `snd_una` (`:240`). A zero window for about 4 s kills the
   connection.
4. **Major: robustness and throughput.** (V) A fixed 500 ms
   retransmission timer with no backoff, 8 tries - about 4 s
   (`tcp.c:48-49`, re-checked): a Wi-Fi roam kills every connection.
   Stop-and-wait: one 1460-byte segment per ACK (`tcp.c:38`). No MSS
   option, so peers use 536 bytes. An 8 KiB receive window, no window
   scaling, SACK, congestion control or TIME_WAIT; out-of-order
   segments dropped (`tcp.c:444-450`); blocking calls poll every 10 ms.
5. **Major: no TCP loopback.** (V) 127/8 and the own address work only
   for UDP (`kernel/net.c:491`); TCP to 127.0.0.1 is sent to the
   gateway (`net.c:769`).
6. **Major: DNS answers are easy to forge.** (V) The native resolver
   uses the fixed ID `0x5053` and accepts a reply from anyone
   (`user/udns.h:74, 96`, re-checked); used by gweb, httpget,
   libthread. musl's ID comes from `tv_nsec`, which here has 10 ms
   resolution - 100 values. UDP source ports are sequential
   (`net.c:430-438`). HTTPS is protected by the certificate check;
   plain HTTP is not.
7. **Major: DHCP is never renewed.** (V) It runs once at boot, reads
   options 1, 3 and 6, ignores the lease time (`kernel/dhcp.c:144-252`);
   without an answer it falls back to QEMU's 10.0.2.15/24 on a real LAN
   too (`net.c:43-45`).
8. **Minor: IPv4 and UDP.** (V) Fragments dropped (`net.c:721-724`); one
   card, one address, on-link plus a default gateway; 8 ARP entries
   that never expire and learn from any ARP packet (`net.c:663, 686`);
   no ICMP errors, no path-MTU discovery; 8 UDP sockets system-wide,
   4 datagrams queued, 1024-byte payloads (`net.c:65-67`); no IPv6, no
   multicast; no privileged ports.

### Solid already

- IP, ICMP, UDP and TCP checksums verified before any field is used;
  random initial sequence numbers; socket ownership checked per
  process; broadcast pings not answered; nothing listens by default.

## 5. Security

### Findings

1. **Major: an unprivileged program can halt the kernel.** (V, traced,
   not reproduced) A buffer is checked when a blocking call comes in
   (`kernel64/syscall64.c:806-818`, re-checked); the call sleeps, which
   releases the big lock (`task64.c:818-823`, re-checked); another
   thread calls `SYS64_DISCARD` on that page (`syscall64.c:1192-1200`);
   on waking the kernel writes the buffer (`net.c:535` with interrupts
   off, `syscall64.c:825`, `tcp.c:707`). A kernel-mode fault only tries
   `paging64_swap_in` (`isr64.c:315-320`, re-checked), which cannot help
   with a discarded page, so the system stops. UDP loopback makes it
   local-only. Other blocking calls likely share the pattern. Fix: an
   exception-fixup table so the copy returns `EFAULT`.
2. **Major: little hardening for programs that parse untrusted input.**
   (V) No ASLR (fixed addresses, `elf64.h:30-55`; programs linked
   `-static -no-pie`); `-fno-stack-protector` everywhere (12 places in
   the `Makefile`); WebKit built with `-DENABLE_BUBBLEWRAP_SANDBOX=OFF`
   (`Makefile:1783`). A memory bug in WebKit, GStreamer or curl is a
   full compromise of the user.
3. **Major: kernel hardening.** (V) The kernel image is mapped writable
   and executable (`paging64.c:1114-1119`); kernel stacks have no guard
   page; SMAP, KPTI and KASLR are off (documented in SECURITY.md).
   (I) Without KPTI a Meltdown-affected Intel CPU lets a program read
   all of RAM through the direct map.
4. **Minor: passwords.** (V) The account salt is timer ticks through an
   LCG (`kernel/user.c:278-289`, re-checked) while the PIN salt uses
   `random_bytes` (`user.c:1031`); PBKDF2-SHA1 with 4096 rounds and a
   default password, both documented in SECURITY.md.
5. **Minor: unfuzzed parsers reachable from outside.** DHCP options,
   Bluetooth HCI events, USB descriptors (xhci, usbnet, usbmass, uvc),
   GPT/MBR. Two one-byte over-reads in Bluetooth: `ad_data` lets
   `i + flen == len` through and reads `p[len]` (`bt.c:168-178`); the
   RSSI read in `le_meta` (`bt.c:196-201`). Very likely harmless.
6. **Minor: trust model.** No revocation, keyUsage/EKU, pathLen or name
   constraints in certificate checks, and the clock is CMOS without NTP
   (`user/ucert.h:207-246`); updates never expire (a freeze attack is
   possible); `.tapp` packages loaded at boot are not signature-checked
   (`initrd64.c:25`) - consistent with trusting the EFI partition while
   Secure Boot is off.

### Solid already

- A BLAKE2s pool feeding ChaCha20 with fast key erasure, RDSEED/RDRAND
  and interrupt timing (`kernel/random.c:284-336`); `getrandom` waits
  for the seed.
- NX on, W^X for ELF and mmap, SMEP where the CPU has it
  (`paging64.c:955-969`); `sigreturn` masks IOPL and TF; no I/O
  permission map in the TSS; the return to ring 3 uses `iretq`.
- Updates: ECDSA P-256 over the manifest, SHA-256 per file, the version
  must rise (`user/update.c:351-360`).
- TLS: host name, dates, the CA flag, SHA-256 or stronger only.
- The password database is unreadable to plain users; fuzz harnesses
  for the kernel packet path, WPA, 802.11, HID, TTF and FDT.

## Compared with Linux 1.0, area by area

| Area | ThreadOS against Linux 1.0 |
|---|---|
| CPU and memory | ahead: 64-bit, NX, SMEP, threads with futexes, SMP in name; behind: 4 GiB, 8 MiB kernel heap, 16 MiB swap |
| Processes and terminals | far behind: no tty/pty/termios, job control, SIGCHLD, environment, PATH, `#!` |
| Filesystem | far behind: 64 files in 32 MiB against ext2, minix, msdos, iso9660, proc, NFS ([tfs4-plan.md](tfs4-plan.md)) |
| Self-hosting | far behind: 1.0 built itself with gcc; ThreadOS has no shell or compiler |
| Hardware | same strategy (standard classes: AHCI, NVMe, xHCI, HDA, GOP, ACPI) and wider in places (hotplug, battery, webcam); but one NE2000 driver reached most network cards in 1994, and today's networking is split across many chips - a random 2018-2026 laptop is less likely to come up usable than a random 1994 PC was |
| Network | behind: 1.0 had routing, loopback, reassembly and a TCP with a retransmission queue, backoff and window checks, enough for servers; ThreadOS's TCP suits a client on a good link |
| Security | ahead: NX, W^X, SMEP, random ISNs, checked TLS chains, signed updates, PBKDF2 |
| Graphics, browser, media | far ahead: none of it existed in 1.0 itself |

One stale item found along the way: README's Roadmap still says the
Intel 3160 "scans already", while commit `10d752b` has it joining WPA2
networks and carrying traffic.
