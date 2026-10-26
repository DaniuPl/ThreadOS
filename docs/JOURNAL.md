# ThreadOS - development journal

A chronological record of what was built, and above all **why**. Nearly
every entry explains why something looks the way it does and which bugs
turned up on the way. Those explain the shape of the code better than
the code itself.

How to build, run, test and maintain the system is in
[README.md](../README.md). This file is the history behind it.

Until autumn 2026 the project was called ProtonOS, and older entries
use that name. They also mention earlier names of things that have
since been renamed: the filesystem was PFS3 before it became TFS3, the
initrd format was PIRD before TIRD, and some programs and messages had
Polish names before the system switched to English. Entries describe
the system as it was on the day they were written.

The journal was kept in Polish until the system switched to English, and
those early entries were translated later.

## How it works

**The 32-bit kernel.** GRUB or QEMU finds the Multiboot header in the
first 8 KiB of the image (`boot/boot.S`), loads the kernel at 1 MiB and
jumps to `_start` already in protected mode. From there `kmain()` sets
up the GDT, IDT, PIC and PIT in turn, builds the physical memory map from
the loader's tag, turns on paging, and finally starts the scheduler and
the shell.

**The 64-bit kernel.** Multiboot 2 leaves the processor in protected
mode, so the switch to long mode has to be done by hand and in a strict
order (`boot64/boot64.S`). The page tables come first, because paging
is **mandatory** in long mode. Then PAE, then the LME bit in EFER, then
paging itself. Only the far jump to a segment with the L bit really
switches the processor. The processor itself reports whether it worked:
the LMA bit in EFER is set by the CPU, not by us.

**Booting through UEFI.** `make iso64` assembles a **hybrid** disc. It
has a boot sector for BIOS and a FAT partition with `BOOTX64.EFI`, so the
same disc starts on old and new machines alike. Under UEFI there is no
text memory at `0xB8000`. All the firmware gives us is a framebuffer, so
`fbcon.c` draws the characters itself, pixel by pixel, in its own font.
Later the project got its own UEFI loader, threadboot, which needs no
GRUB at all.

**Programs.** They live in the initrd as ELF files, and the kernel knows
nothing about them beyond what it reads from the header: where to start,
what to load and with which permissions. The permissions come straight
from the segments. Code is read-only, data and the stack are marked
non-executable, and the processor enforces it, not the kernel.

## Development journal

The list is chronological. It records **why** things look the way they
do, including the bugs that turned up on the way, because those explain
the shape of the code better than the code itself. Everything below
works and is checked by `make test`.

- [x] GDT (Global Descriptor Table): the kernel's own segment descriptors
- [x] IDT and CPU exception handling
- [x] PIC (remapped) and hardware interrupts (IRQ)
- [x] Keyboard driver (PS/2)
- [x] Timer (PIT) and an uptime counter
- [x] Serial port (COM1) and printk
- [x] Reading the memory map from Multiboot
- [x] Physical memory management (a frame allocator on a bitmap)
- [x] Paging (identity-mapped 64 MiB; page faults reported with CR2)
- [x] Kernel memory allocator (kmalloc/kfree)
- [x] A simple shell in the kernel (help, ps, mem, uptime, clear, crash, reboot)
- [x] Kernel threads and a scheduler (round-robin, preemption from the
      timer, sleep/yield/exit, zombie cleanup)
- [x] User mode (ring 3): TSS, `int 0x80` system calls, memory
      protection (a process that reaches for kernel memory dies, and the
      system carries on)
- [x] Separate address spaces (a page directory per process, CR3 switched
      by the scheduler, every frame freed when a process ends)
- [x] Loading ELF programs from the initrd (`ls`, `run hello`, `run evil`)
- [x] System calls read (keyboard, blocking), spawn, wait, getpid
- [x] The shell as a user program (`run sh`: ring 3, takes over the
      console, starts programs with spawn+wait, `exit` returns to the kernel)
- [x] Program arguments (argc/argv): `run echo proton os works`
- [x] An sbrk system call and malloc in user space (dynamic memory): `run memtest`
- [x] ATA disk driver (PIO) and the PFS filesystem: files survive a restart
- [x] Unix-style file descriptors: open/close/read/write/seek, fd 0/1/2
      are keyboard/console, higher fds are files (one `write` for
      everything); a `cat` program in ring 3
- [x] Output redirection in the shell (`echo text > file`): the kernel
      swaps the process's stdout for a file handle before entering ring 3
- [x] Pipes between processes (`echo something | wc`): two processes in
      parallel, a buffer in the kernel; the consumer blocks on an empty
      pipe and gets EOF when the producer ends
- [x] Unix tools that compose through pipes: `seq`, `grep`, `head`, `wc`,
      `cat` (e.g. `seq 9 | grep 7 > file`)
- [x] Multi-stage pipelines (`seq 20 | grep 1 | wc`, up to 5 stages)
- [x] File management from ring 3: `ls` (sys_readdir), `rm` (sys_unlink)
- [x] A process's exit status returned by `wait` (shown by `ush`; `grep`
      returns 1 when nothing matched, a process killed by an exception
      returns -1)
- [x] Real-time clock (RTC/CMOS) and a `date` program
- [x] Appending with `>>`, and the `cp` and `mv` programs
- [x] Directories in PFS: paths (absolute and relative, `.`/`..`), a
      working directory per process (inherited), `mkdir`/`cd`/`pwd`/`ls`
- [x] Background processes (`cmd &`) and `kill <pid>` (delivered on the
      way back to ring 3, from a system call or a timer interrupt)
- [x] PS/2 mouse driver (IRQ12): position and buttons (the `mouse` command)
- [x] VGA graphics mode 13h (320x200x256): pixels, rectangles, the
      palette (the `gfx` command, tested through a QMP screendump)
- [x] A mouse cursor on the graphical desktop (the compositor as a
      kernel thread)
- [x] Text rendering with an 8x8 bitmap font
- [x] Windows with title bars, dragged with the mouse (the compositor): `desktop`
- [x] Clickable controls: a close button [x], an action button with a counter
- [x] Graphical applications in ring 3: a process creates its own window
      and draws through system calls (`win_create`/`win_rect`/`win_text`);
      the compositor composes its buffer and removes the window when the
      process dies (`run guiapp`)
- [x] Events delivered to applications: a click in a window reaches the
      process through an event queue (`win_poll`), and the application
      reacts
- [x] Keyboard events to the focused window (a click focuses; a click on
      the desktop removes the focus and the keys go back to the kernel
      shell)
- [x] A taskbar with a `Menu` button and an application launch menu (a
      click starts the program through exec_spawn): `guiapp`, `gclock`
- [x] A list of open windows on the taskbar: the focused window is
      highlighted, a click raises a window (Z order independent of the slot)
- [x] Higher VESA resolution (640x480x32 through DISPI and PCI), a
      double-buffered desktop (no flicker), with a fallback to 320x200
- [x] An `mmap` system call: anonymous process memory (fresh, zeroed),
      freed with the address space; `run mmaptest`
- [x] A GUI file manager (`gfiles`): a list of files on PFS, and clicking
      a name opens the file and shows its contents (FS -> click -> read
      -> window)
- [x] Memory shared between processes (`sys_shm` by key): two processes
      see the same memory (`shmtest` and `shmread`)
- [x] Desktop icons: clickable tiles that start applications
- [x] A resource monitor (`gmon`): RAM and uptime live, with a memory
      usage bar (the `sys_sysinfo` system call)
- [x] Threads in a process (`sys_thread`): several tasks share one address
      space, which is freed after the last thread (`run threads`)
- [x] A scheduler with priorities (`sys_setprio`): a higher priority
      preempts a lower one, equal ones take turns (`run prio`)
- [x] A terminal in a GUI window (`gterm`): type a command and its output
      appears in the window (keyboard -> spawn with redirection -> read)
- [x] Resizing and minimising windows: the `[_]` button minimises a
      window (it stays on the taskbar, a click restores it), and a handle
      in the bottom-right corner of the kernel's windows resizes them by
      dragging
- [x] Icon positions saved on disk: desktop icons can be dragged with the
      mouse, and their layout is saved to a PFS file (`icons.pos`) and
      comes back after a restart (the compositor reads it at start)
- [x] RTL8139 network card (PCI and DMA: an RX ring, 4 TX descriptors,
      the card's interrupt) and a network stack: Ethernet, ARP with an
      address table, IPv4 with its checksum, ICMP. `ifconfig`, `arp`,
      `ping 10.0.2.2` (the system also answers other machines' ARP
      requests and pings to itself)
- [x] Validating pointers from ring 3: every address given to a system
      call must lie in user space and be mapped in the process's page
      directory (`user_ok`/`user_str`), and strings and structures are
      copied into kernel buffers. A bad pointer is an error -1, not a
      write into kernel memory on somebody else's behalf (`run evil2`)
- [x] UDP: sockets in the kernel (ports, datagram queues, a checksum with
      the pseudo-header) and a DHCP client. The IP address, netmask,
      gateway and DNS server come from the network instead of being
      hard-coded (`dhcp`). `kill` also accepts a process name, and the
      shell prompt is redrawn after asynchronous kernel messages
- [x] UDP sockets in ring 3 (`udp_open`/`udp_send`/`udp_recv`/`udp_close`,
      `netinfo`): a socket belongs to its process and is released when
      the process dies, data goes through checked pointers, and the
      loopback lets two processes talk over UDP without a network
      (`udpecho` + `udptest`). A DNS client in user space
      (`nslookup example.com`), and `kill` can interrupt a wait inside the
      kernel
- [x] TCP: active and passive open (client and server), sequence numbers
      with acknowledgements, retransmissions from a `tcpd` thread, closing
      with FIN, RST for a segment to a closed port. System calls
      `tcp_connect`/`listen`/`accept`/`send`/`recv`/`close`, an HTTP
      client (`httpget`: content from the world lands on PFS) and a
      server in ring 3 (`tcpserv`), with `netstat` to look at them
- [x] A browser in a desktop window (`gweb`): an address bar typed on the
      keyboard, the page fetched over TCP, HTML tags stripped, the text
      wrapped in the window and the content saved to PFS. Network, GUI,
      keyboard and disk in one ring 3 application
- [x] Booting straight to the graphical desktop; the kernel command line
      (Multiboot `-append`) option `nodesktop` keeps the text console
- [x] PFS3: blocks allocated from a bitmap instead of fixed areas. 64
      inodes, names up to 63 characters, files up to 268 KiB (direct
      blocks plus an indirect one), blocks freed on `rm`/truncate, `df`
      and the disk size from ATA IDENTIFY. Metadata writes are limited to
      the changed block of the inode table and the bitmap
- [x] TCP: the window is updated after reading from a socket, so
      receiving a large answer no longer waits for the sender's probe
      (100 KiB in a few seconds)
- [x] Names instead of addresses: a shared DNS client (`udns.h`) in
      `nslookup` and the browser, and a server set by hand with `dns`
- [x] An HTTP/1.1 client (`uhttp.h`): `chunked` encoding reassembled
      across packet boundaries, redirects (absolute and relative),
      parsing `http://host[:port]/path`. In the browser, HTML entities
      (`&amp;`) and UTF-8 bytes are turned into readable characters
- [x] Cryptography for TLS: SHA-256, HMAC-SHA256, HKDF (including
      `HKDF-Expand-Label` from RFC 8446), ChaCha20, Poly1305 and the
      ChaCha20-Poly1305 AEAD, X25519. All in user space and checked
      against the vectors from FIPS 180-4, RFC 4231, RFC 5869, RFC 8439
      and RFC 7748 (`run cryptotest`; one X25519 scalar multiplication
      takes ~20 ms)
- [x] An atomic `printk`: a message from one thread no longer lands in
      the middle of a line printed by another thread or an interrupt
      handler
- [x] A **TLS 1.3** client (RFC 8446) in ring 3: ClientHello with SNI
      and `key_share`, the X25519 shared secret, the key schedule on HKDF,
      the encrypted rest of the handshake, checking the server's
      `Finished`, our own `Finished`, separate application keys and a
      record layer with ChaCha20-Poly1305. `httpget https://...` and the
      browser fetch pages over HTTPS; a handshake with OpenSSL takes
      ~90 ms (the certificate is not checked yet - that is the next step)
- [x] A source of randomness for programs (`urand.h`): the RTC, the timer
      count, the pid and stack addresses mixed through SHA-256
- [x] Certificate verification: big numbers in Montgomery form, RSA
      signatures (PKCS#1 v1.5 and PSS), an X.509/DER parser, checking the
      chain up to a trusted issuer, the validity dates and the host name
      (SubjectAltName with wildcards). The trust store is read from the
      file `roots.der` on PFS (`run certtest`)
- [x] The process stack grew from 4 KiB to 32 KiB: RSA at 2048 bits needs
      several kilobytes for its working numbers alone
- [x] ECDSA on the NIST P-256 curve: point arithmetic in Jacobian
      coordinates on the same big numbers as RSA, verifying signatures in
      certificates and in `CertificateVerify` (~80 ms per signature).
      Servers with an EC key are no longer out of reach
- [x] A built-in trust store: 14 public CA roots in the system image (the
      `mkcaroots.py` script refreshes them from the host's bundle), plus
      roots of one's own from `roots.der`. Verification is required by
      default for `https://` (`httpget -k` skips it)
- [x] Extended keys (arrows, PgUp/PgDn, Home/End) in the keyboard driver,
      delivered to programs as control codes of their own
- [x] Browser: scrolling the page, `<a href>` links highlighted and
      clickable (relative and absolute addresses), text split at block
      tags, the address bar refreshed after following a link
- [x] PFS3: a double-indirect block. The file size limit rises from
      268 KiB to 64 MiB (the test downloads 400 KB straight to disk and
      reads it back through a pipe)
- [x] ECDSA on the P-384 curve and SHA-384/SHA-512: the curve arithmetic
      is parameterised (one set of formulas for P-256 and P-384), and the
      curve constants and the SHA-512 tables are generated rather than
      copied by hand. `CertificateVerify` accepts `ecdsa_secp384r1_sha384`
- [x] Hardened parsers: `uhtml.h` (HTML to text) is split out of the
      browser and bounds-checked, and `make fuzz` feeds broken
      certificates, HTML, HTTP answers, DER signatures and **network
      frames that reach the kernel** through sanitizers (net.c and tcp.c
      compiled on the host with hardware stubs)
- [x] The kernel shell as an ordinary thread: the keyboard interrupt only
      puts a character into a ring buffer and wakes the thread, so a
      command can sleep, wait for the network or read the disk, and
      characters typed in the meantime are not lost (`ping` and `dhcp`
      no longer need workaround threads)
- [x] `fork()` with copy-on-write: the child returns from the system call
      where the parent did and sees its memory, but nothing is copied.
      Both address spaces get the same frames without write permission,
      each frame counts its owners, and a private copy of a page is made
      only on the first write. The WP bit in CR0 extends this to the
      kernel's own writes (a system call filling a process's buffer). The
      child inherits descriptors and pipe ends (`run forktest`: a
      256 KiB buffer, RAM grows by 12 KiB after `fork`)
- [x] `exec()` in place (the equivalent of `execv`): a process replaces
      its own program and keeps its pid, descriptors and working
      directory. Together with `fork()` this is the classic Unix pair
      (fork, then jump into the new program in the child), and `ps` shows
      the new name
- [x] The process tree: every task knows its parent (`ps` shows a PARENT
      column), `waitany` waits for **any** child of one's own and returns
      them in the order they end (pid and exit code), and orphaned
      processes are adopted by the grandparent, so their exit codes are
      not lost with the parent (`run ptree`)
- [x] Signals: `signal()` with a handler of one's own in ring 3,
      `SIG_IGN` and default actions, `kill(pid, signal)`, `SIGCHLD` to
      the parent when a child dies, an uncatchable `SIGKILL`. The kernel
      delivers a signal on the way back to ring 3: it puts a frame with
      the interrupted state on the process's stack and jumps into the
      handler, and a trampoline from `usys.h` asks through `sigreturn`
      for that state to be restored. Only the general registers, `eip`,
      `esp` and the arithmetic flags come back from the frame, never the
      selectors or IOPL. **Ctrl+C** on the keyboard goes as `SIGINT` to
      the process holding the console and interrupts its wait for a key;
      `kill P S` in the kernel shell sends any signal, also by process
      name (`run sigtest`)
- [x] All detected memory under management: the identity mapping no
      longer ends at 64 MiB but reaches the top of RAM (up to 512 MiB).
      The first 64 MiB are described by static tables, because the kernel
      has to see memory before it asks the PMM for the first frame, and
      later tables come from the PMM, so the limit is set by the amount
      of RAM and not by the size of a table in `.bss`. User space moved
      from `0x08048000` to **1 GiB** to get out of the mapping's way: one
      process can now take 80 MiB, more than the whole system could see
      before
- [x] Demand paging: `sbrk` and `mmap` no longer allocate a single frame.
      The area is promised to the process, and pages appear only when
      they are first touched (asking for 64 MiB costs 0 KiB, writing
      1 MiB costs exactly 1024 KiB). Pointer validation in system calls
      can bring such a page into existence, so a freshly allocated,
      untouched buffer does not look like a bad pointer to the kernel
      (`run lazymem`)
- [x] An old bug in recognising kernel page tables fixed: they were
      recognised by the **whole value** of the page directory entry, and
      the processor itself sets the "accessed" bit in it. After a while
      the same table had different values in different directories, and
      cleaning up a process could free a **kernel** table, handing a
      piece of the kernel's own image to the free frame pool. Now the
      USER bit decides, which kernel tables do not have, and the PMM has
      an extra fuse that refuses to free a frame of the kernel image
- [x] Swapping pages to disk: when frames run out, the kernel writes
      idle pages to disk (the second-chance algorithm, using the
      "accessed" bit the processor sets) and brings them back on first
      touch. Space is reserved through the filesystem (`swap.sys`,
      visible in `fls` and `df`), but the pages themselves go there
      directly through ATA, bypassing the PFS code: swapping can start in
      the middle of a file operation, and PFS has a single shared block
      buffer. The entry of a swapped-out page keeps the slot number in its
      upper bits, `mem` shows the balance, and `run swaptest 18` fits
      18 MiB on a machine with 16 MiB of RAM
- [x] Virtual consoles (Alt+F1..F4): each has its own 80x25 screen, its
      own shell, its own key buffer and its own keyboard owner. Which
      console output goes to follows from the **task** that writes it
      (like a Unix controlling terminal). A program started on the second
      console keeps writing there while we look at the first one, and
      `Ctrl+C` reaches the process holding the console that is visible
- [x] An **AHCI (SATA)** driver and a block device layer. The controller
      is found by its PCI class (0x01/0x06/0x01), commands are built in
      memory (a command list, a command table with a FIS and a PRDT), and
      data moves by **DMA** instead of being copied word by word. The
      filesystem and the swapper no longer know which disk is underneath:
      the choice is made once at boot, and ATA PIO stays as the fallback.
      This is what it takes to see the disk on a newer board without a
      compatibility mode. Incidentally, the same 18 MiB swap test on a
      16 MiB machine now takes 4.3 s instead of 8.3 s
- [x] A **USB keyboard** (HID, boot protocol) on a UHCI controller:
      enumerating the device on the bus (port reset, descriptors,
      assigning an address, finding the HID interface and its endpoint),
      switching to the boot protocol and translating the eight-byte
      reports into keys, with modifiers, arrows, Ctrl+C and Alt+F1..F4.
      To the rest of the system it makes no difference which keyboard is
      attached: characters travel the same path as from PS/2. Also tested
      with **PS/2 support disabled**, when the system could only be used
      over USB
- [x] The PCI scan covers every function of a device, not just function
      zero. Controllers built into the bridge sit on function 2 or 3, and
      looking only at function zero never found them
- [x] **ACPI**: powering off and restarting the computer. The RSDP is
      found in memory below one megabyte, it leads to the table list
      (RSDT), and that to the power table (FADT). The value that has to
      be written to turn the machine off sits deeper still, in AML
      bytecode (the DSDT) under the name `_S5_`. There is no full
      interpreter; this one object is read directly. The commands are
      `poweroff` and `reboot` (restart through the ACPI reset register,
      or a pulse through the keyboard controller when the firmware does
      not provide one). The tables usually lie outside the memory the
      kernel manages, so each one is added to the identity mapping
      before it is read
- [x] **Multiboot 2**: the kernel image now carries both headers at once.
      QEMU with `-kernel` understands only the older one, and GRUB
      prefers the newer one; the loader picks the one it knows and says
      in `magic` which it used. Information from the newer format
      (command line, modules, memory map) is copied into the same
      structure the rest of the kernel has used from the start, so
      neither the PMM nor the initrd sees a difference. The new thing
      that really matters: **the loader supplies the pointer to the ACPI
      tables**. Under UEFI there is no BIOS memory to search for it. This
      is the first step towards booting through UEFI
- [x] **A console on the framebuffer**: the Multiboot 2 header asks the
      loader for a graphics mode, and the kernel draws the characters
      itself, in the same 8x8 font as the rest of the graphics and in the
      text-mode colours converted to RGB. The character buffer at
      `0xB8000` is no longer needed, which is a precondition for working
      under UEFI, where there is no VGA text mode at all. The four
      virtual consoles work unchanged; only where a finished cell goes is
      different. The desktop uses the same buffer: `video_init_vesa`
      reaches for the Bochs/QEMU interface, which exists only on a
      virtual machine, so when the loader has set a mode we keep it (and
      get 1024x768 instead of 640x480)
- [x] **The first step on x86-64**: a second kernel image (`make
      kernel64`) enters long mode. GRUB leaves us in 32-bit protected
      mode, so `boot64/boot64.S` builds the page tables itself (the first
      4 GiB in 2 MiB pages, not for memory but because device registers
      and the framebuffer live high up), enables PAE, sets the LME bit in
      EFER, enables paging and does a far jump to a segment with the L
      bit. The processor confirms the result itself: it sets the LMA bit
      in EFER, not us. The most important thing is what this image does
      **not** contain: any copy of code. Printing, the framebuffer
      console, the Multiboot 2 tags and the string functions are the same
      files compiled a second time, and the whole difference between the
      modes fits in `io.h` and a few casts through `uintptr_t`
- [x] Interrupts and exceptions in long mode: an IDT of its own (an entry
      is now 16 bytes, because the handler address is 64-bit and does not
      fit in one piece), stubs that save sixteen registers by hand
      (`pushal` no longer exists in long mode), and return through
      `iretq`. `struct regs` now has a version per mode, so the PIC and
      PIT drivers compile in both kernels without a single change. The
      proof: an `int3` trap returns without stopping the system, and the
      timer ticks on its own (1 -> 201 ticks)
- [x] A bug in the USB driver fixed: when there was no data, the read
      descriptor was **abandoned**, and the controller could complete the
      transfer after that decision. The report landed in a buffer nobody
      read, the device flipped its data toggle, and from then on it never
      matched ours again. It showed up the way such bugs usually do: it
      worked, and worked, and then the keyboard went silent. Now the
      descriptor stays armed all the time, and the controller retries it
      every frame by itself
- [x] Memory in long mode: the PMM and the kernel heap moved over
      **without a single change of logic** (the frame bitmap does not
      depend on the mode; a few casts through `uintptr_t` were enough),
      and paging was rewritten: four levels instead of two, 64-bit
      entries, tables built from frames taken from the PMM instead of
      being fixed in the kernel image. A bit that 32-bit paging did not
      have was added: **no-execute**. The framebuffer and data memory are
      marked so that the processor refuses to execute anything in them.
      The check: a 4 KiB page placed **eight gigabytes** above the
      identity-mapped area, written and read through the full four-level
      path
- [x] Kernel threads in long mode: the context switch saves six
      registers instead of four, eight bytes each, and the rest of a
      thread's state stays on its own stack, just as in the 32-bit
      kernel. A separate **idle thread** was added: with it, the case
      "nobody is ready" simply does not happen, and the scheduler does
      not have to wait for an interrupt in the middle of its own call
      (which, when called from the timer handler, would nest forever).
      The check is arithmetic: over 600 ms, a thread sleeping 20 ms at a
      time wakes exactly 30 times, and one sleeping 50 ms wakes 12 times
- [x] **Ring 3 and the `syscall` instruction** in long mode: the port of
      the kernel closes the circle. A GDT with user segments (their order
      is not arbitrary: `sysret` builds the selectors from one value in
      the STAR register, so data must come **before** code) and a TSS,
      which in long mode has only one job left: to say which stack to
      switch to on an interrupt from ring 3. The way into the kernel is
      not an interrupt gate but `syscall`: the processor takes the
      address from a model-specific register and leaves the return
      address in RCX. The price is that `syscall` **does not switch the
      stack**. One has to be provided in advance, and twice, because an
      interrupt takes it from somewhere else (the TSS) than a system call
      does. The proof: the program tells the kernel its code selector,
      which only the processor knows - `cs=23`, ring 3
- [x] **Booting through UEFI**: the 64-bit kernel starts on a machine
      without a BIOS. `make iso64` builds a **hybrid** disc: a boot sector
      for BIOS on one side, and on the other a FAT partition with
      `BOOTX64.EFI`, which the firmware looks for. The same disc boots on
      old and new machines alike. Two things had to be fixed on the way.
      First, GRUB under UEFI does not set a graphics mode by itself and
      fails with "no suitable video mode found" until it is told to load
      the video drivers (`insmod all_video`) and keep the mode set by the
      firmware (`set gfxpayload=keep`); and the Multiboot 2 tag has to ask
      for "any" resolution (0x0x0), because the firmware gives what it
      has anyway. Second, it turned out that OVMF provides a **24-bit**
      framebuffer, and the console could only draw 32 bits per pixel and
      silently refused. The difference is small but fundamental: at 24
      bits a pixel has no padding byte, so writing a whole word tramples
      the neighbour; it has to be written as three bytes. `fbcon` now
      handles both formats, and under UEFI the screen shows exactly the
      same log as under BIOS, drawn in our font pixel by pixel, without
      help from the firmware and without the character memory at 0xB8000,
      which does not exist there at all
- [x] **Programs from a file in long mode**: the 64-bit kernel runs ELF
      files read from the initrd rather than a piece of itself. Until now,
      what ran in ring 3 was a dozen bytes hard-coded in the kernel's
      `.rodata`: proof that the switch to ring 3 worked, and nothing more.
      Now `kernel64/elf64.c` reads the ELF64 header and learns everything
      from the file: where to start, what to load and **with which
      permissions**. The permissions come straight from the segments, so
      a program's code is read-only and its data and stack are
      non-executable, enforced by the processor, not the kernel. A small
      library, `user64/usys64.h`, was added: the way into the kernel is
      the `syscall` instruction (the number in RAX, arguments in
      RDI/RSI, the result in RAX, and RCX and R11 are lost because the
      processor puts the return address and the flags there). The kernel
      does not take a pointer from ring 3 on trust: `paging64_user_range`
      checks the USER bit on **all four** levels of tables, exactly the
      way the processor evaluates an access; otherwise it would be enough
      to pass an address of kernel memory to have the kernel print it.
      The `hello64` program writes, counts, sleeps, asks for the tick
      count and exits with code 7, and all of it passes the same way
      under BIOS and under UEFI
- [x] **Memory protection that really protects**, and survives its own
      proof. An exception coming from ring 3 no longer stops the system:
      the kernel prints what happened, kills the guilty task and carries
      on. Where an exception came from is told by the two lowest bits of
      the code selector in the interrupt frame; the processor puts them
      there, so the program has no way to lie. The handler for such an
      exception does not return: `schedule()` switches to another
      thread, and the interrupt frame disappears together with the dead
      task's stack. Only this made it possible to **test** the
      protection instead of trusting it. `nx64` writes an instruction
      into its array in `.bss` and jumps to its address: the write
      succeeds, and the jump ends in a page fault with `err=0x15` (page
      present, access from ring 3, instruction fetch), so it was the NX
      bit that fired and not a plain missing mapping. `wild64` reaches for
      kernel memory in two ways: first it asks the kernel itself
      (`paging64_user_range` refuses), then it reaches for it directly,
      getting `err=0x05`, a read of a page without the USER bit. After
      both deaths the kernel threads keep counting, and that is the
      assertion that really matters
- [x] A bug in the system call contract fixed: the kernel destroyed RDX,
      RSI, RDI and R8-R10, as a C function may, but the program did not
      know that. The compiler kept a constant in RDX across a whole call
      and later used it as the number of the next call, hitting garbage
      left by the kernel (it showed up as "unknown system call 1016").
      Now the stub saves those registers itself: apart from RAX with the
      result and RCX and R11, which the processor takes, everything comes
      back untouched. It is cheaper to save six registers once than to
      remember six traps in every program
- [x] **Separate process address spaces** in long mode: two programs can
      finally run at once, each with its own memory at the same address.
      The split followed from the address layout: the top level of the
      tables divides the address space into 512 GiB pieces and the second
      level into gigabytes, and all of physical memory, the kernel and the
      framebuffer fit into gigabytes other than the one programs live in.
      So a process gets its own second-level table with one entry
      cleared, and **the rest stays shared**: the same frames as the
      kernel's. A mapping the kernel adds after boot is visible
      immediately in every address space, and on a task switch CR3 is
      reloaded only when it really changes (every reload flushes the
      whole TLB). A task now also carries its own kernel stack: `syscall`
      and an interrupt from ring 3 take it from two different places, so
      both have to be set on every switch. A dying task first moves to
      the kernel's address space and only then gives its own back; it is
      hard to free tables one is standing on. The proof: `sep64`, running
      as two copies at once, writes to the same address `40002060`, each
      its own number, and after handing the CPU to the other copy **both
      find theirs untouched**. After they die, the count of used frames
      goes back exactly to where it was
- [x] On the way: `iretq` to ring 3 now clears every register apart from
      the ones the program is meant to get. The kernel used to leave
      addresses of its own structures in them, and ring 3 has no reason
      to even know them. The program gets one number from the kernel in
      RDI, which is all of "argv" for now, but it is enough for two copies
      of the same file to tell which one is which
- [x] **`fork()` in long mode**: a second process now comes from the
      program's own decision, not the kernel's. To make that possible,
      the system call stub now pushes **exactly the frame an interrupt
      pushes**. The `syscall` instruction pushes nothing by itself (it
      leaves the return address in RCX and the flags in R11), so those
      five words are added by hand. It costs a dozen instructions and a
      return through `iretq` instead of `sysretq`, and gives one thing:
      the kernel sees a system call and an interrupt the same way. `fork`
      then comes down to copying the frame and telling the new task to
      return to ring 3 with it, with zero in RAX. No registers are
      restored "by hand": the same path every other call returns through
      does it. For now memory is copied outright, page by page
      (copy-on-write will come once plain duplication works). The proof:
      `fork64` stores 100, forks, the child stores 200, and the parent,
      when it wakes up, **still has its 100**
- [x] A bug that was waiting for a second process fixed: a program's
      stack pointer was kept in one global variable for the **whole**
      system call. It was enough for the call to put the task to sleep
      and for a second process to enter the kernel in the meantime, and
      the first one returned to ring 3 on somebody else's stack. It did
      not blow up earlier only because both processes had their stacks
      at the same virtual address, so the overwritten value happened to
      be the same. Now the value goes on the kernel stack of that one
      task, and nobody else sees it
- [x] **Copy-on-write** in long mode: `fork` no longer copies memory. The
      parent and the child get the same frames, and a page that was
      writable loses write permission in **both** and gets a mark in bit
      9 (which the processor leaves to the operating system). On the
      first write the processor raises a page fault, and only then does
      the kernel make a copy, of that one page only. Read-only pages
      (code, constant data) do not even need that: since nobody will
      change them, they stay shared for good, and a reference count in the
      PMM decides when they go back to the pool. The write-protect bit in
      CR0 (WP) was added: without it the kernel can write to pages marked
      read-only, so a shared page would be silently overwritten instead
      of split. The numbers from a real run: `fork64` has seven pages
      (code, constant data, data, four stack pages), so copying outright
      would cost seven frames plus tables. **With shared pages, fork takes
      five frames, all of them for tables, and two pages get split**: the
      one the child wrote and one stack page. The semantics did not change
      one bit: the parent still has its 100 when it wakes up
- [x] **`exec` and `wait`**: the process model comes full circle. A
      program can start **another** program and wait for it to end, which
      is exactly what every shell in the world does. `exec` needed no
      separate return path: we are inside a system call handler, and it
      returns to ring 3 through the frame it was given, so it is enough to
      **rewrite** that frame (a new start address, a new stack, cleared
      registers) and let it return. The program on the other side has no
      way to notice that it came back somewhere other than where it
      called from. The order here is the only possible one: the name has
      to be copied into the kernel **before** the memory it lives in
      disappears, and the old address space can only be freed once the
      processor has left it. `wait` relies on a trace left by a finished
      task: the task itself leaves the list at the next switch, but the
      parent does not have to manage to ask before the child ends, so one
      entry stays behind to be collected. When there are no children at
      all, `wait` says so instead of waiting forever. The proof: `spawn64`
      forks, the child replaces its image with `hello64`, and the parent
      collects its **seven**. That seven is the proof that it waited for
      that child and not for any finished task
- [x] **Keyboard and shell in long mode**: one can finally talk to the
      system instead of watching a script written into `kmain64`. I moved
      the scancode translation out of the driver into `kernel/kbdmap.c`.
      It is the one part of the driver that depends on nothing (not on
      virtual consoles, not on tasks, not on the processor mode), so the
      same state machine handles the keyboard in both kernels, and the
      decision about **where** a finished character goes stays with the
      driver. The state has to live in it, because one key press is split
      into several bytes: the `0xE0` prefix, separate press and release
      codes for modifiers. `kernel64/shell64.c` is an ordinary kernel
      thread with a loop, with as many commands as the kernel can really
      do: `help`, `ps`, `mem`, `ls`, `uptime`, `clear` and `run
      <program>`. `run` **waits** for its child through `task64_wait` and
      reports its exit code, exactly as a program would through a system
      call; the shell does not respond in the meantime, as it should not.
      On the way, starting programs moved from `kmain64` into
      `proc64_spawn`, where it belongs: the kernel at boot and the shell
      on request now take exactly the same path
- [x] **Signals and Ctrl+C** in long mode: a program in an endless loop
      is no longer impossible to stop. A signal is not a message to be
      read but an interruption of the program's flow, and the kernel
      cannot deliver it at the moment it is sent: the target is somewhere
      else then, maybe asleep, maybe in the middle of a system call. So
      it waits with the task until the **next return to ring 3**, because
      only then does the frame describe the program and not the kernel.
      That is checked in two places: on return from a system call and on
      return from an interrupt; the second works even when the program
      asks the kernel for nothing. The delivery itself is again an
      operation on the frame: save it on the program's stack, point it at
      the handler and put a return address under it. The address has to
      come **from the program**, because the only executable pages a
      process has are its own code; the stack is non-executable by design
      (NX). The frame comes back from the user stack, so the program could
      tamper with it: the registers and the return address are its own
      business, but the segments and flags are not, because that is the
      way into the kernel. The proof: `spin64` without a handler dies on
      Ctrl+C and the shell gets the console back, while `sigtest64`
      **catches** the signal: its handler prints and returns, and the
      program goes on to finish normally with code zero
- [x] **Input for programs and a shell in user space.** `read` blocks
      until the first character and then hands over everything that has
      accumulated, like every sensible input: one waits for anything, not
      for a full buffer. A signal interrupts that wait and returns an
      error, because otherwise Ctrl+C would have no way to reach a
      program that is waiting for a key, and that is exactly the program
      that needs Ctrl+C most often. `user64/ush64.c` is an ordinary ring 3
      program with no privileges beyond those of any other process: it
      reads the keyboard through a system call, starts programs with
      `fork` and `exec`, and waits for them with `wait`. Everything it
      does, any other program could do, and that is the point. The shell
      in the kernel was needed so that there was anything at all to drive
      the system with, and this one is the proof that **it no longer has
      to live there**. A failed `exec` ends only the child (code 127), and
      the parent lives on, as everywhere
- [x] A **hang found by the user** fixed: `run ush64`, in it `spin64`,
      Ctrl+C, and nothing. The program kept spinning and the console did
      not come back. Two causes, both on my side. First, a signal sent to
      a task waiting in `task64_wait` **was never delivered**: signals are
      delivered only on the way back to ring 3, and that loop never goes
      back there, so the signal waited for delivery for as long as the
      waiting lasted, which, when the child does not end on its own,
      means forever. `read` checked for a pending signal and returned an
      error; `wait` did not. Second, Ctrl+C hit the shell and not the
      program: the interrupt's target pointed at `ush64`, the one that is
      **waiting**, so even a working signal would have killed the shell
      and left the program alive. `task64_foreground` was added: it walks
      down the chain of children to whatever is actually running at the
      end. The walk is finite by construction (a child gets a higher
      number than its parent), but there is a depth limit anyway: a loop
      looking for the end of a loop is the last place one wants to rely on
      reasoning. `ush64` also has its own SIGINT handler now, so a stray
      Ctrl+C interrupts the line being typed instead of ending the
      session, like every shell. An earlier audit of the loops **missed
      this spot**: in `task64_wait` I checked the paths of the child's
      exit record, not the signal path, and that was exactly the hole
- [x] **The disk in long mode**: PCI, ATA PIO, AHCI and the block layer
      are the same files as in the 32-bit kernel, compiled a second time.
      The drivers know nothing about the processor mode: they talk to the
      device through ports and memory, and both look the same on either
      side. `pci.c`, `ata.c` and `blk.c` went over **without a single
      change**; `ahci.c` needed casts through `uintptr_t` (DMA addresses
      stay 32-bit, because the frames are below 512 MiB anyway) and one
      condition on the mode: the controller's registers live high up,
      outside the area mapped at boot, so they have to be added to the
      mapping, or the first read from the BAR ends in a page fault.
      `paging64_map_phys` was added: it maps a physical range writable and
      non-executable, because there is no code in device registers. Both
      paths are tested: `disk` writes the last sector, reads it back and
      counts the differences: zero, through ATA PIO and through AHCI with
      DMA alike
- [x] **The filesystem in long mode**, and the first moment anything in
      this kernel survives switching the machine off. `fs.c` went over
      **without changing a single character**: the disk seen through the
      block layer looks the same whatever the processor mode, and PFS
      touches neither paging nor tasks. The shell got `fls`, `cat <file>`
      and `write <file> <text>`; three commands are enough to check that a
      file really is on the disk and not only in memory: write it, print
      it, see it in the list. The fourth proof comes by itself, on the
      next boot, and it is the only conclusive one: the test boots the
      machine **a second time on the same disk** and checks that the file
      is there with the same contents and that the filesystem was not
      formatted anew
- [x] **File descriptors for programs**: it is no longer only the kernel
      that reaches the disk. `open`, `close`, `read` and `write` work in
      ring 3, and a descriptor is an ordinary number in **a table that
      belongs to the task**. One process's number means nothing in
      another, and that is the whole difference between a descriptor and
      an inode, which is shared by the whole system. Every descriptor
      carries its own position in the file, because two processes may
      read the same file at different speeds. The first numbers have
      always been taken and refer to no file: zero is the keyboard, one is
      the console. Thanks to that, printing to the screen is **an ordinary
      write to descriptor one**, exactly like a write to a file, so `read`
      and `write` take a descriptor as their first argument, as
      everywhere. A child of `fork` inherits open files together with
      their positions, just as it inherits memory. `fileio64` checks
      itself: it creates a file, writes, closes, opens it again, reads and
      compares byte by byte; if anything did not match it ends with an
      error rather than a message someone has to read. A file written by
      a program survives a restart just like one written by the kernel:
      the descriptor path ends on the same disk
- [x] **Program arguments (`argv`)**: tools no longer have to live inside
      the shell. Until now `echo` and `cat` were built in, because there
      was no way to pass a program a single word. The layout on the stack
      is the same as everywhere: first the strings themselves, above them
      an array of pointers ending in zero, and the program gets their
      count and the address of the array in registers. There is nothing
      magic about it; it is the program's ordinary memory, which the
      kernel happened to fill before its first instruction. The arguments
      are copied into the kernel **before** the memory they live in
      disappears, just like the program name, and it is the same trap as
      with `exec`. The strings are copied page by page, because a string
      may end just before the boundary of what the program has mapped. The
      limits are modest on purpose: this is meant to be `argv`, not a
      channel for transferring data. `uecho64` prints what it got, and
      `ucat64` takes a file name from its argument, opens it with a
      descriptor and reads it; both work the same from the kernel's shell
      and from the user shell. `sep64` also stopped getting its number
      written into a register: it arrives as `argv[1]`, the way every
      program gets it
- [x] **A program heap (`sbrk`) with memory on demand**: until now every
      buffer had to be fixed in the program. The kernel does not allocate
      a single page here: it only moves the boundary and remembers how far
      the program asked for it, and pages appear on first touch, in the
      page fault handler, writable and non-executable, because there is no
      code in a program's data. The difference is bigger than it looks:
      **`heap64` asks for 4 MiB, i.e. 1024 pages, and the system gives it
      four**, exactly the ones the program touched. Shrinking gives pages
      back immediately; otherwise memory would go back to the pool only
      with the whole process. A request below one's own heap or into the
      stack is refused
- [x] On the way, an x86-64 difference that could not show up earlier:
      **SSE2 is baseline there**, so GCC vectorises loops without asking.
      The first loop copying 4 KiB got `movdqa ... %xmm9` and ended in an
      "invalid opcode" exception, because the kernel had not yet set
      `CR4.OSFXSR`. For now programs are built with `-mno-sse`, like the
      kernel; eventually SSE has to be enabled and the FPU state saved on
      task switches. A second thing came up as well: the rule that builds
      programs did not depend on the `Makefile`, so changing the compiler
      flags **rebuilt nothing**, exactly the same trap as once with the
      kernel headers
- [x] **SSE and the floating-point state**: programs are built normally
      again, without `-mno-sse`. On x86-64, SSE is not an extra but part
      of the calling convention: the compiler uses it without asking,
      because every processor in this mode has it. The processor,
      however, starts with support disabled, and every such instruction
      ends in an exception until the system says explicitly that it can
      handle those registers. "Can" means one thing here: **it can save
      and restore them on task switches**. The XMM registers are task
      state just like RAX or RSP, except that there are many of them and
      the processor saves them with one instruction into a 512-byte area.
      The 16-byte alignment is not a courtesy: `fxsave` and `fxrstor`
      simply refuse to work on a misaligned address. The kernel itself
      stays without SSE: there is nothing to compute in an interrupt
      handler, and every such instruction would require saving the
      interrupted program's state. The proof: two copies of `fpu64test`
      compute at the same time, each with a different starting number,
      giving up the processor along the way, and **each gets its own
      result**, not its neighbour's. The operation is chosen to be exact
      to the last unit (`v = v*2 + 1` on integers that fit in double
      precision), so the test does not have to approximate anything
- [x] **Directories and paths** in long mode: the filesystem is no longer
      flat. `mkdir`, `cd`, `pwd`, `rm` and `readdir` work from ring 3, and
      **the working directory belongs to the task**, just like the
      descriptors: two processes can stand in different places of the
      tree without getting in each other's way. A child starts where its
      parent stood, so `uls64` started from the shell lists the directory
      the shell is in. `open` also looks at the working directory and not
      always at the root. A bug worth remembering on the way: `fs_readdir`
      returns **1 on a hit and 0 at the end** of the directory, not a
      negative value. Checking for "less than zero" turned the end of the
      directory into success, and the listing looped forever on the stale
      contents of the buffer. I made the same mistake in two places at
      once, because I assumed the convention instead of reading it
- [x] **Pipes**: programs can be chained. A pipe is not a file and has
      nothing to do with the disk: it is a ring in kernel memory plus
      **two owner counters**, and those matter more than the buffer
      itself, because they say when to stop. When the last writer
      disappears, the reader gets end of data instead of waiting forever.
      `dup2` was added, which is how input and output are redirected, and
      where a write goes is now decided by **the kind of descriptor, not
      its number**: descriptor one without redirection is the console,
      but after `dup2` it can be the end of a pipe, and the program does
      not see the difference. `uupper64` reads from zero and writes to
      one; that a second program sits on the other side rather than a
      keyboard and a screen is the business of whoever started it. This
      is the easiest place to hang, so two things are done explicitly:
      the shell closes **both** ends of the pipe on its side right after
      handing them to the children (otherwise one writer, the shell
      itself, would stay alive forever and the reader would never see the
      end), and the waits in `pipe64_read`/`pipe64_write` check for a
      pending signal, because nobody will deliver it for them inside a
      loop in the kernel. A task also closes all its descriptors when it
      ends
- [x] **Networking in long mode**: the RTL8139 card, the IP layer, DHCP
      and TCP, again the same files as in the 32-bit kernel. **Three out
      of four went over without a single change**; the card driver only
      needed an explicit cast of addresses, because the card addresses
      memory with 32 bits and a pointer is eight bytes here (the buffers
      are in the kernel image, just above one megabyte, so the address
      fits with room to spare, but the cast has to be visible). They did
      not compile for another reason: the network code talks to the
      kernel through a few functions (put me to sleep, should I die, is
      that task still alive, make a new thread), and the scheduler has
      different names here. Instead of scattering conditions on the mode
      across them, `kernel64/compat64.c` was added: **those names are a
      contract, and what stands behind them is the kernel's business**.
      That one file is the whole difference. The proof from a real run:
      DHCP assigns an address, `ifconfig` shows the netmask, gateway and
      DNS, `ping` to the gateway comes back 3/3, and the ARP table knows
      its hardware address, so frames really go out and come back
- [x] **The network from a program's side**: until now only the kernel
      used the network, since `ping` and `ifconfig` are its own commands.
      Now a ring 3 program asks for its address, opens a UDP socket, sends
      a datagram and connects over TCP, without any special privileges. A
      socket belongs to the task that opened it: the kernel keeps its
      number and cleans up after its death, so a program cannot grab
      someone else's socket by passing its number. A lesson about the ABI
      on the way: the first version packed the socket number and the
      timeout into one register, because three argument registers are not
      enough for a socket, an address, a port, a length and a timeout at
      once. It read terribly and broke at the first change. Now everything
      apart from the data itself goes in one descriptor passed by pointer,
      and the kernel writes the sender's address into the same descriptor
      on receive. `unet64` checks three things in turn and grades itself:
      an address from DHCP, a datagram sent **to itself** (the loopback
      does not go out on the wire, so the attempt tests the stack and not
      the network around it), and a TCP connection to an HTTP server on
      the host. On QEMU's user network the gateway `10.0.2.2` **is** the
      host, so no port forwarding is needed
- [x] **Graphics mode in the 64-bit kernel**: `video.c` went over like
      the rest, but it revealed something that could not show up in the
      32-bit kernel. When it accepted the loader's framebuffer it assumed
      **32 bits per pixel**. That worked as long as the buffer came from
      BIOS; UEFI firmware can give 24, and then writing a whole word
      tramples the neighbouring pixel. Now the depth is whatever the loader
      really gave, exactly the same fix as in the console earlier. The
      shell got `gfx`: four rectangles in the corners, a frame and text in
      our own font. The picture is simple on purpose, because any mistake
      in computing a pixel's address shows at once: a wrong bytes-per-pixel
      skews the image diagonally, and swapped channels show in the
      colours. The test checks it under BIOS **and** under UEFI, because
      the firmware decides how many bits a pixel has. A mistake of my own
      worth remembering: the first pattern used the **text-mode** colour
      numbers, while the `video.c` palette defines indices 0-5 of its own.
      The code drew correctly; it was the pattern that had the wrong
      colours
- [x] **The graphical desktop in the 64-bit kernel**, and the entry where
      the whole approach to this port paid off most. `desktop.c` (736
      lines: compositor, windows, title bars, icons, taskbar, menu,
      dragging) and `mouse.c` went over **without a single change**. The
      whole difference came down to three functions added to the
      compatibility layer: `exec_spawn` by name, the desktop taking over
      the keyboard, and giving it back. The rest (tasks, sleeping, the
      filesystem for saving icon positions) was already there from the
      network port. I was afraid of this step and it turned out the
      cheapest so far, not because the desktop is simple, but because it
      depends on the processor mode in nothing but the names it uses to
      call the kernel. What is still missing: graphical applications in
      ring 3. The `App`, `Clock` and `Files` icons call programs that are
      not in the 64-bit initrd, so clicking them does nothing. That needs
      window system calls and shared memory, which is a separate step
- [x] **Graphical applications in ring 3 (64-bit kernel)**: the desktop
      icons are no longer dead. Four system calls (`win_create`,
      `win_rect`, `win_text`, `win_poll`) and a `guiapp64` program in the
      initrd: it draws in its own window, gets clicks and keys back, counts
      them and answers with a drawing. The compositor did not get a single
      new line; `desktop_win_*` has existed since the 32-bit days, and only
      the path to it from ring 3 was added. The program did not even need
      another name: `mkinitrd` puts it into the image as `guiapp`, the same
      name the shared icon table calls. Protection is where it was: a
      window is owned by a process, not by whoever passes its number, so
      passing someone else's number achieves nothing. The kernel copies
      strings and drawing commands into its own memory, checking byte by
      byte whether the program may reach there; a string cannot be checked
      in bulk, because it may end at the last byte of a mapping.

      And this revealed a bug that had been asleep in the kernel since the
      first program: the first `movaps` in `guiapp64` ended in a general
      protection fault. Not because of SSE (the kernel has saved the SSE
      registers for a long time) but because of **stack alignment**.
      `_start` is written in C, and the compiler treats it like any other
      function: it assumes the stack is as found after a `call`
      instruction, with the return address on top. The kernel gave a top
      aligned to sixteen bytes, eight too many, and every sixteen-byte
      store of an SSE register hit an unaligned address. The earlier
      programs survived only because the compiler had nothing to
      vectorise in them. Interestingly, signal delivery computed the stack
      **correctly** from the start (its comment says outright "as every
      function called with a call instruction finds it"), and process
      start did not. One line in `build_args`, the same rule in both
      places
- [x] **ACPI, virtual consoles, the USB keyboard and swapping in the
      64-bit kernel**: four things the 32-bit kernel has had for a long
      time came over one after another. Three of them nearly for free; the
      fourth revealed a bug the 32-bit version could not show.

      **ACPI** is `kernel/acpi.c` without changes to its logic: the way to
      power off (RSDP, the table list, the FADT, the `_S5_` object in AML
      bytecode) does not depend on the processor mode. Only adding the
      tables to the mapping does, so the file got the same macro pattern
      as `initrd.c` and `video.c`, plus explicit casts of addresses to
      pointers. `poweroff` turns the machine off and `reboot` resets it,
      and the latter cannot be checked any other way than by QEMU (with
      `-no-reboot`) simply disappearing.

      **The virtual consoles** were already in `kernel/vga.c`, shared from
      the start; the 64-bit kernel simply had only one, because `task_vc()`
      returned zero. So a task now owns a console (inherited from the
      parent, like the working directory), each console has its own key
      queue, and Alt+F1..F4 is handled in the interrupt. On the way, the
      shell stopped keeping the command line in a global variable: four
      shells are four threads, and the line belongs to the thread and
      lives on its stack.

      **The USB keyboard** needed the same in `uhci.c` as the network card
      before: the controller addresses memory with 32 bits, a pointer is
      eight bytes here, so every structure address goes to it through an
      explicit cast. `usbkbd.c` went over unchanged.

      **Swapping** is the only item with real new work on the long-mode
      side. The store (`kernel/swap.c`) is shared, but the eviction policy
      had to be written anew: walking four levels of tables instead of
      two, the second-chance algorithm on the "accessed" bit, and an entry
      for a page that lives on disk, without the present bit, so the
      processor ignores it, and with the slot number in the upper bits.

      And here a bug worth remembering came up. The first version of the
      on-disk entry carried only the slot number, and on the way back the
      page got "writable, non-executable", because most pages are like
      that. The program died right after starting with exception 14,
      error code `0x15`. I first read that as **fifteen decimal** and
      looked for a reserved-bit violation that was not there; in hex it is
      a present page, an access from ring 3 and an **instruction fetch**.
      A *code* page had been swapped out and came back non-executable. So
      the access rights now travel to disk with the page, and that is the
      right principle, not a patch: a code page is read-only and
      executable, a data page the exact opposite, and the kernel has no
      business guessing which is which when it handed them out itself.

      On the way, a program's address space grew from eight megabytes to
      half a gigabyte: swapping only makes sense when a program can ask for
      more memory than the machine has RAM. The test takes 18 MiB on a
      machine with 16 MiB and checks two things: that every page came back
      from disk without a single byte changed, and that swapping **really
      happened**, because without the second assertion a test in which
      everything fitted in memory would pass too.

      Five new test groups: `gui64`, `acpi64`, `consoles64`, `usbkbd64`,
      `swap64`. All the 64-bit ones now read the screen the way a person
      sees it: GRUB, seeing a framebuffer request in the kernel header,
      always gives graphics, so the character memory at `0xB8000` is empty
      and the console draws itself in the 8x8 font. The test recognises
      those glyphs with the same table the kernel uses: one source of
      truth, and reading what is really visible.
- [x] **Programs from disk, not only from the initrd**: the ELF loader and
      the filesystem had sat side by side for a long time and were never
      connected. `run` looked only in the initrd, the image loaded together
      with the kernel and read-only. Now a program can live on the disk:
      `cp hello64 anything`, then `run anything`.

      The whole connection is one file, `kernel/prog.c`, and there is not a
      word about the processor mode in it: it only decides where to get the
      bytes from, and the loader already knows what to do with them. That
      is why **both kernels** use it: the 64-bit one through the
      compatibility layer (`task_cwd`), the 32-bit one directly. The order
      is fixed: the initrd first, then the disk. The initrd is always there
      and always works, so a broken file on the disk has no way to shadow a
      program shipped with the kernel, which means it has no way to take
      the system away.

      A program is looked for in the **task's** working directory, just
      like with `cat` or `ls`: the directory belongs to the process, so
      `run` means the same as it would after a `cd`.

      And the same mistake again as with `fs_readdir`, even though I had
      already described it here once. `fs_write_at` returns **the number of
      bytes written**, not zero, and I checked `== 0`; the same for
      `fs_read_at`. The first `cp` "failed" despite a correct write. The
      lesson is probably just one: a return convention must not be guessed
      from the similarity to a neighbouring function; it has to be read.
      Now one condition checks that **as much as intended** was written,
      because a file cut off halfway is not the same program.

      The test uses the strongest proof there is here: a second boot of
      the same machine with the same disk image. A program that still runs
      after a restart and is not in the initrd can only come from the
      platter. The path from ring 3 is checked too: `ush64` does `fork` and
      `exec`, and `exec` looks for the program the same way as the kernel.

      On the way, I gave the tests `q.command(...)`: it types a command
      and returns **only what arrived after it**. Without that, a false
      alarm in the other direction is easy: the shell echoes the typed
      line, so the file name is on the serial port even when the command
      found nothing, and the result of the previous command stays there
      until the end of the run. Three assertions in this test passed for
      that reason before the feature worked at all.
- [x] **Shared memory and two more graphical applications (64-bit
      kernel)**: `kernel/shm.c` went over without changes to its logic,
      because a region is simply a contiguous block of frames from the
      PMM. What is new is the path to it from ring 3: an area for mappings
      was set aside between the heap and the stack (from 0x50000000
      upwards), and the heap ends at its threshold, so one cannot run into
      the other. Swapping skips shared frames, as in the 32-bit kernel:
      with them one would have to count owners on disk as well.

      The test shows what this is really about, not only that the bytes
      match: the writer first maps a region on the side, under a different
      key, so it **sees the shared one at a different address than the
      reader**. The same bytes at two different addresses, because where
      something is visible is decided by the address space, not by memory.
      Without that the test would also pass on an accidental match of the
      layout.

      On the way: the CMOS clock (`rtc.c`, pure I/O ports, went over as it
      was), a `date` command and a `SYS64_TIME` system call.

      Two new applications in windows: `gclock64` (the time from the
      hardware and the uptime from the kernel's ticks, two independent
      time sources side by side) and `gfiles64` (a list of files on the
      disk; clicking a name loads its contents). The `Clock` and `Files`
      icons are no longer dead; three are left: Monitor, Term and Web.

      And the same mistake again as with `fs_write_at` and `fs_readdir`,
      **the third time in this project**. `sys_readdir` in the programs'
      library returns ZERO on a hit and -1 at the end of the directory,
      while `fs_readdir` in the kernel does the opposite: one on a hit. I
      wrote `!= 1`, so the first entry looked like the end of the directory
      and the window showed "(empty)". Neighbouring names are no hint about
      a return convention; it has to be read, every time.
- [x] **A resource monitor and a terminal in a window (64-bit kernel)**:
      the fifth and sixth graphical applications, and with them the
      `SYS64_SYSINFO` system call: how much memory there is, how much is
      used, how long the machine has been running. None of that is a
      secret (the numbers describe the machine, not other processes), so a
      ring 3 program gets them without any privileges.

      `gterm64` is the most interesting one here, because there is not a
      single thing in it the kernel could not do before, and yet it uses
      everything at once: the keyboard through window events, `fork`,
      `exec`, redirecting output to a file with `dup2`, waiting for the
      child, reading the file and drawing. The terminal is an ordinary
      program and knows nothing about the compositor beyond four window
      calls, and still a shell in a window comes out of those building
      blocks.

      The compositor holds at most six windows and the kernel takes two,
      so five applications at once do not fit. The test got a second boot,
      and that is the right answer, not raising the limit.

      One dead icon is left: Web. It first needs TLS and an HTTP client in
      ring 3.
- [x] **TLS 1.3, HTTP and the browser in ring 3 (64-bit kernel)**, and
      with that the catching up is over: everything the 32-bit kernel does,
      the long-mode one now does too.

      The most interesting part is how much code had to be written: **one
      header file**. Cryptography, TLS, HTTP, DNS, X.509 and the HTML
      parser (five thousand lines in `user/*.h`) compute on bytes and know
      nothing about the processor mode. They talk to the kernel through a
      few names: connect, send, receive, what time is it, open a file.
      Those names are a contract; `user64/compat64.h` translates them into
      long-mode calls, and `user/uapi.h` picks the side. Exactly the same
      pattern as `kernel64/compat64.c` on the kernel side, and for the
      second time it turned out cheaper than I feared.

      Changes to the shared code itself: one. A stack address used to seed
      the random generator went through `(uint32_t)` from a pointer; in
      long mode that cuts off half the address, so now it goes through
      `uintptr_t`. Nothing else.

      A real bug did turn up, though. `certtest` died halfway: a page fault
      just below the top of the stack. The program got four pages (16 KiB),
      and verifying a signature on two-thousand-bit numbers goes deeper,
      all the more so because pointers are eight bytes here, so every frame
      with an array of pointers is twice as big. The constant could have
      been raised. **Instead the stack now grows on demand**, exactly like
      the heap: a program gets four pages to start with, the rest is added
      as it goes, and the limit (a quarter of a megabyte) is there so that
      a wild pointer does not silently turn into a mapped page; below it a
      page fault stays a fault. Guessing in advance how much stack is
      needed always ends either in wasted memory or in an exception in the
      middle of a deep call.

      The test talks to a real OpenSSL, not to a loop of our own, so a
      handshake that completes means every byte matches: X25519,
      ChaCha20-Poly1305, key derivation and the signature in Finished. It
      also checks something more important than encryption itself: a
      foreign certificate must **break** the connection, because an
      encrypted conversation with a stranger is not secure, only private.

      The last dead desktop icon came to life: `gweb` resolves the name
      through DNS, fetches the page, strips the tags and draws it in the
      window, one program tying together the compositor, keyboard, mouse,
      DNS, TCP, TLS, the filesystem and its own address space.
- [x] **Splitting the `core` group in three**, not for tidiness but for
      time: one group made 174 assertions on one machine and on its own
      decided how long the whole run took, because all the others
      finished much earlier. Now there are three: boot and processes
      (`core`), the network with the filesystem (`core2`) and TLS alone
      (`tls32`). Since they run in parallel, the whole suite takes 259 s
      instead of 310 s. The number of assertions is the same, 518.

      The split also flushed out a flaky check that had been waiting
      there for a long time. Demo thread C ends after two seconds and for
      the next few redraws a message in row zero of the screen, and that
      was the only proof that it ran. Row zero disappears on every scroll,
      so on a loaded machine the proof scrolled away with the boot
      messages before the test got to look. Moving the check earlier was
      not enough: under full load just reaching the prompt takes longer
      than the whole window of visibility.

      So the thread now writes one line **directly to the serial port**,
      not through `printk`, because it ends after the shell has started,
      and output on the console would land in the middle of a line someone
      is typing (the keyboard test caught that at once). The serial port is
      a log and the screen is the work in progress; they are not always the
      same thing, and it is good that this particular difference had to
      show up here.
- [x] **Memory-mapped files (64-bit kernel)**: the second half of the same
      idea as running programs from disk. If a file can be a program, it
      can also be **memory**. A program gets an address and reads the
      contents like an ordinary array, without a single `read` call.

      The most interesting part is the cost. A mapping does not create a
      single page: the kernel only remembers the block numbers, and the
      bytes come from disk as they are touched. Touching one byte of a
      four-hundred-kilobyte file costs **one page**, not a hundred, and the
      test checks that assertion directly, because after every program the
      shell reports how many pages were read from mappings.

      The block numbers are worked out once, when the mapping is set up,
      and then read through the block layer, bypassing the PFS code. The
      reason is the same as for swapping, and it was written down then too:
      PFS has a single shared block buffer, and a page fault can land in
      the middle of any file operation. A principle described once saved a
      whole class of questions here.

      The mapping is read-only, and that is a decision, not a gap: writing
      would require sending pages back to the disk and deciding when. Until
      that exists, a write attempt had better end in a protection fault than
      be silently lost. The test checks this with a child: the child tries
      to write and dies, and the parent survives and reports that the file
      is untouched.
- [x] **The e1000 card and the `netdev` layer**: disks have had a block
      layer for a long time. ATA and AHCI stand under one name, and which
      controller answered is the business of boot. The network had no such
      layer, and it showed in every line that needed the MAC address:
      `rtl8139_mac()` called directly in the IP and DHCP code. Now there is
      `netdev.c`, on exactly the same idea.

      The Intel 82540EM is built completely differently from the RTL8139,
      which made it a good test for that layer. Its registers are in memory,
      not in I/O ports, so they have to be added to the mapping first,
      because they live high up. Traffic goes through two **descriptor
      rings**, one per direction, instead of one ring of bytes: a
      descriptor says where a buffer is and what happened to it, and who
      owns it follows from the head and tail pointers. It is more
      convenient than the RTL8139 in that packets do not run into each
      other and the end of the buffer does not have to be copied to its
      start.

      For ARP, IP, DHCP and TCP nothing changed, and that is the real point
      of this step. The test runs the same set of checks with `-device
      e1000` on both kernels: a DHCP lease, a ping to the gateway, the ARP
      table, HTTP from ring 3. The same path for a packet, a different card
      underneath.

      Receiving goes the other way from the rest: the driver itself calls
      `net_rx` when a frame arrives, and the `netdev` layer does not stand
      in between; that would be passing a pointer along for no reason.
- [x] **An NVMe disk**: the third driver under the same block layer, and
      the least like the other two. There is nothing of the ATA heritage
      in it. No "select the drive", no FIS frames. There are two **queues
      in memory** (we put 64-byte commands into the submission queue, and
      the controller puts 16-byte answers into the completion queue) and
      two doorbell registers, through which each side tells the other how
      far it has got.

      The nicest detail of the whole standard is the **phase bit**: the
      controller flips it on every lap of the queue, so to know whether an
      answer is new nothing has to be cleared or counted. It is enough to
      compare one bit with the one we expect.

      We have two pairs of queues: the admin one, which comes into being
      by itself when the controller starts, and one for data, requested
      with a command on the first. There is one buffer, page-aligned: a PRP
      pointer describes a page, not an arbitrary piece of memory, so
      copying through our own aligned page is cheaper than translating
      every caller's buffer into a list of pages.

      On the way the test fell into a trap that has nothing to do with
      NVMe: it typed commands right after the message about the filesystem
      being ready, but the shell starts only after the scheduler. The first
      characters were lost, and of `write nvmetest text-through-queues`
      the kernel received `through-queues`. The keyboard works from the
      interrupt, but there is someone to read it only once the thread runs.
- [x] **The other processors: detecting and waking them (both kernels)**:
      the first step towards SMP, and deliberately only the first.

      After power-on one core runs, the one the firmware picked as the boot
      processor. The rest stand and wait, and that is the hardware's
      decision: a processor woken with a SIPI starts in real mode, sixteen
      bits, at the address `(vector << 12)`, exactly as a PC started forty
      years ago. How many there are is told by the MADT table in ACPI; they
      are woken with a pair of signals through the local APIC.

      The most interesting part is how much of the way such a core has to
      go **by itself**. In the 32-bit kernel: real mode, its own descriptor
      table, protected mode. In the 64-bit one the whole way again: sixteen
      bits, thirty-two, sixty-four, with PAE and EFER on the way. It does
      not build page tables: it takes the ready ones, the same ones the
      kernel runs on, and gets their address in a field the boot processor
      fills in before sending the signal.

      The trampoline is linked with the kernel but runs at `0x8000`, so
      every address in it is computed as a base plus the offset from the
      start of the block. Addresses go into a register first and only then
      get used; the assembler then has one obvious meaning to choose
      instead of two.

      And that is where this step ends: a woken core reports in a counter
      and halts. Giving it work requires a scheduler per core and locks in
      every kernel structure, and those were written for one core and
      quietly rely on that. Better for a processor to stand still than to
      corrupt the task list. That is why the test checks two things at
      once: that the cores report **and** that the system works exactly the
      same after waking them. "I see four processors" alone would be worth
      nothing if something broke along the way.
- [x] **Fixing `make fuzz`**, and a lesson about why there is a separate
      command at all that `make test` does not run.

      Fuzzing builds the parsers **on the host**, under ASan and UBSan,
      with a stub in place of the entry into the kernel. Porting TLS to
      long mode changed that entry: the headers now include `uapi.h`, not
      `usys.h`, so the stub replaced a name nobody calls any more, and the
      harness stopped building. The `netdev` layer added a second such spot
      on the kernel side.

      Both went unnoticed through a full test run, because `make fuzz` is
      not part of it, and rightly so: it takes its time and checks
      something else. But it means it has to be run separately after every
      change in how the shared code talks to the kernel. I am writing this
      down precisely because it is easy to forget: five hundred cases per
      parser, zero errors, but only when someone runs them.
- [x] **The secondary cores get work**, and the first lock in this system.

      Disabling interrupts was enough while there was one core: nobody else
      could get into the middle of an operation. Now they can, and **truly
      at the same time**, not just apparently. Hence `spinlock.h`: the
      `xchg` instruction is atomic by definition and is also a barrier for
      the ordering of writes, so no extra fences are needed here. What a
      core wrote before releasing the lock, another one will see after
      taking it.

      The secondary cores do not run tasks of their own and will not for a
      long while: the scheduler, the process list and most of the kernel
      were written for one core. Instead they get **countable** work: a
      shared queue of units, from which each takes the next number under
      the lock and computes it on its own. All the state shared between the
      cores is one number and one lock, and that is exactly why it can be
      reasoned about.

      The boot core computes together with the others; without that nobody
      would compute anything on a single-core machine. Interrupts are off
      while the lock is held on it too: if the timer preempted the thread
      holding the lock, every other taker would wait until its next turn.

      The `smp` command hands out 64 units and compares the result with the
      one computed on one core. That is the right question about
      concurrency: not "did something get computed" but "was **every** unit
      computed exactly once". Four cores, sixteen units each, the sum the
      same to the bit.

      A bug on the way worth remembering: the first version of the
      trampoline called a kernel function with a relative jump. The offset
      computed at link time describes the distance from where the code
      **lies in the kernel image**, but it runs at `0x8000`. So the jump
      landed in a void and the machine reset in silence, because a secondary
      core has no exception table yet. The address now goes through a
      register, as in the 64-bit version, where it had to from the start,
      because there a relative jump reaches at most two gigabytes.
- [x] **A stack that grows on demand and memory-mapped files, in the
      32-bit kernel too**: evening out an asymmetry I introduced myself by
      doing both first in long mode. This project rests on both kernels
      being the same files compiled twice; differences the processor mode
      does not force are simply debt.

      The stack: eight pages to start, the rest added as it is touched, a
      limit of a quarter of a megabyte. The test recurses forty levels deep
      with four-kilobyte frames and **after returning** checks that every
      frame is still its own, because adding pages over somebody else's
      frames would look half the same.

      Mapping files flushed out a real bug on the way, in code older than
      this whole change. `fork` marked **every** page of a process as
      copy-on-write, including one that deliberately had no write
      permission. So the child wrote to a mapped file and the write
      *succeeded*: the copy-on-write handler made it a private, writable
      copy. A page without write permission now stays as it is; that is
      its whole property, so marking it for copy-on-write was a
      contradiction in itself.

      The pair `paging_map_user_in` / `paging_map_user_ro_in` was also
      added over a common core: the same mapping, one bit of difference,
      chosen by the caller.
- [x] **Writing through a memory-mapped file**: the way back, in both
      kernels at once.

      Until now the mapping went one way: a file could be read like memory,
      but a write attempt ended in a protection fault. That was honest
      (better an error than a silently lost change) but half done. Now
      `sys_mmap_file_w` gives the pages write permission, and the contents
      go back to the file.

      The most important question is **when**. Writing back after every
      byte would kill the whole point of a mapping; it would be cheaper to
      call `write` then. So the program decides, through `sys_msync`, and
      the kernel adds one moment the program does not have to remember:
      the end of the process. The write-back has to happen **there**, in
      `task_exit` and in `exec`, while the process's address space is still
      alive. The zombie reaper looks at those pages from someone else's
      CR3, and `exec` is about to lose that address space.

      Which pages to write back, the processor knows. It sets the "dirty"
      bit itself on every write, so nothing has to be counted or guessed.
      The test reads the whole twelve-kilobyte file, writes to the first
      two pages, and `sys_msync` writes back **two**. A second one in a row
      writes back zero, because the bit is cleared with the write-back.
      That is an assertion that really decides something; "something was
      written back" would pass just as well if the whole file were
      rewritten every time.

      A third software bit was added to the page entry: `PTE_FILE`
      (`PG_FILE` in long mode), the last free one. It means two things at
      once, both negative:

      - `fork` must **share** such a page, not mark it copy-on-write. Both
        processes write to one file, so a private copy in the child would
        mean its writes never leave its own memory. The test forks, has the
        child write, and checks in the parent.
      - swapping must skip it. The place of such a page on disk is the
        mapped file itself, not the swap file; writing it to `swap.sys`
        would lose both the link to the file and the fact that it is dirty.

      On the way, the 64-bit kernel needed a **fourth system call
      argument**. Three had been enough until now; the fourth goes in R10,
      as in Linux and for the same reason: the `syscall` instruction itself
      takes RCX for the return address.

      A bug older than this change also came up: `exec` in the 32-bit kernel
      reset the counter of free mapping addresses but kept the
      **descriptions** of the old image's mappings. Their ranges lie exactly
      where the new image starts setting up its own, so a page fault in the
      new heap hit the description of a file this process no longer had
      open. `task_mmap_release` had existed from the start and was not
      called from anywhere; the 64-bit version called its own.
- [x] **xHCI, the third USB controller**, and the layer that hides it.

      UHCI was enough until now only because QEMU emulates it. A machine
      newer than roughly 2010 has neither PS/2 nor UHCI, only xHCI. Without
      it, "works on real hardware" would stay an empty item on the list
      forever.

      The difference between these controllers is not in the details but
      in **the division of labour**. In UHCI it was the driver that laid out
      the transfer descriptors in a frame list the controller swept every
      millisecond, and the driver made up the devices' addresses. In xHCI
      one talks through commands placed in a ring, answers come back on a
      second ring of events, and the controller assigns the addresses.

      The ring is the nicest thing in this driver. It is an array of TRBs
      whose last element is a jump back to the start, and which entries
      already belong to the controller is decided by one bit, the cycle
      bit. The jump has a flag that flips the meaning of that bit on every
      lap. Thanks to that the ring never has to be cleared, and nobody has
      to count how much is left in it: the same bit means "mine" one time
      and "yours" the next.

      Adding the driver REVEALED a boundary that had not been visible
      before. `usbkbd.c` called `uhci_*` directly, not because recognising
      a keyboard has anything to do with the controller, but simply because
      there was only one controller. The new `usbhc.c` puts the boundary
      where the difference ends: resetting the port, assigning the address
      and finding the packet size of endpoint zero look completely
      different in the two controllers, so they sit under one word,
      `usbhc_attach`. Above it is only the conversation with the device,
      the same whatever the bus, and that is why the whole HID code, the
      key tables and the polling thread did not change by a line. It is the
      same pattern as `blk.c` and `netdev.c`, and for the third time it
      turns out it can only be put in place once there really are two
      things to hide.

      Writing PORTSC needs care about something UHCI did not have: some
      bits are **cleared by writing a one**, and writing a one to the port
      enable bit DISABLES the port. A plain read-modify-write would disable
      the port while setting anything else. Hence `portsc_safe`.

      The test is twofold on purpose. It checks that a keyboard on xHCI
      talks at all, but above all that it talks **the same way**: the same
      set of checks as with UHCI, the same character, the same Shift, the
      same Alt+F2. Separate groups `usbkbd64` and `xhci` watch two buses
      and one conclusion: above the layer one cannot see which is
      underneath.
- [x] **A shared library and binding symbols at load time**: no more copy
      of the cryptography in every program.

      `httpget` weighed 82 kilobytes, `gweb` 89, `certtest` 51. Nearly all
      of it was the same cryptography and the same TLS, compiled in yet
      again, once in the initrd and once in memory, separately for each
      process. Now those three plus `cryptotest` together weigh less than
      the one library they share.

      **Where the library's source came from.** Cryptography lives in
      ProtonOS in headers, as `static inline`. That was a good decision and
      I did not want to undo it: one source of truth, no intermediate
      files. A library, however, needs two different views of the same
      file: with bodies (to compile them and make them findable by name)
      and without bodies (so that a program contains calls, not copies).
      Splitting fourteen headers into .c/.h pairs by hand would mean four
      thousand lines rewritten and two places that could drift apart from
      then on. So `scripts/split_headers.py` does it on every build: it
      matches braces, skipping those in strings and comments, and emits
      both versions. The source stays single and untouched.

      **Why the library has a fixed address.** We link it at the address
      where the kernel then places it in every process. Thanks to that, the
      addresses in its symbol table are already final, and neither it nor
      the kernel has anything to recompute at load time. There are two
      costs and both are acceptable: there can be only one library, and it
      is always visible at the same address. A real system does not do
      this, because it wants many libraries and wants to randomise their
      addresses, but a real system has a linker in user space for that,
      and here a few dozen lines in the kernel play the role of `ld.so`.

      **What exactly the kernel does.** It reads the dynamic symbol table
      from the library (function name -> address) and the list of
      relocations from the program (the address of an empty slot in its
      jump table -> function name). Putting one together with the other is
      the whole binding. Everything is bound at once, at start: lazy
      binding would require a piece of the linker to live in the process's
      address space and be called from a PLT stub, a whole separate
      mechanism to save microseconds at start.

      **Split into shared and private.** The segment without write
      permission (code and constants) gets SHARED FRAMES: every process
      sees the same physical pages, and that is the whole saving. The data
      segment has to be private, because the library keeps its state in
      it: the random generator's seed, TLS settings. One process's state
      must never be visible in another. The tail of zeros (`.bss`) is
      created on touch, like the heap: a program that takes only SHA-256
      from the library has no reason to pay for TLS buffers.

      **Two things that had to come out of the headers.** The elliptic
      curves and the built-in trust store were variables in them. A program
      that reached for them by name would get its own copy (a copy
      relocation), and from then on the library would compute on one
      instance while the program looked at another. Now they are exposed
      through functions, and the kernel refuses to load a program in which
      such a relocation appears. Better loudly at start than quietly in the
      middle of a TLS handshake.

      **A trap that shows only in the numbers.** The first version reported
      "0 shared pages", and that would have bothered nobody, because
      everything worked. The linker had put `.dynamic` into the same segment
      as the code, and `.dynamic` is writable, so the whole segment became
      writable and the kernel rightly gave every process its own copy.
      There was no sharing for a single moment. Hence the page alignment in
      the linker script, and hence that number in the boot message: without
      it the mistake had no way of coming to light.

      **A second trap, which showed up only in the test.** The library's
      data first got its pages on touch: the zero tail (`.bss`) came into
      being only on first touch, like the heap. TLS worked in the 32-bit
      kernel and not in the 64-bit one. The reason: checking pointers from
      ring 3 looks different in the two kernels. The 32-bit kernel BRINGS
      INTO EXISTENCE a page the program asks about; the 64-bit one only
      checks whether it is there and refuses when it is not. A buffer that
      only the kernel writes to (like the one for received TCP bytes) is
      never touched by anybody, and the system call rejects it. The whole
      data segment is now set up at once; thirty-odd frames per process is
      the price of one mechanism fewer and of both kernels doing the same
      thing.

      The test does not ask "does it work" but about things that cannot be
      faked. The address of the running code lies OUTSIDE the program's
      image, and the library itself must report it, because a non-relocatable
      program that takes the address of one of its functions gets the
      address of its own jump stub. A second process sees the same code
      address (shared frames) but its generator counter starts from zero
      again (private data). A write to the library's code ends in a
      protection fault; otherwise one program could replace a function for
      everyone at once.
- [x] **Locks on the kernel's shared structures**: the first step without
      which real SMP cannot begin.

      Until now the secondary cores could only compute on what they were
      given. All the state shared between them was one number and one lock,
      and that is exactly why it could be reasoned about. For them ever to
      run tasks, they first have to be able to **call the kernel** safely,
      and the kernel has to withstand that.

      Disabling interrupts, which was enough from the start, protects
      against something other than a lock does, and it is worth naming
      directly. A lock protects against ANOTHER CORE that really runs at
      the same time. Disabling interrupts protects against ITSELF: a timer
      entering the middle of a section would wait for a lock held by the
      code it just interrupted, on the same core, the only one that could
      release it. Hence `spin_lock_irq`, which does both things in one
      order: interrupts go off BEFORE the lock is taken and come back AFTER
      it is released.

      Locks went onto three structures everything else reaches for: the
      physical frame map, the kernel heap and the console. Each is one for
      the whole machine, so its lock is one too.

      **A trial that really decides something.** The `smpmem` command makes
      every core allocate frames and heap blocks three thousand times at
      once. We do not ask whether it crashed, though; the answer to that
      can be a matter of luck. Every allocation gets a pattern that depends
      on the unit number and is read right after: a frame handed to two
      cores at once has two writers, and the pattern does not match. At the
      end we count frames and heap bytes: the same before and after.

      I checked whether this trial can fail at all by taking the locks out
      again. Without the lock on the frame map: sixty-one pattern mismatches
      and fifty-eight frames that never came back. Without the lock on the
      heap: the kernel dies on a page fault at address `aaa9a8ab`, because
      the free list falls apart in the middle of splitting a block. That is
      the only way to know that a test is guarding anything.

      **What a secondary core still may not do.** It has no exception or
      interrupt table, so it may only call what does not sleep and does not
      wait for hardware. Allocating memory, yes. The disk, the network,
      anything that might reach for swapping, no. That is where the
      boundary is today, and only a per-core scheduler will move it.

      **An assertion that was a race.** The first version of the test
      demanded that EVERY core take part of the work. It passed, but on the
      64-bit kernel the shares came out `[1, 20, 21, 22]`: the boot core is
      running the shell thread at that time and gets preempted by the
      timer, while the secondary ones spin in a tight loop and take the job
      apart before it gets a chance. The assertion now says what is true
      whoever wins the race: more than one core took work, and every unit
      was computed **exactly once**.
- [x] **A secondary core gets an exception table and its own clock**: the
      second step towards real SMP.

      A woken core was in an odd position until now: it ran kernel code but
      had neither the kernel's segment table (it sat on the temporary one
      from the trampoline) nor any exception table. Any fault of its own
      ended in a triple fault and **a silent reset of the whole machine**,
      without a single word on the screen. Both tables are one for the
      whole system and nobody changes them after they are built, so it was
      enough to point at them. A secondary core does NOT load the task
      state segment (TSS): `ltr` marks the descriptor as busy, so the same
      table cannot be loaded twice, and there is no need anyway, since
      ring 3 programs run only on the boot core for now.

      **Its own clock.** The PIT interrupt goes through the PIC to ONE
      processor; a secondary core would never see it, and without
      interrupts it can never be preempted. Every processor does, however,
      have its own timer in the local APIC. Its frequency cannot be read, so
      it has to be measured: the boot core starts the APIC counter and sees
      how far it counts during a known number of PIT ticks. Only it
      measures, because only it has something to measure with.

      **The bug that cost the most time and was worth it.** In long mode a
      secondary core died the first time it touched the local APIC,
      silently, together with the whole machine. The reason: the trampoline
      set the long-mode enable bit in EFER but not the NXE bit. And the
      kernel maps everything that is not meant to be code with the
      no-execute bit, including the hardware registers added to the
      identity mapping. Without NXE, bit 63 of a page table entry does not
      mean "no execute"; it is **reserved**, and any access through such an
      entry is a page fault. The same address, the same tables, the same
      CR3, and one processor reads while the other dies. The difference was
      one bit in a register nobody copied.

      Finding it took going down to the simplest possible output: one byte
      straight to the serial port, no locks, no functions, nothing that
      could fail by itself. Letter by letter one could see how far the core
      got.

      **An idle core should stand still, not spin.** Until now a core with
      no work spun on `pause`. On a virtual machine that costs exactly as
      much as work: three cores spinning aimlessly took three quarters of
      the machine from the boot core, so an ordinary disk write took four
      times longer. **Booting to the shell on four cores: just over 22
      seconds before the change, 6.5 seconds after.** Now the core halts on
      `hlt`, and an inter-processor interrupt sent when work is posted wakes
      it, the simplest interrupt one can send anybody, since all it carries
      is the fact that it arrived.

      The order matters and is written out in the code: the wake-up goes
      AFTER the job is stored. Sent before, it would find the queue still
      empty.

      **And a race in an assertion again.** Since the cores sleep, how the
      work is divided depends on how fast the wake-up reaches them, and that
      is the emulator's business, not the kernel's. The assertion "every
      core took a share" stopped meaning anything, so it now says what is
      true regardless of the race: more than one core took work, and every
      unit was computed exactly once. The comparison with the single-core
      result, to the bit, guards that.
- [x] **Kernel threads on every core**: what the three previous steps were
      waiting for.

      A secondary core already had an exception table, its own clock and
      locks on the shared structures. One thing was missing, and the most
      inconspicuous one: **the current task was a single global variable**.
      As long as it was, more than one core had nowhere to record what it
      was running. Now there are as many as there are cores, and which one
      is mine is told by the processor itself: the number in its local
      APIC.

      The word `current` stayed, because the whole kernel uses that word
      and there is no reason for it to stop. Only what it points at
      changed: from one variable to a field of the core's block. One
      hundred and thirty-nine places in each of the two kernels did not
      have to know.

      **A lock held across the switch.** This is the part that cannot be
      done halfway. There is one task list, so one lock protects it, but it
      is not enough to take it while choosing the next task and release it
      right away. There would be a window in which the previous task is
      already marked ready while its stack is still in use: a second core
      would jump onto a stack the first one is still running on. So the
      lock is held **across** the context switch, and whoever arrives on
      the other side releases it. A new task does not return from a switch
      like all the others, so it releases the lock itself, in its
      trampoline. A task that is ending never returns, and does not have
      to: its successor does it for it.

      For the same reason, the current task's state now changes **under
      the lock**, together with choosing the next one (`switch_to(state)`).
      Set earlier, it would leave a window in which the task is marked as
      sleeping while its stack is still busy, and a second core could wake
      it in the meantime.

      **What a secondary core still may not do, and why that is a decision
      and not a gap.** An ordinary task stays on the boot core. A secondary
      core takes only what somebody explicitly allowed through
      `task_create_anycore`, because the rest of the kernel reaches for the
      drivers, the filesystem and swapping, and those were written for one
      core and quietly rely on it. The task state segment is one and
      belongs to the boot core, so ring 3 stays there too. A caller who asks
      for a task "anywhere" takes on the promise that its thread touches
      only things that can stand two cores at once.

      Every core gets its own idle task; otherwise there would be nowhere
      to put its context. It runs on the stack the core got when it was
      woken, and nobody else may run it. They show up in `ps` under negative
      numbers: they are not processes.

      **The proof has three parts, because each one alone could be faked.**
      The thread records which core it is computing on, but if the scheduler
      merely moved it between cores in turn, the mask would look the same.
      The result of every chunk is the same to the bit as a chunk computed in
      the shell thread, but that only proves correctness. **Time** decides:
      four chunks run in parallel must take clearly less than four times as
      long as one.

      The numbers are nicer than I expected. In long mode one chunk takes 64
      ticks, and four at once **61**: four times the work in the same time.
      The same trial on one core: 28 ticks for one, 185 for four. That is the
      difference that matters, and the test shows it directly.
- [x] **Ring 3 on every core**: user programs are no longer the property of
      the boot core.

      It took four things. First, **a task state segment per core**. The
      processor reads the kernel stack from it on entry from ring 3, and each
      core enters from a different task onto a different stack; one shared
      segment would mean two cores entering the kernel on the same stack.
      There have to be as many descriptors as cores, too: `ltr` marks a
      descriptor as **busy**, so a second core loading the same selector would
      get a protection fault.

      Second, **its own `syscall` entry**. In long mode the entry registers
      (`IA32_LSTAR`, `IA32_STAR`) belong to the core, and one that does not
      set them will not accept a system call at all. The kernel stack itself
      comes through `swapgs` and `IA32_KERNEL_GS_BASE`, the only mechanism the
      processor provides for "this is my core": an array of 16-byte blocks,
      one per core.

      Third, **the big kernel lock**. The rest of the kernel (drivers, the
      filesystem, swapping) was written for one core and quietly relies on
      it. The lock is reentrant, because it has to be: an interrupt lands in
      the middle of a system call, and a core that already holds it cannot
      wait for itself. Every entry from ring 3 takes it and every exit gives
      it back, and `switch_to` gives it up **for the duration of the switch**;
      otherwise a task sleeping in the middle of a syscall would close the
      kernel to everyone until it woke. The depth is kept on that task's
      stack, so each one gets exactly its own back.

      The fourth turned out to be real and cost a day. **A secondary core in
      protected mode did not have paging enabled.** The boot trampoline
      switched to protected mode and stopped there; nobody ever set its CR3
      or the PG bit. For kernel code it came out the same, because low memory
      is mapped one to one, and that is why kernel threads on secondary cores
      worked flawlessly through the whole previous step. But a user program's
      address (0x40000000 and up) means nothing without page tables: the core
      read whatever lies at that address **physically**, which was zeros.

      The symptom was misleadingly far from the cause. The process died at a
      random, wild address, different every time, because `iret` entered
      ring 3 at the right address, and the processor executed zeros there and
      jumped wherever. The big lock looked guilty, so I turned it off, and it
      changed nothing, which was the first piece of good news. What decided
      it was a dump in which the same data read two ways gave two different
      answers: through the virtual address **zeros**, and through the same
      frame read directly, the correct code. The page directory matched, the
      entry matched, CR3 matched, and `invlpg` and reloading CR3 changed
      nothing. One possibility was left: address translation was not working
      at all. The fix is six instructions in the trampoline and one field for
      CR3, exactly what the 64-bit core had from the start, because without
      paging long mode simply does not start.

      **A thread and a process are now two different permissions.** A kernel
      task created through `task_create_anycore` is *independent*: it does
      not take the big lock, because whoever asked for it took on the promise
      that its thread touches only things that can stand two cores at once.
      If it did take the lock, four threads computing in parallel would
      compute one after another, which is not what they are for. A process
      started with `runw` is the opposite: its kernel side runs under the
      lock, and only the computing in ring 3 runs in parallel, which is where
      a program spends most of its time anyway.

      **The proof is again a comparison, not a measurement.** `run cores` and
      `runw cores` in the same machine: the same program, the same checksum to
      the bit, the same number of steps. All that differs is how many cores
      may run it. In long mode 86 ticks against **33** and the mask of all
      four cores; in protected mode 42 against **26**. On a single-core
      machine `runw` changes nothing, and an assertion guards that too: the
      promise "may run anywhere" is not the promise "must run elsewhere".
- [x] **System calls without the big lock**: the first bite of what the
      previous step put in the way.

      It had to start with admitting a mistake. While cleaning up the
      diagnostic code after the previous step I reverted `kernel/isr.c` to
      the version in the repository, together with the wrapper that took the
      big lock on entry from ring 3. So the 32-bit kernel went into the
      repository with ring 3 on every core and **no lock at all**, although
      the description said otherwise. All the tests passed, because nothing
      in them pressed on that spot hard enough, and that is exactly the kind
      of green that means nothing. The lock went back in this step.

      **First, a measurement that showed something else entirely.** The
      `syscalls` program calls the kernel a million times and does nothing
      else. Under the big lock, two processes on four cores finished in 1080
      ticks, and the same two on one core in 594. Four cores were **twice as
      slow** as one. But when I took the lock off that call, the improvement
      was negligible, so the lock was not the biggest cost here at all.

      `smp_core()` was. The whole kernel uses the word `current`, and that
      word means "the task of THIS core", so every use of it asked the
      processor for the core number, and the question went **through a read
      of the local APIC**. That is a hardware register outside the cache:
      hundreds of cycles on a real machine, and more under an emulator,
      because hardware access takes its own lock. The kernel paid that
      several times per system call.

      The number was there all along, and for free. Every core has **its own
      task state segment** (it got one in the previous step), and its selector
      sits in the processor's task register. The `str` instruction reads it
      without a single memory access. Before a core loads its segment, `str`
      gives zero, and that is the right answer, because until then only the
      boot core runs. Setting up the segments itself still asks the APIC;
      otherwise it would be asking for the number in order to set it.

      The effect: a million calls on one core went from 594 ticks to 286 in
      protected mode and from 288 to 159 in long mode. Twice as cheap, in
      every system call and every interrupt.

      **Only then the lock.** The list of calls that do not need the big lock
      is short and explicit: the process number, the clock counter, the core
      number, sleeping. Each of them touches only the calling task's state or
      things that have locks of their own. The default answer is "with the
      lock" and should stay so; adding a call to that list is a promise that
      the call has been read to the end, together with everything it calls.

      Delivering a kill and a signal stays under the lock, because one ends
      the process and the other rewrites its stack, but the lock is taken only
      when there really is something to deliver. Without that, this path would
      differ from the ordinary one only in name.

      **The numbers.** A million calls in each of two processes: in protected
      mode 287 ticks one after the other against **192** at once, in long mode
      147 against **62**. Before the whole change, this was slower on four
      cores than on one.

      The test watches more than time, though. Each of the million calls has
      to return the number of THIS process, and that is exactly the mistake
      the lock had been hiding until now: two cores confused about which task
      they are running. Two million matching answers mean more than any
      timing.
- [x] **English names, tests split up**: a change of convention, not of
      behaviour.

      Identifiers, file names and program names are now in English; what the
      system says to a person stays Polish: kernel messages, assertion
      descriptions in the tests and this journal. Code is written in English
      because that is how it is read.

      I did the renaming with a syntax-aware tool: for C a scanner of my own
      that skips strings and comments, for Python the tokenizer from the
      standard library. I compared the result mechanically against a plain
      renaming of the repository version: only the files I had fixed by hand
      were supposed to differ, and only they did.

      There were four manual fixes, all for one reason: a new name collided
      with an existing one. One of them changed the MEANING of the code, and
      neither the compiler nor any test reported it. In the NVMe driver the
      DMA buffer at file scope got the name `buf`, which a parameter of
      `nvme_write` already has, and it would have shadowed it so that the
      write went to the caller's buffer instead of the DMA one. So I wrote a
      separate tool that looks for NEW shadowing in the whole tree: apart from
      one spot in ush64, no file gained any.

      On the Python side there was one more trap. The tokenizer returns a
      whole f-string as one token, so names inside the braces were left
      untouched and `run_tests.py` fell over on an old name. A second pass
      replaces them separately, only inside braces.

      **Tests split into files of one thing each.** Three groups were sacks:
      `core` (174 assertions), `core2` and `k64` (795 lines). Each of them did
      everything on one machine, so a failure pointed at a group covering a
      third of the system rather than at the broken thing. They became
      separate groups: boot, shell, processes, fork, demand paging, signals,
      protection, network, HTTP, mouse, ush, filesystem, and on the 64-bit
      side the long-mode core, processes, shell and disk. The drivers split
      into ACPI, USB keyboard, AHCI, consoles and swapping as well.

      The price is fair and shows in the numbers: there are 49 groups instead
      of 32, and the whole run takes 361 seconds instead of 326, because every
      group has to start its own machine. In exchange the longest group went
      from 174 to 87 seconds, and a failing assertion now says what broke.
- [x] **Comments in English**: closing the previous step.

      The names went over to English earlier, the comments stayed Polish, and
      for a while a file looked as if two people had written it. Now Polish is
      left only in what the system says to a person: kernel messages,
      assertion descriptions in the tests and this journal.

      All of it went through: kernel64/, user/, user64/, both .S files in
      kernel/ that the previous pass missed, and the comments and docstrings
      in scripts/. I did it with a tool that cuts the comments out of a file
      and pastes the translations back under the same numbers: for C a scanner
      of my own, for Python the tokenizer from the standard library. Once I
      got the numbering off by one and overwrote a section header in
      t_e1000.py with someone else's text; it showed at once, because one
      Polish comment was left in the file that should not have been there.

      On the way came names the previous pass had missed, because they were
      not identifiers in code but file and program names (the map maker, the
      deep recursion test, the two shared memory programs), plus the mode
      arguments of mmapzap and a handful of identifiers with a Polish stem
      that slipped through the sieve.

      Automatic renaming has its price, and I paid it here too: replacing one
      short Polish word with `nosync` got into Polish runtime strings and
      turned a message about "bytes without msync" into "bytes nosync msync".
      I caught it reading the diff, not by a test. No test will catch damage
      of that kind, because the text of a message is nobody's assertion.

      768 assertions, 49 groups, no change in behaviour.
- [x] **A mapping change reaches the other cores (TLB shootdown)**, and
      threads can finally leave the boot core.

      A page table is memory, and memory is shared. A translation COMPUTED
      from that table is not: every core keeps its own in a cache for which
      the kernel has no instruction. Changing an entry changes what the
      tables say and nothing in what the neighbour believes, and the
      neighbour keeps writing to a frame the kernel has already freed or
      given to someone else.

      As long as a process ran on one core, nobody suffered from this. And
      that is exactly why threads were kept on the boot core: they share an
      address space, so they are the first to run into it.

      The only way into somebody else's cache is to ask its owner: send an
      interrupt and WAIT for the answer. That waiting is the whole thing. A
      request sent and not waited for says nothing about when it will be
      carried out, and "when" is exactly what the caller needs to know: it
      is about to free the frame.

      Only those at fault pay for it. Every core announces which address
      space it is in, so a change in a space nobody else holds sends no
      interrupt at all, which covers every single-threaded process, i.e.
      nearly all of them.

      **The mask cannot be wrong, and that is the whole argument.** We ask
      only the cores that say they are in the given space, and a core can
      change its mind while being asked. Both directions are safe for the
      same reason: a core that LEFT the space after the count invalidates an
      address in a space it no longer runs (one wasted miss, and the switch
      itself flushed its cache anyway). A core that ENTERED after the count
      had to load CR3, which flushes everything; there is no way to enter
      while holding anything older than the moment of entry.

      **The deadlock that stopped it.** The first run hung the machine. A
      core waiting for the big kernel lock does so with interrupts off (it
      came in through an interrupt gate), so it hears nothing. That was
      harmless as long as the only thing to wait for was the lock itself:
      the holder runs and sooner or later lets go. It stopped being harmless
      the moment the holder started waiting for US. The lock's wait loop now
      answers while it waits: one attempt, an answer, another attempt.

      **A second bug, found on the way and older than the whole change.**
      The test program showed 7296 memory mismatches, and they had nothing to
      do with the TLB. The heap limit and the next free mmap address describe
      the ADDRESS SPACE, and threads share one; kept per task, they were
      copied when a thread was created and moved separately, so two threads
      asking for memory at the same moment got THE SAME address, each sure it
      was first. The bug had been a bug since the day threads appeared; it
      was just hard to hit while a process stayed on one core.

      **The numbers.** The `tlb` group and the `tlbtest` program: four threads
      of one process, fresh memory in every round, so fresh mapping changes in
      a space three other cores stand in.

      | trial | core mask | remote | answers | memory errors |
      |---|---|---|---|---|
      | `run tlbtest`, 4 cores | 1 | 0 | 0 | 0 |
      | `runw tlbtest`, 4 cores | 15 | 124 | 203 | 0 |
      | `runw tlbtest`, 1 core | 1 | 0 | 0 | 0 |

      Mask 15 is four cores in ONE address space. The zeros in the first and
      third rows matter as much as the 124 in the second: the mechanism costs
      nothing where there is nobody to tell.

      The 64-bit kernel got the same mechanism and the same counters, but it
      has no threads yet, so two cores cannot be put into one address space
      and the test only checks there that the machinery stands and stays
      quiet when it should.

      796 assertions, 50 groups, 385 s.

- [x] **Threads in the 64-bit kernel**, and only now can one see that the
      TLB shootdown works there too.

      The previous step built mapping-change broadcasts into both kernels,
      but could show them working only in the 32-bit one: long mode had no
      threads, so two cores could not be put into ONE address space. The test
      there only checked that the machinery stands and stays quiet, which is
      no proof.

      A thread is a new task with THE SAME address space and its own stack.
      The whole difficulty lies outside creating it: the address space has
      to survive the death of the first thread (whoever ends last frees it),
      and the heap limit and the next free address have to be one counter
      for everyone, not a copy in each, exactly the same bug that came up on
      the 32-bit side a week earlier. Fixed here at once, before it could
      bite anybody.

      On the way: anonymous memory (SYS64_MMAP), reserved addresses with pages
      made on touch, because that stack has to come from somewhere.

      **Two bugs on the way, both older than this change.**

      First: threads died with "Invalid Opcode" at an address that turned out
      to be an ordinary `movdqa %xmm7`. CR0 and CR4 belong to the CORE, and
      only the boot core enabled SSE, so every program using XMM that landed
      on a secondary core died on a perfectly valid instruction. The bug had
      been there since programs started running on all cores; no test had hit
      it so far, because no 64-bit program started with `runw` used XMM in a
      visible place. This is the category of bug that does not show at all
      until something new is done.

      Second: threads then died with a "General Protection Fault" on
      `movaps %xmm0, 0xe0(%rsp)`. The top of the stack was aligned to sixteen
      bytes, which was exactly eight bytes too well. A C function assumes the
      stack as it is AFTER a call instruction, with the return address on top,
      and bases the alignment of its variables on that. The program stack had
      been set up correctly for a long time; I simply had not copied that
      minus eight.

      **The numbers.** `runw tlbtest64` on four cores: core mask 15, 40 pieces
      of memory checked twice, zero errors, 140 mapping changes that had to
      reach other cores, and 221 answers. `run tlbtest64`: mask 1 and not a
      single interrupt. The same four checks on both sides, and on both sides
      they mean something.

      809 assertions, 50 groups, 492 s.

- [x] **Page faults without the big lock**, and the first subsystem to get
      a lock of its own.

      A page fault is the most common way into the kernel there is. Every
      program touching memory goes through here thousands of times, and most
      of these faults are neither faults nor even work in name: a frame from
      the allocator, four kilobytes of zeros and one table entry. All three
      have their own locks.

      And still every such entry took the big kernel lock, so two processes
      touching THEIR OWN memory, in THEIR OWN address spaces, with nothing in
      common, still went one after the other.

      The list of causes handled without the lock is short and explicit, just
      like the list of system calls: the heap, the stack, anonymous memory in
      the mapping area, and copy-on-write. The rest (a page from the swap
      file, a page of a mapped file and every real fault) comes in as before,
      because it reaches for the disk or the task list.

      **The page tables got a lock of their own.** They were the big lock's
      property for as long as every page fault took it; now that it no longer
      does, they need their own. One for the whole kernel, deliberately: the
      section under it is a walk of two (or four) levels and one write. What
      must not be under it is the disk, so swapping takes it around the entry
      and lets go of it while reading.

      **Two cores on THE SAME page.** This is a new race and had to be handled
      explicitly: both see no mapping, both bring a frame, and the loser has
      to find out that it lost; otherwise the winner's page, already written,
      gets replaced by an empty one. Hence the mapping "only if nothing is
      there".

      **The entry must be EMPTY, not merely not present.** A page swapped out
      to disk is not present either, and giving it a fresh page of zeros would
      lose what is on the disk. A separate question with a separate answer.

      **The frame allocator scanned the bitmap from zero on every
      allocation.** Allocating N frames cost N squared, under a lock held the
      whole time. A program touching nine thousand pages spent more time
      walking a bitmap it had already walked than on anything else. A cursor
      remembering where the last search ended solves that for the one-frame
      case, which is almost every caller. That one helps on a single core too.

      **What I do NOT claim.** The timing here is unstable, and I say so
      plainly: in a fresh machine `run faults` gave 21, 45 and 51 ticks, and
      `runw faults` 12, 12 and 25. The direction is visible, the number is
      not. That is why the assertion does not rest on a stopwatch but on a
      counter: the kernel counts how many faults it handled by which path.
      After `runw faults` it grows by 18001 without the lock and by ZERO with
      it: two processes of nine thousand pages each. That says directly what
      a stopwatch can at most suggest.

      Correctness is guarded just as strongly, and it is what gives the
      counters their value: every page has to come back with what was written
      into it, and a fresh one has to be zeroed. A frame handed to two
      processes at once shows up here as a wrong sum.

      830 assertions, 51 groups, 493 s.

      A note for the future: with 51 groups and four machines at once, the
      timing assertions in `proccores` and `lockless` started to lose against
      the host's load now and then (35 against 44 ticks with a threshold of
      3/4). Run alone they pass with room to spare. The threshold is not
      wrong; timing in a virtual machine standing next to three others is
      simply not reliable.

- [x] **System settings**: the machine starts to remember something about
      itself.

      Everything the system knew about itself was written into it at build
      time. The name server could be changed with a command, and the change
      lived exactly as long as the machine; switching it off was enough to
      forget.

      A setting is a line in a text file on the disk: name=value, and nothing
      more. Plain text on purpose: it can be read with `cat` and changed with
      `write`, and when something goes wrong one can see what. The kernel
      reads this file once, at boot, right after the filesystem.

      The set of names is fixed and explicit, also on purpose: each of them
      has a place somewhere in the system that reads it and does something
      with it. A name nobody reads is not a setting, it is a line in a file.
      Hence four and not forty: the machine name (the prompt), the name server
      (the network), the prompt colour (the console) and the desktop
      background (the compositor).

      **A window on the desktop.** The `gsettings` program in ring 3 keeps
      none of this: it asks the kernel for the i-th setting and asks the
      kernel to change it. There is one place that decides what a setting is,
      and it is not the program. The program has the part the window exists
      for: which row is selected, what was typed and whether it has reached
      the disk yet.

      Two ways of changing, because settings come in two kinds. A number from
      a small set (the background, a colour) is clicked through in its row;
      there is nothing to type when there are six possibilities. Text (the
      machine name, a server address) is typed, because it cannot be guessed.

      **The assertions are not about the file but about EFFECTS.** A file with
      the line `name=machine` proves nothing; a prompt saying `machine>` after
      a restart proves everything. So every setting is checked where it is
      used: the name in the prompt, the colour in the prompt's colour, the
      background in the desktop's pixels, and the name server in what
      `ifconfig` says, and each one across a RESTART on the same disk image.

      The window is checked the same way: the icon opens it, a click cycles a
      value, Enter saves, and after a restart the desktop has a different
      colour.

      One bug on the way worth writing down, because it shows how easy it is
      to get one convention wrong: fs_write returns zero on success, not a
      number of bytes. I checked "more than zero", so every successful write
      reported failure: the file on the disk was correct and the message said
      it had not worked. No test caught it, because there was no test yet; an
      eye on the output did.

      849 assertions, 52 groups, 483 s.

- [x] **Closing and resizing application windows**: the desktop starts to
      behave like a desktop.

      A window that can be opened and not closed is not a window. And that is
      exactly how it was: the compositor's [x] button pulled the window out
      from under a running process, which kept drawing into something that no
      longer existed. Resizing worked only for the kernel's own windows.

      **Closing is a REQUEST.** The compositor tells the application its
      window is being closed and gives it time to finish its own way. A
      program that answers ends with its own exit code, having cleaned up
      after itself.

      **A program that does not answer dies.** A window nobody can close is
      worse than an application losing its say, so after about two seconds the
      desktop stops asking. There is a separate program to check this,
      `gdeaf`: it opens a window and listens to nothing. It dies with code -9,
      and that is the point.

      **Resizing** gives the window a new content buffer and tells the
      application the new size, once, at the moment the corner is released,
      not on every frame of the drag. The old content is not carried over: the
      program is about to redraw everything anyway, and a stretched copy would
      only be visible until then.

      Two bugs on the way worth writing down, both of the same kind, a shared
      file compiled twice:

      - desktop.c is shared by both kernels, so reaching for task_kill broke
        the link of the 64-bit kernel, where that name does not exist. The
        compatibility layer got it as an uncatchable signal. I saw it only
        after a full run, because I had run the earlier build with stderr
        silenced, and that is the real lesson of this bug.
      - Graphical programs are compiled twice as well, so the new events had
        to be added to user64/usys64.h too.

      **Timing assertions got a retry.** The `lockless` and `proccores` groups
      sometimes lost against the host's load: with 53 groups and four machines
      at once, timing in a virtual machine next to three others is not
      reliable. The threshold did NOT change; what changed is how many
      readings it takes to believe one: when the first one loses, we measure
      again.

      862 assertions, 53 groups, 480 s.
- [x] **Twelve windows at once and a taskbar that fits them**: no more of a
      limit one could hit just by clicking icons.

      The compositor held six windows, and two of them are its own. That
      means four applications, with seven icons on the desktop. The fifth
      did not come up and did not say why: win_create returned -1, the
      program ended quietly, and nothing happened on the screen. The worst
      kind of limit: one you cannot see until you walk into it.

      The number itself is one change, but done sensibly it is three things:

      **More slots**: twelve instead of six, i.e. seven icons plus a few
      more from the menu.

      **A cascade that does not go off the screen.** Windows are placed in
      steps to the right and down; far enough along those steps, a window
      lands under the taskbar or past the right edge, where it cannot be
      seen or grabbed. Now the step goes back three positions as long as the
      window fits, and at the end it is pushed against the edge anyway. The
      step back is computed from the SLOT NUMBER, not from a count of
      openings, so slot 2 still gives (94,54) and the coordinates older
      graphical tests stand on did not move.

      **A taskbar that shares out its width.** The taskbar is a fixed piece
      of pixels: 640 minus the "Menu" button is 592 px. Twelve buttons of
      52 px are 624, so the last two were drawn off the screen. The button
      width is now computed from the number of open windows and the space
      left, and the length of the label from that width (the font is 8 px).
      With nine windows it comes out at 52 px as before, with twelve at 49.

      And the most important part: ONE function computes the width, and both
      the drawing and the click handling ask it. Two places computing the
      same thing separately drift apart quietly: a button is visible where it
      does not work, and nobody can say what is broken. The test aims right
      at that: it clicks the LAST button (the one that exists only because
      the buttons got narrower) and checks that the focus went to its window
      and not to a neighbour.

      A new group, `manywin`: seven icons open seven windows, the menu adds
      three, the taskbar shows twelve buttons, no two overlap, all fit on the
      screen, and clicking the last, a middle one and the narrowed last one
      lands on the right window every time. The assertions are about the
      taskbar's pixels, because that is the only place where both what is
      drawn and what is clickable can be seen at once.

- [x] **A window filling the screen**: the last thing a window could not do,
      and the thing one does with a window most often.

      A window could be moved, grabbed by a corner, minimised to the taskbar
      and closed. It could not be stretched to fill the screen, and that is
      what a person does with a window they really work in. Dragging the
      corner to the edge is not the same, and the differences are worth
      listing, because they are the whole content of this change:

      **Up to the taskbar, not one pixel further.** The taskbar is the only
      way to the other windows. A window drawn over it would be a window with
      no way out.

      **The window REMEMBERS where it came from.** The same button puts it
      back exactly where it stood, not in some sensible place but in the same
      one. Hence four fields in the window structure, not one flag.

      **The application gets the new size.** A window is not only a rectangle
      but also a content buffer, so the buffer grows and WEV_SIZE is sent. A
      program that was not told would keep drawing on 330 pixels in a window
      638 wide, and the rest would be an empty grey area, looking like a
      drawing bug while being a missing message.

      And that is what the assertions stand on. `gsettings` draws the
      selected row as a blue bar across the WHOLE width of its content, so
      that bar is a ruler: after maximising it must reach the right edge of
      the screen, and after restoring it must end where the smaller window
      ends. Both show in the pixels.

      Two things on the way, both of the "what if" kind:

      - No memory for a 638x456 buffer (and the kernel heap is 1 MiB)? Then
        NOTHING happens: the window stays as it was. Half a maximise (a window
        larger than its buffer) would draw somebody else's memory on the
        screen. resize_content now returns -1 instead of staying silent.
      - Grabbing a maximised window by its title bar restores its own size,
        as everywhere else, and the window comes back under the cursor at the
        same point of its title bar, so it does not slip out from under the
        hand. Dragging a corner simply stops being a maximise: whatever comes
        out of the drag is the window's new size.

      A new group, `maxwin`.

- [x] **A clock on the taskbar**: the machine had a clock, but one had to ask
      it.

      `date` printed it, and a program could get it through a system call. It
      was not where a clock is needed: on top, without asking. Now it sits at
      the right end of the taskbar and shows the time to the second.

      Reading the CMOS clock means waiting out an update in progress and
      reading until two readings agree, and the desktop redraws about thirty
      times a second. So the time is read every sixth frame (about five times
      a second) and kept in a buffer. No second is lost that way, and the CMOS
      is not asked thirty times a second for a digit that changes once.

      The space for the clock is SUBTRACTED from what the window buttons share
      between them; it is the same function that computes their width. With
      twelve windows a button is now 41 pixels instead of 47. A clock drawn
      over a button would be a clock that one button hides.

      The test reads the digits FROM THE SCREEN: the eight-byte pattern of each
      character is looked up in the same font the kernel drew with (the console
      tests in graphics mode work the same way). The machine starts with the
      clock set to a fixed moment, so the digits must show that moment, and
      after four seconds a few seconds more. A clock drawn once and never
      refreshed would pass the first assertion and fail the second.

      A new group, `clock`.

- [x] **A terminal that is a terminal**, and one program instead of two.

      What existed ran one command and drew what it printed. Each next one
      erased the previous, the window was 240x150 whatever a person did with
      it, and there was no cursor, no history, no way back to what scrolled
      off and no change of directory. It proved that a ring 3 program can
      start another and read its output. It was not a terminal.

      A terminal is a session. What was run stays on the screen above what is
      being typed; the prompt says where you are; the arrows go back to what
      was typed; and how much of it is visible is decided by the size of the
      window, not by a number written into the program.

      So: a ring of lines (SCROLL; the oldest drops out, since moving fourteen
      kilobytes on every new line would be the other way), the prompt and the
      command echoed as they happened, output appended as it is read, PgUp/PgDn
      over what scrolled away (with a note that we are not looking at the end;
      otherwise the terminal looks hung), left and right arrows, Home and End,
      typing AT THE CURSOR, `cd` changing the prompt at once, a pipe `a | b`,
      and a message instead of silence when a program does not exist.

      **And one file instead of two.** There were two terminals, user/gterm.c
      and user64/gterm64.c, each with its own bugs to fix separately. One is
      left, compiled twice, exactly like the kernel's shared files. One thing
      differs in it, the one that really depends on the mode: how to run a
      command and catch its output. In protected mode the kernel has a call
      for that (spawn with redirection, and a whole pipeline in one call); in
      long mode it does not, so the terminal builds it itself from fork, dup2,
      exec and pipe, the way a shell does. The rest (the ring, editing,
      history, drawing) is one piece of code.

      Three things the test caught and the eye did not:

      - After running a command, the input line was "empty" only by having its
        length reset; the old text was still in the buffer and was drawn under
        the cursor. An empty line is one whose first byte is zero.
      - The "^ scrolled back" note was drawn straight over the first row, and
        both texts shared the same pixels. Now it clears the background under
        itself first. That showed only when reading the screen through the
        font: a person sees "something is there", the test sees
        "co^u????scrolled".
      - The note is fifteen characters long, and the background cleared under
        it was fourteen, so the last letter was drawn outside the cleared area
        and got lost at the edge. A test reading the note letter by letter sees
        a difference like that; the eye does not.

      **A dark theme and English.** The terminal has a background in the
      palette's black and light text, as a terminal looks. Desktop windows are
      grey, so it is clear at once that this is not a form but a screen in a
      window. Its own messages are in English ('no such program',
      'cd: no such directory', '[code N]', '^ scrolled back'), unlike the
      kernel's messages, which stay in Polish.

      The tests read DIGITS AND LETTERS FROM THE SCREEN: the eight-byte pattern
      of each cell is looked up in the same font the kernel drew with. So the
      assertions talk about what a person sees, not about what the program
      believes. Two groups: `gterm` (the session, history, moving in the line,
      cd, a pipe, scrolling, more lines after stretching the window) and
      `gterm64` (the same session through fork, dup2, exec and pipe in long
      mode).

- [x] **Alt+Tab**: the keyboard gets what only the mouse had so far.

      With twelve windows the only way to a window was the mouse: a click on
      the window or on its taskbar button. The keys for switching had been in
      the driver for a long time; it knew Alt was held and it knew what Tab
      was. Nobody had put the two together.

      Now Alt+Tab gives the focus to the next window and raises it. The order
      is the order of the slots, i.e. the order of the buttons on the taskbar,
      so two presses take you where looking at the taskbar says they will.
      Minimised windows are skipped (a focus you cannot see is a keyboard
      typing into nothing), and so are the kernel's two windows: there is no
      program behind them to take the key.

      The Alt+Tab code is produced in kbdmap.c, shared by both kernels, as
      KEY_NEXTWIN, another control code next to the arrows. The desktop catches
      it before anything else; where there is no desktop, nobody reads it and
      nothing happens on the text console.

      A new group, `alttab`. Two assertions matter more than the rest here:
      that the window really COMES TO THE FRONT (its whole title bar is
      visible, not half of it under a neighbour) and that THE KEYS FOLLOW IT:
      after Alt+Tab, a typed command lands in the terminal that a moment
      earlier was under two other windows.

- [x] **Who is at the machine**: two levels, a password and sudo.

      Everything that ran could do everything. A ring 3 program could
      overwrite the file the system reads its settings from at boot, delete
      the swap file or change the machine's name, and so could anyone who
      typed something. There was nobody to ask, because there was nobody.

      Now there are two levels: an admin, who can change the system, and an
      ordinary user, who can do everything else. No groups, no file owners, no
      permission lists; two levels are enough to say the one thing worth
      saying: that changing the system is a different activity from using it.

      **The level belongs to the process and travels to its children.** A new
      field in the task structure in both kernels; fork and exec inherit it.
      The kernel's shell is not a process, so it has its own level beside, and
      a program started from the shell gets its level, because a thread
      standing in kernel space is, from this point of view, the shell.

      **Up through a password, down for free.** Three system calls: auth, drop,
      level. Going down is never checked; giving up a privilege is always
      allowed.

      **The password is stored in the file as a number**, not as text, and it
      is worth saying plainly what that is: an eight-byte hash (FNV-1a with a
      salt), not a function made for passwords. It protects against READING
      the password from a file anyone can look at. It does not protect against
      someone who takes the disk and goes at that number with a dictionary,
      and nothing here pretends otherwise. Guessing costs time: three wrong
      attempts and the machine answers "no" for a few seconds without even
      looking; otherwise a six-character password is an afternoon's work for a
      program in ring 3.

      **What is guarded:** the settings (SYS_CFG_SET and the `set` command),
      the system's own files (system.cfg, swap.sys, icons.pos, on opening for
      writing and on deleting), and signals to other people's processes. The
      console guards the same things as the system calls; otherwise it would
      be a way around every rule the rest of the machine follows. Anyone may
      read these files: there is nothing secret in them, and changing is a
      different thing from looking.

      **Three places where one asks for it**, and all of them ask the same
      way, with asterisks instead of text:

      - the kernel shell: `sudo <command>` (one command), `sudo` (a session
        until logging out), plus commands to show who you are and to change
        the password. The prompt ends with `#` instead of `>` when you are an
        admin, the oldest way of saying it, and one that is seen rather than
        read.
      - the terminal in a window: `sudo <command>` turns the line into a
        password line. A ring 3 program cannot raise its own privileges; it can
        only get a refusal and do something with it. It does what is wanted:
        asks, tries once more, and drops back down right away.
      - the settings window: a save comes back refused, so the window says so
        and asks for the password under the setting being changed.

      On top of that, `rm` and `cp` now tell a refusal apart from "no such
      file"; those are two different answers, and a person does different
      things after them.

      Two new groups: `users` (the console: a refusal that really does not
      change the value; a wrong password; one command, not a session; the lock
      after three attempts; a password change surviving a restart; the file
      without the password in plain text) and `usersgui` (the terminal and the
      settings window). The `k64shell` group got the same assertions for long
      mode, because it is the same user.c on both sides.
- [x] **A terminal that does not freeze**: live output and Ctrl+C.

      The terminal started a program and WAITED: it drew nothing, answered
      nothing and showed nothing until the program ended. With a program that
      ends quickly this did not show. With one that does not end (and `spin`
      is exactly that) it took the window with it: no output, no way to
      close it, no way to stop it. That is not a terminal running a program,
      it is a terminal that has stopped.

      **Output read as it is written.** The program writes to a file, and on
      every pass of its loop the terminal reads what lies past the point
      where it stopped last time. So lines appear while the program is still
      running, and keep appearing.

      **A wait that does not wait.** That needed a new system call in both
      kernels: SYS_TRYWAIT, "0 if it has ended (and here is the code), -3 if
      it is still running". Without it, a program watching another one has to
      choose between blocking and not knowing. The 64-bit kernel also got
      seek on the way: without it, reading "what is new" would mean reading
      everything every time.

      **Ctrl+C means the same as everywhere.** The key arrives as 0x03 like
      any other, the terminal sends SIGINT to the program it started, and a
      program without a handler of its own dies of it. Then the terminal
      takes the next command.

      One bug worth writing down: the output file was not cleared before the
      start, so the first read of a new command showed leftovers of the
      previous one. Reading only at the end hid it: the old file had already
      been overwritten before anyone looked. Reading as it goes dragged it
      out.

      A new group, `gtermlive`: output is visible before the program ends,
      there is more and more of it, the window takes keys while the program
      runs, Ctrl+C shows in the session and ends the program, nothing is
      appended after it, and the next command shows its own output, not the
      leftovers of the previous one.

- [x] **Talking to a program**: a shell in the terminal window.

      The terminal could start a program and show what it printed. It could
      not say anything back to it: the program's input came from nowhere, so
      everything it reads was out of reach. No shell, no program asking about
      anything, nothing interactive.

      Now what is typed while a program runs goes through a pipe to its
      standard input, and Ctrl+D closes that pipe, which tells the reader
      there will be nothing more. It took three things:

      **A way to start a program with a pipe into it.** In protected mode the
      kernel does it: a new call, SYS_SPAWN_TALK, creates a pipe, gives the
      child its reading end as descriptor zero, and hands the caller the
      writing end as an ordinary descriptor. In long mode the terminal builds
      it itself from pipe, fork and dup2; all of that was already there.

      **A shared stdout.** The shell's children have to write where the shell
      writes. Every descriptor carries its own position, so the only place
      everyone agrees on is the END of the file. Without that, the shell's
      next prompt landed on what its child had just written, and the output
      vanished under it. The same in both kernels: in the 32-bit one a flag
      on the descriptor, in the 64-bit one the rule "a file under descriptor
      one is written at its end".

      On top of that, in the 32-bit kernel a child now inherits its parent's
      redirected stdout (exec passes the inode), because otherwise programs
      started by the shell would write to the text console, which nobody
      looks at in graphics mode.

      **An end of input that means something.** Both user shells spun in a
      loop when read returned zero: `if (n != 1) continue`. A shell reading a
      stream that has ended has ended too; now zero from read ends both.

      **On the way, two sources of false alarms in the tests**, both fixed
      without touching what the assertions claim:

      - The wait for the prompt at boot was fifteen seconds. A full run starts
        four machines at once, and four kernels booting on one host need
        more; this is a wait FOR AN EVENT, so a longer limit weakens nothing.
        Thirty seconds.
      - The `lockless` group measures whether two processes can be in the
        kernel at once. That is a claim about POSSIBILITY, not about an
        average: only what the big lock did refutes it, two cores slower than
        one. So a measurement that loses is repeated (up to four times) and
        the best one counts, and the margin is now 0.85 instead of 0.75. The
        64-bit kernel comes out at 0.42, the 32-bit one at 0.68-0.80, and that
        is an honest picture: in protected mode most calls still sit under
        the big lock, and only page faults are free of it so far.

      And this is the proof that it works: the user shell (ring 3) runs in the
      terminal window, its commands execute, its children's output is BELOW
      the command and ABOVE the next prompt, a pipeline inside the shell
      inside the terminal writes to the same window, and Ctrl+D ends it all.
      A new group, `gtermsh`; the same path in long mode added to `gterm64`.

- [x] **A clipboard**: text that moves from window to window.

      Copy and paste is the simplest thing two programs do together, and the
      only thing they cannot do on their own: neither can touch the other's
      memory, and neither knows the other exists. So the text has to wait in
      a place that belongs to both, which is the kernel.

      Five hundred and twelve bytes of text and two system calls, in both
      kernels. No formats, no owners, no negotiating what a selection means;
      that is enough to paste a name typed in the terminal into the settings
      window, and anything beyond it would be a protocol nobody here needs
      yet.

      The oldest keys there are: Ctrl+Insert copies, Shift+Insert pastes. Not
      Ctrl+C: in a terminal Ctrl+C means "stop this program" and nothing else.
      Insert is the only extended key whose meaning depends on what is held
      with it, so that is decided where that state is known, in kbdmap.c,
      shared by both kernels.

      The terminal, the settings window and BOTH kernel shells use it: the
      clipboard belongs to the system, not to one window. It shows best on the
      text consoles: a line copied on the first one pastes on the second,
      which has its own buffer and its own shell.

      A new group, `clip`. The most important assertion is the last one: the
      window the text came from is CLOSED, and the text still pastes, because
      it never belonged to that window.

- [x] **Selecting with the mouse**: the clipboard finally reaches what a
      program wrote.

      The clipboard could take the line being TYPED. It could not take
      anything a program had written, which is most of what is in a terminal,
      and exactly the part a person wants to copy: an address, a number, a
      file name from a listing.

      Selecting needs something the compositor did not say. A click says
      WHERE something happened; a selection is what happened BETWEEN two
      places. So the desktop now sends three events for a drag inside a
      window's content (button down, movement with the button held, button
      up) in content coordinates, just like a click. What they mean is the
      application's business: the compositor knows nothing about selections
      and does not have to.

      The terminal turns them into a range in ITS RING OF LINES, not on the
      screen, so the selection stays on its text when the view scrolls under
      it. And releasing the button copies: in a terminal, selecting HAS BEEN
      copying for forty years, and there is no second key to press.

      A new group, `gtermsel`: the selected piece has a field under it (and the
      neighbouring one does not), exactly what was under the mouse is pasted,
      a selection across two lines takes both, and a plain click clears the
      selection.

- [x] **The look of the desktop**: depth instead of flat rectangles.

      Everything on the screen was drawn with one call: fill this rectangle
      with this colour. It worked and it looked like it: a flat background,
      flat windows with a black border, tiles with a label underneath. There
      was no way to say which window was listening, that a window is ABOVE the
      desktop and not in it, or what was on an icon beyond its caption.

      The screen has eight bits per pixel and its own palette, so a shade
      costs one index and nothing more. The whole depth is made of that:

      **Light falls from the top left**, and it falls that way everywhere. An
      edge light at the top and left and dark at the bottom and right is a
      raised window; swapping those two lines makes it sunken. Hence a button
      that stands out, a clock field set into the taskbar, and the focused
      window's button that is PRESSED, not merely another colour.

      **A shadow under a window**: not black but a darkened background; black
      on a dark desktop is a hole, not a shadow.

      **The background is sixteen bands**, darkening towards the bottom.
      Sixteen and not six, because six can be counted by eye: the steps have to
      be small enough to be a slope and not stairs. The range is narrow on
      purpose: a machine set to "sea" should look like the sea from every part
      of the screen.

      **A title bar in two colours, and a different pair when the window has no
      focus.** That is a functional change, not decoration: before, there was
      no way to see which window would take a key. Now it shows from the other
      end of the screen.

      **Icons have pictures.** A coloured rectangle with a word under it is a
      label, not an icon; it says nothing until it is read. A picture is
      recognised before it is read, and that is what icons are for. Twelve by
      twelve pixels is not much room, so each one is a single idea without
      details: a window, a clock face, a folder, the bars of a chart, a prompt,
      a globe, a gear.

      A new group, `look`. A look cannot be written down as "pretty", so the
      assertions are about the STRUCTURE the eye reads: the background darkens
      towards the bottom in steps that cannot be seen; a window casts a shadow
      darker than the background beside it; edges are light at the top and dark
      at the bottom; the focused button has that reversed; there is exactly one
      blue title bar and it moves with a click; an icon has dark pixels on it,
      and two icons have different ones.

      There were quite a few fixes in old tests, all of the same kind: probes
      hit a pixel whose meaning had changed. The title bar of an unfocused
      window is no longer blue, the taskbar buttons now have edges, and the
      labels moved by a pixel so that an edge does not cut the last row of
      letters. None of those assertions was weakened: each says the same as
      before, in the place where it can now be seen.

- [x] **Flat and clean**: the same said with colour instead of relief.

      The previous step added depth: light from the top left, shadows under
      windows, raised buttons, sunken fields. It worked, but it looked like a
      system from thirty years ago, and we want modern and clean. So the relief
      went, and what it said stayed:

      **A surface is one colour.** No gradients on buttons, no edges light at
      the top and dark at the bottom. What separates two surfaces is one line a
      shade darker.

      **There are no shadows.** Not under a window, not under an icon, not
      under the menu.

      **There is one accent and it says one thing**: which window is listening.
      That window's title bar and taskbar button are blue, the rest is grey.
      Since that is the only thing that shouts, nothing competes with it.

      **The title bar buttons are just symbols**, without frames around them.
      The symbol says what they do; a frame would only add ink. The exception
      is close, which stays red, because that one has to be unmistakable.

      **Icons are flat tiles with a white drawing.** No shadow, no edge: a shape
      in the accent colour differs enough from the desktop to be a thing.

      One thing from the previous step stayed, because it is not relief but
      space: the background in sixteen bands, darkening towards the bottom.

      The `look` group now says what is really visible: a window's inside is
      ONE colour (four readings, all equal; otherwise it would be a gradient or
      an edge), one line separates it from the desktop, there is nothing under
      the window, the focused button has the accent colour and is flat (top and
      bottom the same colour), and an icon tile is flat with the white pixels
      of its drawing on it.
- [x] **Pictures on the taskbar, and a taskbar to configure.**

      The taskbar carried names, cut to six letters, so "Settings" read as a
      stub and two terminals read the same. It carried them because a window
      knows its title and nothing more; it does not know what was clicked to
      start it.

      Now the desktop remembers. When it starts something from an icon, it
      records which icon that was; the window that process opens a moment
      later gets the same picture. Nothing else would connect those two
      things: the compositor sees a pid, and the icons are the only place a
      program has a face. Kernel windows and everything started from the shell
      get the default window symbol.

      The taskbar grew from twelve pixels to eighteen, because what is on it
      now is pictures, not words: a twelve-pixel drawing needs room around it.
      On the way a button shrank to a third of what the cut name took, so
      twelve windows now fit WITHOUT narrowing.

      **And the taskbar became a matter of taste rather than fate.** Two
      settings, kept on the disk like the rest (their names were Polish at the
      time):

      - the bar width: 100, 75 or 50 percent of the screen width. Narrower than
        the screen, it is centred, and that is exactly what makes it a dock.
      - the icon position: 0 on the left, 1 in the middle, 2 pushed against the
        clock.

      The drawing reads them and the click handling reads them, through the
      same two functions, so a button is clickable exactly where it is seen,
      in every one of these settings. That is the assertion that means the
      most in the new `bar` group; the rest say that the bar really has that
      width, that the pictures really move, and that two windows of different
      programs have different pictures while two kernel windows have the same.

      The settings window can switch both with a click: the icon position like
      any other cyclic setting, and the width its own way, because it is not a
      sequence of numbers but three of them. The window itself grew, because
      six settings with a description and a line about what happened no longer
      fitted in a hundred and sixty pixels.

      One more timing assertion, "four chunks at once faster than one after
      another" in the `kthreads` group, got what `lockless` got earlier: a
      losing measurement is repeated and the better ratio counts. The threshold
      stays where it was (halfway between one and four); only how many readings
      it takes to believe one changed, on a host running four machines at once.

      On the way, two assertions in old tests stopped computing a row's
      position and started SEARCHING for it in the band where it can be: where
      the program writes "saved to disk" depends on the number of settings and
      the length of one of their descriptions, i.e. on a layout that is the
      program's business. The message is the test's business.

- [x] **A menu with pictures, the window's name under the cursor, a wallpaper
      from a file.**

      Three things in the same spirit: the desktop should speak without
      reading, and where reading is needed, it should offer it on request.

      **The menu has pictures.** The same as the icons and the taskbar buttons;
      a program that looks like three different things is three things to
      remember. Entries now carry the name under the icon ("Clock"), not the
      program's name ("gclock"), because the program's name is for the shell.
      The entry under the cursor gets the accent colour: without it a menu is
      a list of words that happens to be clickable.

      **The window's name comes back under the cursor.** The picture says WHICH
      program; it does not say which of two terminals. So above the button the
      cursor is on, the window's name appears, and only then. It is drawn at
      the very end of the frame, after the menu, because what explains has to
      be above what it explains.

      **The wallpaper is a file.** The background was a colour, then sixteen
      bands of colour. It could not be a picture, because a picture has to come
      from somewhere, and a machine with no camera and no editor has only
      arithmetic. So a picture is computed by the `mkwall` program in ring 3,
      saved to the disk, and the compositor reads it at boot.

      The format is the whole idea: "PWAL", the size, 192 colours of three bytes
      and one byte per pixel. Small on purpose, 160 by 120, stretched over the
      screen. A full-screen image is three hundred kilobytes and the kernel heap
      is a megabyte; this one is a fifteenth of that, and each of its pixels
      covers a piece of the screen. It looks like what it is: a picture, softly.
      The wallpaper's colours sit ABOVE the desktop's colours (indices from 64),
      so the wallpaper has no way to repaint a window.

      Four patterns, because that is what one afternoon of arithmetic is worth:
      a slope, rings, a weave and a lattice, each in six hues.

      A new group, `wall`. The best assertion is the one that tells a picture
      from a gradient without knowing what is drawn: a gradient has ONE colour
      across a row, and a picture has many. Besides that: the file has the size
      the format implies, the kernel says at boot what it loaded, a window on
      top of the wallpaper still has its own colour, and a wallpaper that does
      not exist breaks nothing; the gradient comes back.

- [x] **Desktop icons from a file**, and no more list written into the
      kernel.

      Seven programs, fixed when the kernel was built. Adding an eighth meant
      editing kernel/desktop.c and rebuilding the kernel, which is the same as
      "it cannot be added".

      Now the list is a file on the disk, one line per icon:

          program  label  picture-number

      and nothing more. Plain text, like everything this system remembers. It
      can be read with `cat` and, more importantly, IT CAN BE MADE ON THIS
      MACHINE: the shell in the terminal has redirections, so two `echo` lines
      with `>` and `>>` are enough. A format nobody on the machine can produce
      is a format for somebody else's machine.

      A line that does not parse is skipped; an empty file, or one with no
      sensible lines, keeps the built-in seven. A desktop without icons is a
      machine with nothing to click on, and no broken line should have that
      power.

      A new group, `icons`, builds the file exactly as a person would (through
      `run sh` and echo with `>` and `>>`) and then reads FROM THE SCREEN that
      the tiles have the labels and pictures from the file, that there are as
      many as there are lines, and that a click on such an icon starts its
      program.

      On the way, two holes in the test harness itself, both of the same kind:
      QEMU does not know the key 'T' or '#'. A capital letter is shift plus the
      letter, and '#' is shift plus three; without that, typed text quietly
      lost characters, and it showed only as "erminal" instead of "Terminal"
      in a file the test had written itself.

- [x] **Alt+F4 and a right-button menu**: two gestures everyone knows that
      this desktop did not have.

      The keys had been in place for a long time: Alt with a function key
      switched text consoles. But on the desktop there is nothing to switch
      (the screen is one), so those presses went nowhere. Now, when someone has
      taken over the keyboard (and on the desktop the compositor does), the
      same keys go to it, and F4 means what it has meant everywhere for thirty
      years.

      It closes EXACTLY like the [x] in the window's corner: the program gets
      the question and ends on its own, with its own exit code. Pulling the
      window out from under a running process would be different from what the
      mouse does, and one gesture and the other should do the same thing;
      otherwise there are two things to remember instead of one.

      The right button on the empty desktop opens the menu where the cursor
      is. It is the same list as under the "Menu" button in the corner,
      because a second list would be a second thing to learn; it just comes to
      the hand instead of sending the hand to the corner. The menu does not go
      past the edge of the screen or onto the taskbar: when clicked in a corner
      it slides left and up. On a window the right button opens nothing (a
      window's own menu is the window's business, and none has one) and closes
      the one that was open.

      The key path is not shared (long mode has its own keyboard driver), so
      Alt+F4 had to be passed through it separately. A new group, `winkeys`,
      checks both kernels: that a program closed with Alt+F4 ends ON ITS OWN
      with code zero (so it was asked), that the window disappears from the
      screen and from the taskbar, that the menu stands under the cursor and
      not above the "Menu" button, that the item under the cursor lights up,
      that a program chosen from it really runs (the clock draws digits, not
      just an empty window), and that at the edge the menu fits.

- [x] **A window snapped to an edge**, i.e. two windows side by side in one
      move.

      Two windows side by side used to mean two drags and two corner pulls, by
      eye, and the eye is bad at halves. Now a window pushed to the left edge
      takes the left half, to the right edge the right half, and to the top the
      whole screen. There is nothing to learn: the window goes where it was
      pushed.

      While the button is held, the place the window WILL take is drawn as an
      outline in the accent colour. A window that jumps somewhere else without
      warning when released is a window that escaped; the outline is a
      question, and releasing the button is the answer.

      The decision is made from the WINDOW's position, not the cursor's. The
      cursor can already be at the very edge while the window has not reached
      it (a dragged window never goes off the screen), and a person aims with
      the window. The cursor only says which edge was meant; otherwise a window
      wide enough to touch both would snap wherever it was dropped. Corners
      give a half, not the whole screen: two windows side by side is what one
      aims there for, and the whole screen is one button away anyway.

      The size from before the snap is remembered the same way as with
      maximising, so the [] button gives the window back its own size; a window
      kept on half the screen has lost its size no more than a maximised one.

      A new group, `snap`, checks that there is no outline in the middle of the
      screen, that there is one at the edge and it ends above the taskbar, that
      the window really takes half (grey on one side, desktop on the other),
      that the program redraws at the new size, that [] gives back its own
      width (360, not 320 or 640) and, finally, what all of it is for: a
      terminal on the left half, the settings on the right, the border exactly
      in the middle.

- [x] **Four virtual desktops**: the oldest answer to a screen that has run
      out of room.

      Instead of a dozen windows in one pile, four small piles to switch
      between. Switching costs nothing: nothing is drawn, moved or resized. The
      whole thing is one question, here(i), asked by everything that draws a
      window, counts windows or looks for the window under the cursor. A window
      on another desktop is not HIDDEN; it is simply not here. It runs and
      draws into its buffer as before.

      Ctrl+Alt and an arrow changes the desktop; the arrow shows where one is
      going. With Shift the focused window comes along, because a window left
      behind is a window searched for. We follow it instead of pushing it out
      of sight and staying.

      Which desktop is visible is shown at the right end of the top bar: one
      cell per desktop, the visible one in the accent colour, empty ones in a
      dimmed grey. "Where am I" and "is there anything there" are one glance,
      not two. A cell is also a button.

      What ends on a switch: dragging a window, dragging an icon, resizing and
      an open menu. A drag that survived a switch would drop the window on a
      desktop nobody is looking at.

      On the way, one thing only the full suite caught: a cell on the top bar
      lies exactly where a window maximised to the full screen has its buttons.
      The bar is drawn UNDER the windows, so it has to take clicks under them
      too; otherwise the [] of a maximised window switched the desktop. One
      question, window_at(x,y), asked from the top of the stack down, now
      answers both the bar and the right mouse button.

      A new group, `desks` (19 assertions), checks the cells on the bar, that a
      window disappears from the screen AND from the taskbar after moving next
      door, that a new window opens where one is looking, that a click on a
      cell moves there, that Shift takes the window along, and most
      importantly: that a clock left on an invisible desktop shows, on return,
      a time later by as much as we were away. A program nobody looks at keeps
      running.

- [x] **A text editor in a window**, because a system that can make a file
      but cannot CHANGE it is a machine for storing text, not for working.

      Everything that made a file so far (`write` from the shell, `echo` with
      `>`) was one line typed at a prompt, and a line typed at a prompt cannot
      go back to what it said a moment ago.

      So: a white page, black text, a cursor wherever the arrows or the mouse
      put it, Enter splitting a line and Backspace joining two (the only way
      back after Enter pressed by mistake), Ctrl+S and Ctrl+O. The clipboard is
      the same as the rest of the desktop's: Ctrl+Insert copies the line with
      the cursor, Shift+Insert inserts at the cursor, so text moves between the
      editor and the terminal although neither knows about the other.

      The status bar at the bottom is the whole interface: the file name, an
      asterisk when it has changed, the line and column, and, when the editor
      needs a name, a question asked in the same bar. A dialog box would be a
      second window to move, focus and close, for one line of text.

      Two new key codes: Ctrl+S and Ctrl+O. Other Ctrl+letter combinations
      arrive as the bare letter (that is what the shell wants), but a program
      that writes files has to tell "save" from "s". The kernel knows nothing
      more than that the keys were pressed; what to do with them is the
      program's business.

      The cursor is a hairline BETWEEN characters, not a block and not a bar in
      a cell: both of those cover exactly what a person looks at while typing.
      The test caught that at once: it read the text from the screen with a
      character missing, because the cursor ate a letter's column.

      A new group, `gedit` (19 assertions, both kernels), reads the text and the
      status bar from the screen and then goes outside the editor: the kernel
      shell does `cat` on the saved file, and in 64-bit mode (where the kernel
      shell cannot be typed at from the desktop) the terminal does it through
      `ucat64`. One program wrote it, another read it.
- [x] **The editor takes a file, and the file manager has a way to hand it
      one**, and the two kernels do the same again.

      `gedit note` opens the file at once. Making a person type the name a
      second time, in a program they have just told which file they mean, is
      exactly how an editor turns into a chore.

      The file manager got an "Edit" button, because what IS a file manager for
      if not for getting to a file, and getting to a text file means opening it
      in the editor. In protected mode that is one sys_spawn call with a ready
      command line, in long mode fork and exec, since there is no spawn there:
      two sources, because the file manager had separate ones, but the same
      button in the same corner.

      On the way, a hole the editor's test found: in 64-bit mode the desktop
      had nobody to give a key no window wanted. keyboard_console_handler()
      returned NULL, so a screen full of windows was a screen on which nothing
      could be typed into the kernel shell, while in protected mode it had
      always worked. Now the key goes to the queue of the console being viewed,
      which is where the shell reads from. The `desktop64` group checks this
      with one `uptime` typed with windows on the screen.

      The `gedit` group grew to 24 assertions and ends the same way on both
      sides: the file manager sees the file the editor saved, and hands it back
      to the editor.

- [x] **A program can refuse to close**, because closing was always a
      REQUEST; there was just no way to answer "not yet".

      The countdown after the request was there from the start, for programs
      that do not answer. The second answer was missing: a program that HEARD
      and has a reason to stay. An editor with an unsaved file is the whole
      example.

      A new system call, win_keep(window): "I heard, I am not closing yet". The
      desktop stops counting down, and what the program does with the moment it
      asked for (asks in its own window, saves the file) is its business.
      Silence still costs the window: a program that does not answer loses it
      after two seconds.

      On the way, the focus: the close request took it away from the window
      being asked. Since a program can refuse, a second Alt+F4 has to be able to
      reach it, so the focus stays with the window as long as the window does,
      and is let go during cleanup.

      The editor refuses once: the first Alt+F4 turns into a question in the
      status bar, the second is the answer to it. Saving the file clears both
      the asterisk and the memory of the question.

      The `gedit` group now has 30 assertions and checks both sides: that the
      window still stands five seconds after the refusal (the countdown really
      stopped, not just slowed down), that the second Alt+F4 removes the button
      from the taskbar, and that the program ends ON ITS OWN with code zero
      rather than being killed. The same in long mode, where the call has its
      own number and its own handler.

      And one lesson about the test itself: the mouse cursor is as white as the
      editor's page. A probe left under the cursor said "the window is still
      there" about a window that was gone.

- [x] **Every core gets its own scheduler cache line**, and a lesson about
      what a test really guards.

      The `kthreads` group started failing after SIX LINES were added to the
      compositor, code that does not even run in this test (it runs in text
      mode). Four kernel threads that used to finish in 45 ticks needed 508. A
      bisection over my own changes pointed at a culprit that could not be the
      culprit, and that was exactly the clue.

      The `running` and `idle` pointers of every core lie in the cores[] array.
      Eight bytes per core, four cores, all in ONE cache line. Every task switch
      is a write to that line, so with four cores the line travels between them
      instead of work getting done. How much that costs depends on where the
      line boundary happens to fall, and that changes whenever anything in the
      kernel grows. Hence a test that fails because of a change in a completely
      different file.

      Sixty-four bytes per core is the cheapest thing this kernel buys. The
      scheduler lock gets its own line for the same reason. After the fix:
      45 ticks, whatever grows elsewhere.

      The test was not too sensitive. It measured exactly what it was meant to
      measure, whether work runs in parallel, and told the truth about a machine
      that had stopped running in parallel. The threshold stayed where it was.

- [x] **A task manager in a window**, because `ps` prints, and a program
      cannot read printing.

      A list one has to type a command to get is a list nobody looks at. A
      machine with a dozen windows needs one place that says what is running,
      and, when a program has stopped answering, a way to end it other than
      restarting the machine.

      A new call, sys_ps(index, buffer): one row of the task list per call,
      indexed from zero, -1 when there is nothing there, exactly the way a
      directory is read. The order is the order of the list, so the list the
      program shows is one refresh behind, as in every task manager in the
      world.

      The window: a row per task, refreshed twice a second, a click selects, a
      button ends it. Two things it will not do: it will not end a kernel thread
      (those share kernel memory, so taking one takes a piece of the machine)
      and it will not end ITSELF; a task manager that disappears when clicked is
      a joke, not a tool. Both cases say why instead of doing nothing.

      What tells a program from a kernel thread: its own address space, not a
      heap. Both have heaps; the first version showed everything as "program",
      and the test caught it at once.

      A new group, `gtasks` (22 assertions, both kernels), checks that the list
      is ALIVE: a program started while the window is open appears on it by
      itself, and one ended with the button disappears both from the list and
      from the taskbar. And both refusals.

      On the way the test had to move the task manager's window out of the
      cascade's way, because every new window opens one step further and landed
      over the list being read. A person would move it; the test does too.

- [x] **A load meter on the top bar**: how much of the machine is busy, at a
      glance.

      A number says "4128 KiB", i.e. it is something to READ. A bar filled a
      third of the way is something to SEE, and the exact numbers (how much
      memory, out of how much, and how many tasks are behind it) are one hover
      away, so the bar stays quiet until asked.

      The place: the top bar, next to the desktop switcher. That is where what
      is ALWAYS true lives: which desktop is visible and how much of the machine
      is in use. The taskbar at the bottom is for windows; a meter there would be
      one more thing squeezing the buttons (and, as the first version showed, one
      more thing the tests count as a button).

      The fill is drawn INSIDE the frame. Drawn over it, a reading of a few
      percent is one column wide, exactly the one the frame covers, so the meter
      would show nothing until the machine was half full.

      The tooltip can now appear where the thing it explains is: until now it
      always came out above the taskbar, and a tooltip at the other end of the
      screen from the thing it is about is a puzzle.

      The task count: task_count() in both kernels; the 64-bit one had it for a
      long time, the 32-bit one got it now.

- [x] **A window says what it has open**, because three windows called
      "Editor" are three windows to check one after another.

      A new call, win_title(window, text): a program renames its own window.
      The title bar and the taskbar tooltip read the same field, so one call
      takes care of both.

      The editor now says "Editor: note", after loading and after saving,
      because only then does the file have a name. The terminal says
      "Terminal: /dir", and does it where it builds the prompt anyway: the
      directory changes with one `cd`, and the prompt and the title should say
      the same thing.

      Two kernels, two call numbers, one function in the compositor.

- [x] **A double click and "show desktop"**: two reflexes a person tries in the
      first minute.

      Two clicks on the same title bar, close together in time, are one gesture:
      to the full screen or back. "Close together" is twenty frames, i.e. about
      six tenths of a second at thirty-three frames a second, the value every
      system settles on. Shorter and the hand misses on a busy machine; longer
      and two separate clicks (which is how a window is raised) start to count as
      one gesture. The first version had twelve frames and could not be hit.

      The "show desktop" button on the top bar hides all windows, and a second
      click brings back EXACTLY the ones it hid. That is the whole thing: a
      button that only hides costs the window layout, and nobody presses it a
      second time. A window minimised by hand stays minimised, because that was
      somebody's decision.

- [x] **The file manager rewritten**, and one source instead of two.

      What was there listed eight names in a window the size of a postage stamp
      and showed the first hundred bytes of one of them. That proved the path
      "disk -> click -> read -> drawing"; it was not a file manager. A file
      manager is where a person GOES to find a file: it says how big things are,
      it walks into directories and back out of them, it hands a file to the
      program that opens it, and it throws one away when asked twice.

      So: a list with sizes, directories marked and walkable (Enter or "Open"
      goes in, Backspace goes up), the directory's name in the window title,
      PgUp/PgDn scrolls, and a counter in the corner says which part of the list
      is visible; a list that scrolls without a word looks like one that has lost
      something.

      "Remove" asks. A file manager that throws a file away on one click is a
      file manager a person is afraid of, and the second click is an answer, not
      a repetition. It does not remove directories at all.

      The content preview went, because showing content is the editor's job, and
      "Open" hands it the file. Two assertions in old tests talked about the
      preview; they were rewritten for what the program does now: a click selects
      a row, and the selection shows in the accent colour.

      And most importantly: ONE source for both kernels. They were separate
      because of one thing: reading a directory, which in protected mode returns
      the size as the result and in long mode fills a structure. That one
      difference is now ten lines under #ifdef, and the rest (the list, the
      clicks, the drawing) is shared. user64/gfiles64.c was removed.

- [x] **One click, one action**, and buttons that hold the edge.

      Screenshots caught a bug no test saw: clicking "Open" in the file manager
      started the editor TWICE. A window gets both WEV_PRESS and WEV_CLICK (the
      same fingers, two events), and the programs listened to both. Now the click
      is what they listen to, and press and release stay where they are needed:
      for dragging.

      The second screenshot (a window maximised to the full screen) showed buttons
      nailed to a number: "End" stood in the middle of a full-screen window and
      looked like a mistake. Buttons are now placed from the right edge.

      Both things are obvious in a picture at once, and in a test only if someone
      checked how many windows opened.

- [x] **Ctrl+F in the editor and Tab in the terminal**: two keys without which
      typing is slow.

      Ctrl+F asks in the status bar, searches from just AFTER the cursor and wraps
      around the end of the file, back to where it started; a search that stops at
      the bottom has to be thought about, and thinking about searching is not
      searching. Enter on an empty question repeats the last one, because the first
      hit is rarely the one that was meant. It says on which line it found it, or
      that it did not.

      Tab in the terminal completes the word under the cursor with what is on the
      disk. One match completes the word; several push it as far as they agree and
      list the candidates (going further would be guessing which one); none does
      nothing, because that is the only honest answer. It takes the names from the
      working directory, i.e. from where the shell takes them, so what gets
      completed is what would run anyway. While a program runs, the line belongs
      to it and Tab is a tab.

      A third key code for the kernel: Ctrl+F. The kernel still knows only that
      the keys were pressed.
- [x] **Ctrl+Z in the editor**, because an editor without undo is an editor
      one types in carefully, and careful typing is slower than typing and
      fixing.

      What is remembered is not the FILE but the CHANGES: a character inserted,
      a character deleted, a line split, two lines joined. Each of those four has
      an obvious opposite, and doing the opposites in reverse order is the whole
      of undo. A hundred and twenty steps back cost two kilobytes; two hundred
      copies of the file would cost four megabytes.

      The oldest step drops out when the log fills up. Whoever has written a
      hundred and twenty things since that mistake is no longer looking for it.

      On the way, the two line operations, split and join, were written once, as
      split_at() and join_at(): both the keys and undo use them, only in
      opposite directions. Before, the same work was written into Enter and into
      Backspace separately.

- [x] **The task structure on its own cache line, and a hidden bug this
      uncovered.**

      The `kthreads` group started failing again, this time on the 64-bit side:
      four kernel threads took 320 ticks instead of 42. A bisection over commits
      pointed at an unbelievable culprit: FOUR LINES ADDED TO THE KEYBOARD MAP
      (Ctrl+F and Ctrl+Z). Code this test does not even call.

      Task structures live on the heap, and the heap aligns to eight bytes. The
      first words of the structure (the stack pointer and the state) are written
      on EVERY task switch, by the core running that task. Two structures in one
      cache line are two cores writing to one line, and then the line travels
      between them instead of work getting done. Whether that happens depends on
      how the heap blocks happened to be laid out, i.e. on the size of code that
      has nothing to do with it.

      A task now gets a line to itself: we ask for one line more and move up to
      the boundary ourselves, remembering the address to free.

      On the way, a first attempt ("every heap block on its own line") broke the
      xHCI driver. And just as well, because it revealed a bug older than
      everything: when creating a task, only the START and END in the file
      mapping table were cleared, not the `blocks` pointer. Releasing a mapping
      therefore handed the heap a pointer that was garbage from a previous life
      of that memory. It was waiting for a heap that would hand out a block with
      the right garbage in it, and it got one.

      The heap now has a guard: a pointer without a sensible header in front of
      it is not freed but reported. The crash was in kfree and the bug somewhere
      else entirely, and that is exactly the kind of crash worth talking about
      loudly.

- [x] **Tests caught up: eight features get a net**, because it was a net that
      caught a months-old bug in the file mappings.

      Two new groups and three extended:

      `sysbar` (15 assertions): the top bar. The meter shows that something is
      in use and that not everything is; the tooltip gives numbers that hang
      together (used <= total, percent <= 100); and most importantly, those
      numbers are ALIVE: a program started between two looks has to show up in
      the task count. "Show desktop" hides the windows without closing them (the
      buttons stay on the taskbar), and a second click brings back exactly the
      ones it hid; a window minimised by hand stays minimised.

      `files` (18 assertions): the file manager. The size is checked on a file
      whose length the test knows, because a column of numbers from nowhere
      proves nothing. Deleting is checked in both halves: the first click only
      asks (and the file is still there), the second deletes, which the kernel
      shell confirms by no longer being able to read the file. It does not remove
      a directory at all. The window's name says which directory we are in, and
      changes on entering one.

      `winkeys` (+4): a double click on the title bar to the full screen and
      back. `gedit` (+9): Ctrl+F finds a word ABOVE the cursor, so it really
      wraps around the end of the file; Enter on an empty question repeats;
      Ctrl+Z undoes one character at a time; the window carries the file's name.
      `gterm` (+4): Tab completes a name, with several matches it lists the
      candidates and adds only what they share; the window says which directory
      it is in.

      Two things fixed on the way, both found by writing the test: the meter's
      tooltip hid UNDER THE CURSOR (the arrow is drawn on top of it), so it now
      comes out to the left of what it explains; and the word "directory" in the
      file manager was aligned differently from the size column; two columns
      ending in different places read as two columns.

- [x] **Renaming in the filesystem**, because until now moving four megabytes
      meant reading and writing four megabytes to change sixty-four bytes of a
      directory entry.

      fs_rename(): the file's blocks do not move, only the entry that names them
      changes, and when the paths point at different directories, the parent
      too. The rules are the ones every filesystem has: the source must exist,
      the target's directory must exist, the target name must be free (a rename
      is not a way to delete something by accident), and a directory cannot be
      moved into itself or below itself; that would cut a piece of the tree off
      from the root and leave it pointing in a circle.

      On top of that: a system call on both sides (system files are protected
      from BOTH sides: renaming a system file is as good as deleting it, and
      renaming something TO its name plants a foreign file where the system looks
      for its own), an `mv` command in the kernel shell, and an `mv` program that
      now tries the cheap way first and leaves copying as a second attempt,
      because "mv onto an existing file" is something people do.

      The file manager got a "Rename" button: it asks for the new name in the
      same bar where it says everything else. A dialog box would be a second
      window to move, focus and close, for one word.

      Tests: `pfs` got renaming and both refusals (the name is taken, the source
      does not exist), `files` asks and renames, and `gedit` does the same on the
      64-bit kernel, where the call has its own number and its own handler.

- [x] **A screenshot and an image viewer**: a machine that can photograph
      itself and look at the photo on its own.

      Print Screen saves the screen in the machine's own terms: four bytes
      "PSCR", the width, the height, the whole palette (256 colours of three
      bytes, so that the file says what its numbers mean) and one byte per pixel,
      a palette index. It is exactly what the compositor drew, so the viewer has
      nothing to interpret: it puts the bytes back into a window, and they are the
      same colours again.

      Reading the screen back: in eight-bit modes the buffer ALREADY holds
      indices, and it is a copy; in 24- and 32-bit modes it holds colours, and
      the way back is a search in the palette, exact, because everything on this
      screen was drawn from that palette. The search would be 256 comparisons per
      pixel done honestly every time; the screen consists of runs of one colour,
      so the last answer is remembered and is nearly always right again.

      A new call, win_blit(): a block of pixels straight into a window's content.
      Everything a program could draw so far was a rectangle of one colour or a
      line of text, and an image is neither; drawing it a rectangle per pixel
      would be three hundred thousand system calls per screenshot.

      The viewer shows every second (or third) pixel, because the whole screen
      does not fit into a window on the same screen. Nearest neighbour, because
      the point is to SEE what was on the screen, and a blurred half of a screen
      full of eight-pixel letters would show less, not more. Nothing is kept after
      loading: the window's content IS the copy.

      The file manager hands ".psc" to the viewer and everything else to the
      editor, by name, because looking inside would mean reading every file in
      the directory to draw a list.

      A new group, `screenshot` (9 assertions), checks that the file has exactly
      as many bytes as the screen plus the palette, and, more importantly, that
      the image in the window REALLY is the screen: an orange icon is found in the
      window in the same place and in the amount that showing every second pixel
      implies. No accident produces a number like that.

- [x] **An image from outside: BMP, and colours the screen does not have**,
      because the question "what about JPG, PNG and BMP" is hard not in the
      formats but in the colour.

      This screen is eight-bit with a palette: a window's content is indices,
      not colours. A .psc file works effortlessly because it IS the compositor's
      bytes. Every image from outside is RGB and has to be quantised to what the
      machine has: the desktop's thirty-six colours plus a hundred and ninety-two
      slots left by the wallpaper. A drawing comes out well, a photo comes out
      like a photo seen through thirty-six crayons, and that is what an eight-bit
      screen is, not a defect.

      So first the foundation: sys_palette() hands a program the whole palette,
      because without knowing what the indices mean no format makes sense.
      Searching for the nearest colour would be 256 comparisons per pixel, so the
      answer is remembered per colour rounded to five bits per channel: at most
      thirty-two thousand searches, and an image rarely uses a tenth of that.

      BMP: uncompressed, 24 bits or 8 with its own palette, rows padded to four
      bytes and, unless the height is negative, counted from the BOTTOM.

      How is such a file supposed to get here? There is no floppy and no sharing
      with the host, but there is a network: `httpget` downloads a file to the
      disk and the viewer shows it. That is how the test checks it too: the
      harness's server serves a BMP with four quarters in red, green, blue and
      white, because the palette has those four colours EXACTLY. A decoder that
      works shows exactly them, and a decoder that merely draws something shows
      something else.

      Next in line: PNG (inflate is some three hundred lines and will come in
      handy more than once), then baseline JPEG.

- [x] **PNG: an inflate of our own, chunks and five filters**: the second of
      the three formats from "what about JPG, PNG and BMP".

      PNG is three things in one coat: chunks, a deflate stream and one filter
      byte per row. Chunks are easy (a length, a name, the bytes, a checksum one
      does not need to look at, because the deflate stream will not come
      together anyway if the bytes are wrong).

      Deflate went into a separate file (user/uinflate.h), because it will come
      in handy more than once: three kinds of block (stored, with fixed trees,
      with trees written in the block itself, described by a third tree), and
      then a stream of symbols, where a symbol below 256 is a byte, 256 ends the
      block, and above that means "copy N bytes from D back", which is all of
      compression. Copying byte by byte is deliberate: a copy can overlap
      itself, because "three bytes from one back" means the same byte three
      times.

      Huffman decoding by plainly walking the bits, without a lookup table:
      thirty lines instead of a hundred and fifty, and the image loads in a
      moment anyway. Checked on the host against the real zlib, at four
      compression levels, including zero, which produces stored blocks.

      Filters are where decoders usually fail, so the image in the test has
      EVERY one of the five in turn (none, sub, up, average, paeth). An image with
      only zero filters says nothing about the other four.

      Interlaced PNG (Adam7) I cannot do, and I say so plainly instead of drawing
      a quarter of the image: an image shown wrong is worse than an image not
      shown.

      More patience in `proccores` (four readings instead of two, the best one
      counts) at the same threshold: the suite has heavier and heavier
      neighbours, and measuring parallelism on a host without a free core
      measures the host.

- [x] **Baseline JPEG: Huffman, the cosine and colour from half resolution**:
      the third and last of the formats from "what about JPG, PNG and BMP".

      BMP was reading bytes, PNG was unpacking a stream. JPEG is something else:
      the image IS NOT in this file. There are cosine transform coefficients,
      packed with Huffman and divided by quantisation tables, and the image has to
      be computed from them.

      In order: bits (with byte stuffing undone: 0xFF in the data is written as
      0xFF 0x00 so it does not pose as a marker), Huffman over the canonical
      tables from the header (mincode/maxcode/valptr, no tree: codes of the same
      length lie next to each other, so a comparison is enough), the zigzag back
      into an 8x8 square, multiplying by the quantisation table, and the inverse
      cosine.

      The cosine in fixed point, twelve fractional bits, and separated: eight
      columns, then eight rows. Separating it is the only reason it computes in
      reasonable time; done directly it would be 64 multiplications per pixel
      instead of sixteen.

      And at the end, colour: JPEG keeps luminance at full resolution and colour
      usually at half in both axes, because the eye does not see it. So the
      components are kept at their own sizes, a question about a pixel converts
      the coordinates, and only then does YCbCr go back to RGB.

      Checked against the Go decoder (image/jpeg) on its whole test set: thirteen
      baseline files (4:4:4, 4:4:0, 4:2:2, 4:2:0, 4:1:1, 4:1:0, with restart
      markers, greyscale, with 2x2 sampling on the only component, and with
      colours stored directly as R, G, B). The largest difference per channel: 4
      levels out of 256, and only on the rounding of the cosine. That is the kind
      of check a test of one's own cannot replace: a foreign file from a foreign
      encoder.

      On the way, three real bugs, all found by corrupting a file at random (900
      damaged versions, under ASan and UBSan): table numbers from the header were
      not checked (tq=66 reached past the quantisation tables), shifting a
      negative number left is undefined, and coefficients from a broken file went
      far enough out of range that the multiplications in the cosine overflowed
      an int. Hence two limits, on a coefficient and on the result of the first
      pass, chosen from measurements on real images (the largest coefficient
      ~1000, the largest intermediate result ~4200) and set ten times higher: they
      do not touch a correct image, and give a broken one meaningless pixels
      instead of undefined behaviour.

      On the way, both image decoders (inflate and JPEG) became a permanent part of
      `make fuzz`, next to certificates, HTML and frames. An image comes from the
      network exactly like a certificate, and on top of that this input decides
      HOW MUCH gets written; a one-off check while writing guards nothing later.

      Progressive JPEG I cannot do, and I say so plainly: that is a second decoder,
      not a variant of the same one. The same goes for CMYK and arithmetic coding.

      The test takes an image from a foreign encoder (Go, quality 90, 4:2:0),
      written into the harness as bytes, because an encoder of our own would write
      files for our own decoder, and that is not checking. The same four quarters
      as with BMP and PNG, read at the centres of the quarters, because a lossy
      format is allowed to blur the borders. Checked on both kernels: the decoder is
      one source compiled twice, and in long mode a pointer has a different size
      and indexing a plane goes through a long.
- [x] **Colours: no more 256-colour palette where the screen has more**: the
      answer to "what does it take to have as many colours as modern
      computers".

      First, what did not need doing: the screen buffer ALREADY was 32-bit.
      video.c has handled 24 and 32 bits per pixel for a long time, and
      video_adopt_lfb takes the buffer from GRUB or UEFI, so on real hardware it
      works. The ceiling was not the hardware but ourselves: everything above
      video_putpixel spoke in palette indices.

      **The heap.** The first real blocker had nothing to do with colour. A
      window buffer is one byte per pixel; in colour, four. A 420x300 window
      grows from 123 KiB to 492 KiB, and the kernel heap had ONE MEGABYTE: two
      windows and the heap is gone. So the heap is now computed from the
      detected memory: a sixteenth, between 1 and 8 MiB. A machine with 128 MiB
      gets 8 MiB, and the 16 MiB machines the swap tests run on get exactly the
      megabyte they had; we do not take half their memory along the way.

      **An 8-bit palette.** video_set_palette took 6-bit components (0..63), a
      legacy of the VGA DAC, which takes that many. Now it takes 0..255, and the
      narrowing stays where it belongs: in one line writing to the DAC. The
      constants in desktop.c were converted with the exact formula (v<<2)|(v>>4),
      i.e. the way the palette itself used to convert them; the look did not move
      by a single level, as the `look`, `wall` and `icons` groups confirmed. That
      is a change that lifts a ceiling and touches nothing.

      **A second drawing path.** video_putpixel_rgb, video_fill_rect_rgb and
      video_blit_rgb take 0x00RRGGBB. In 8-bit mode (mode 13h, the fallback) a
      colour is swapped for the nearest index, remembering the last answer,
      because an image is made of runs of one colour.

      **A window with a format.** A window is born with indices, because that is
      how every program on this machine drew, and none had to change. A program
      that has something to show asks once (win_truecolour) and from then on
      writes colours through win_blit32. Its own frame (the background, the status
      bar) still goes through win_rect and win_text in indices, because those calls
      TRANSLATE an index into a colour for such a window. Thanks to that gimg
      changed three loops, not the whole file.

      Swapping the buffer under a window is safe not because of a new lock but
      because of the one already there: the syscall takes the big kernel lock, and
      the compositor holds it during the whole drawing. The same principle has
      protected resize_content for a long time.

      An interesting side effect: a window in colour is composed FASTER than one in
      indices; its pixels are already what the screen buffer wants, so it goes row
      by row instead of pixel by pixel.

      **gimg.** The nearest() function and the table of 32768 remembered answers are
      gone. A photo is a photo, not a photo seen through thirty-six crayons. .psc
      files stay as indices: their bytes ARE indices of this palette, so win_blit
      puts them back exactly.

      **The wallpaper.** The format got a second shape: "PWL2", the size, three
      bytes per pixel, no palette. The old "PWAL" is still read. And here one has
      to be honest: for four of mkwall's five patterns this changes NOTHING; a slope
      over 120 rows has 120 colours whatever one does. The change pays for itself
      with a fifth pattern: a blend along both axes where every pixel has its own
      colour, nineteen thousand of them, which a 192-colour palette cannot carry.

      **gweb got nothing**, and I say so plainly: its HTML parser does not read
      colours at all; it draws with six constants. Rewriting those six constants as
      RGB would not change a single pixel. Real colours in the browser mean a CSS
      parser, which is a different job.

      **Presenting only what changed.** The compositor redraws the whole frame
      thirty times a second whether anything moved or not, and until now the whole
      frame went to the card: 1.2 MB at 640x480, 3 MB at 1024x768. Writing to the
      card's buffer is MUCH more expensive than writing to memory, so a third buffer
      was added: a copy in RAM of what last went to the screen. Comparing RAM with
      RAM says which rows really changed, and only those cross the bus. Reading the
      card's buffer back would cost more than the writes it saved, hence a copy and
      not a comparison with the screen.

      Measured, not estimated: a command shows how many rows went out in the last
      frame. An idle desktop: ZERO. With the clock ticking on the bar: six rows. The
      test guards that.

      And the bug this optimisation brought, because it did bring one. The
      framebuffer console (fbcon) keeps the address of the VISIBLE buffer and writes
      straight to it, knowing nothing about the compositor's working buffer. As long
      as the whole frame went to the screen, it did not matter: the next frame
      covered the text anyway. With row comparison it does. The compositor drew the
      right thing, compared it with a copy that already matched, and sent nothing;
      the kernel's text stayed on the screen forever. The `gterm64` group fell over
      on it, because its OCR read those leftovers as the window's content.

      The fix is where the problem is: whoever writes to the visible buffer behind
      video.c's back says so through video_front_touched(), and the next frame goes
      out whole. My first attempt put that in video_console_cell() and it was
      useless: vga.c calls fbcon_cell directly, bypassing that function. So the hook
      sits in fbcon.c itself.

      The `iso` group asks about it directly: the desktop stands on the loader's
      buffer, the kernel shell runs `mem`, and afterwards there has to be as much
      background as before. That is an assertion that passes without the bug and
      fails with it.

      What this still does not give: it is a framebuffer without a GPU. No
      acceleration, the mode is set for us by GRUB or UEFI, one monitor. A graphics
      card driver is a project the size of the whole system so far, and not where I
      would go.

      A new group, `colours`, counts what cannot be faked: the desktop drawn in
      indices has 29 different colours on the whole screen, and a JPEG gradient
      shown in a window has 3031, growing to the right in red and downwards in
      green, so it is not "many colours" but "the right colours".

- [x] **A laptop: the touchpad, the wheel and the panel's own resolution**,
      because "I will test the system on a laptop first".

      **A bug that on a laptop would have been visible from the first second.**
      mouse.c drew "M x,y bN" in the corner of the screen on EVERY mouse packet, and
      pit.c drew "uptime: Ns" every second. In text mode that was diagnostics. Under
      UEFI there is no text mode (vga_write_at goes through fbcon straight to the
      VISIBLE buffer), so the mouse driver painted over the desktop, from an
      interrupt, several hundred times a second. It showed in a screenshot as text
      hanging over the wallpaper with no window around it.

      The fix: the compositor says vga_screen_taken(1) when it takes the screen,
      and the corner of the text screen stops being a place. Three callers at once
      (the mouse, uptime, the demo threads in kmain.c), and all of them write to the
      serial log anyway, where diagnostics belong.

      On the way: without this, every finger movement on the touchpad invalidated
      the screen copy (because the text went to the visible buffer), so the
      optimisation from the previous entry stopped working exactly when it is most
      needed.

      **The wheel, i.e. two fingers.** A touchpad on the PS/2 port is a mouse: it
      reports relative movement and buttons exactly the same way, and its own
      firmware turns a tap on the pad into a click. What a 1987 mouse did not have
      and a touchpad does is a wheel, and that is how it reports two fingers sliding
      on the pad. It is the IntelliMouse extension: a fourth byte that has to be
      asked for with a knock nobody sends by accident (three sample rates, 200, 100
      and 80), after which the device says what it is. An answer of 3 means the
      fourth byte is there.

      The sign is the opposite of what it seems: the protocol counts a notch
      TOWARDS the person as positive, and the rest of the system calls the direction
      AWAY from the person positive, because that is the way the document moves. The
      flip sits in the driver, so that nobody above has to remember it; it showed up
      in a test that showed a list moving the wrong way.

      On the way, the packet parser: it was a switch with a label and a jump; now it
      is an array and a counter.

      **WEV_SCROLL goes to the window UNDER THE POINTER**, not to the focused one.
      Every other system does it that way, and it is the right way: one looks at a
      list and scrolls it, without first making it the keyboard's window. gweb,
      gfiles and gedit handle it.

      In the editor something came up to think about: draw() called
      scroll_to_caret() every frame, so a scroll would be undone at once. Hence
      follow_caret: the wheel lets go of the cursor, the first key catches it again.
      Scrolling a document is looking elsewhere, not moving there.

      **The resolution: the kernel spoiled it for itself.** boot/boot.S asked the
      loader for 1024x768. A request in the Multiboot 2 header WINS over
      gfxpayload, so a 1920x1080 panel got a quarter of itself. The 64-bit header
      had zero ("any") there for a long time, with a comment explaining why; the
      32-bit one did not, and that was the whole source of the small desktop.
      Measured, not guessed: with the request, 1024x768; without it, whatever the
      loader gives. Likewise grub.cfg got the `insmod all_video` and
      `gfxpayload=keep` that the 64-bit disc had and the 32-bit one did not.

      **Window buffers moved off the heap to the PMM.** A window buffer is the
      largest single thing this system allocates: at a laptop's native resolution a
      maximised window in colour is eight megabytes, i.e. THE WHOLE heap. Keeping
      the largest and smallest objects on one first-fit list is a recipe for
      fragmentation. Now they come in whole pages straight from the frame
      allocator, and the size is a matter of the panel, not of a number written in
      the code.

      **And something that was not in the plan.** 1920x1080 worked at once, and was
      UNREADABLE. The whole UI stands in absolute pixels, the font is eight pixels,
      and eight pixels on a 15-inch 1080p panel are a millimetre and a half. A system
      that is correct and useless.

      The answer: the compositor draws on its OWN, logical screen, and that screen
      goes to the panel with every pixel laid down as a square of four. video_w()
      and video_h() answer in logical pixels, and not one line of layout anywhere
      had to change. In a machine where every window, icon and status bar stands in
      absolute coordinates, anything else would mean rewriting all of them, and with
      them the geometry in dozens of test groups.

      It has to be said plainly what this costs: it is pixel doubling, not scaling
      the elements. The letters are the same 8x8 letters drawn twice as large, not
      sharper. A photo on such a screen has half the detail the panel could carry.
      In exchange: zero changes above and zero risk.

      The scale is a setting, and zero means "let the screen decide": a panel 900
      pixels tall or more gets 2, everything smaller 1, because doubling 640x480 would
      give a desktop 320 pixels across, and that is not a desktop. Thanks to that all
      existing tests stay at scale 1, and nothing changes in them.

      The `scale` group asks about the mechanism at a scale forced by hand, and asks
      honestly: not only whether neighbouring pixels are equal in pairs (11100 out of
      11100), but also whether the same grid shifted by a pixel NO LONGER matches
      (8590 out of 11100). The first number without the second would mean nothing.

- [x] **A USB mouse, and a machine with no PS/2 at all**: the second half of the
      answer to "on a laptop first".

      The pointer has come through the 8042 controller since the first day, and on
      a desktop computer it still does. A laptop from recent years may have no 8042
      at all: no PS/2 keyboard and no PS/2 touchpad, because it has no such ports.
      Its pointer is a HID device on the USB bus, and so is its keyboard.

      So a mouse driver next to the keyboard driver, the same path (the ports the
      controller layer reports, the configuration descriptor, a HID class interface
      in its boot version), and they part on ONE byte: the interface protocol is 1
      for a keyboard and 2 for a mouse. The boot report has three bytes, sometimes
      four: the buttons, movement in X, movement in Y and, on a device with a wheel,
      the number of notches. The movement bytes are SIGNED and have to be read that
      way: a movement of minus one arrives as 0xFF, and taken as 255 it would throw
      the pointer across the whole screen.

      The shared pieces (building the eight bytes of a request and the two kinds of
      control transfer) moved out of usbkbd.c into usbhc.c. They sat there as long
      as the keyboard was the only device; a second one would have copied them, and
      two copies of a wire format are how formats drift apart.

      **A trap I would have set myself.** Both drivers walk the same ports, and each
      stops at the first device it recognises, looking at all the earlier ones on the
      way. And looking means RESETTING the port. The second pass would knock over the
      keyboard the first one had just started. So a port is greeted once and its
      handle remembered: the second asker gets the same device, not a device just
      reset under its owner.

      Movement from both paths ends in mouse_inject(), because there is one pointer
      on the screen and the compositor has nothing to choose between. The wheel's
      sign in HID is already the one the rest of the system uses, unlike the PS/2
      packet, which has to be flipped.

      **Two bugs on the way, both mine, both caught by tests.**

      First: usbhc_init() started the controller unconditionally, and starting means
      a RESET. The second driver, asking for the controller, wiped the slot and
      endpoint the first one was already using, and the USB keyboard was found,
      reported and silent. Now a controller found once answers at once.

      Second, subtler: even without resetting the port, the second driver READ the
      descriptors of a device the first one was already talking to: two
      conversations over one wire. Hence usbhc_claim(): a driver that took a device
      says so, and from then on the port answers "nothing here" to everyone else.

      Neither is visible in code that compiles; both came out only from running the
      `usbkbd` group. That is exactly the kind of bug these tests exist for.

      The `usbmouse` group asks two things, and the second is the one that counts:
      that the machine finds the mouse on the xHCI controller and says where, and
      that with the 8042 DISABLED the desktop comes up and its cursor follows the
      mouse. The question has to be asked in pixels, not words, because a machine
      without an 8042 has no PS/2 keyboard either, so there is no way to TYPE
      anything. Whatever moved that cursor came over USB, because there is no other
      way there.
- [x] **The keyboard went silent when a mouse joined, and a hardware report for
      the first boot on someone else's machine.**

      The laptop turned out to be a Dell. The good news: Dell keeps the built-in
      keyboard on the i8042 even in new models. The touchpad is a lottery that
      depends on the year: older ones are PS/2, newer ones from around 2016 sit on
      I2C-HID, which we cannot do. So instead of guessing which machine it is, the
      machine had to be made to SAY it itself.

      **The first lie to remove.** mouse_init() never checked whether anything was
      on the auxiliary port at all: mouse_ack() read a byte and threw it away. A
      machine without a PS/2 mouse looked exactly like one with a mouse: every
      command "worked", the interrupt handler came in, and the pointer did not move
      and nobody said why. Now a missing 0xFA acknowledgement ends the matter, and
      the boot says so plainly. The same with wait_write/wait_read: on a machine
      without an 8042 every port read gives 0xFF, and 0xFF has that bit set;
      without a check we waited the full number of spins for every byte of every
      command and called it success.

      Second: the report said "keyboard yes" without any basis. That is exactly the
      same mistake I blamed the mouse for. Now it asks about the controller itself:
      a machine that does not have one answers 0xFF to everything, and that is a
      classic and honest test.

      **The `hardware` command**, written for the first boot on hardware nobody had
      tried. The screen (where the buffer came from, what scale), the PS/2 ports,
      what was found on USB, and a list of PCI devices. That last one is there for a
      reason: a touchpad that is not on PS/2 usually sits on an I2C controller, and an
      I2C controller is a serial bus class device visible in that list. We cannot do
      that bus, but being able to SAY that it is the one is the difference between a
      puzzle and a known gap.

      **And a bug this work uncovered, in the xHCI driver.** It was written for
      exactly one device, and nobody noticed, because until today there was one. One
      buffer for the whole controller: two devices have two reads in flight at once,
      so one's report lands in the place the other is reading from. And one event
      ring: whoever looks first sees SOMEBODY ELSE's completions, and taking one that
      is not yours loses that report forever. The symptom: the keyboard went silent
      the moment a mouse joined the bus, i.e. exactly on a laptop with a keyboard and
      a touchpad over USB.

      The fix: a buffer per slot, and an event put aside under its slot (the slot
      number is in the top byte of the control word, and only that is why two devices
      can share one ring at all).

      On the way I broke it twice, and each time the tests said so, not the
      compiler. First sort_events swallowed the COMMAND COMPLETION events the
      enumeration was waiting for, hence a separate queue for whatever is not a
      transfer. Then it turned out that a slot needs a QUEUE, not one place: a control
      transfer completes twice, once for the data and once for the status, and with a
      single place the second overwrote the first and the caller waited forever for
      something it had already been told. The third time I went back to the committed
      state, confirmed it worked, and only then did it once and properly.

- [x] **A disk that is not ours is not ours to take**, and this is an entry that
      should have been written long ago.

      The question was "what about an installer for this system". I started by
      checking what an installer could even be here, and on the way I came across
      this:

          if (memcmp(sb->magic, MAGIC, 4) != 0)
              format_disk();

      fs_init() checked on EVERY boot whether the first block said PFS3, and if it did
      not, it formatted the disk. Without asking, without looking at what was there.

      That was harmless as long as every disk this system had seen was an empty file
      made by a test. It stops being harmless the moment it meets a real machine: a
      laptop's disk does not say PFS3 either. Starting this from a stick on the Dell
      would have wiped the partition table, Windows and everything else in the first
      second of the first boot.

      I saw it not by reading, but because my test image with a GPT came back as
      "no partition table": the kernel had wiped it before I got to read it.

      Now only a REALLY empty disk is formatted. "Empty" is answered carefully,
      because the cost of a mistake one way is a new disk a person formats by hand,
      and the other way, somebody's data: a partition table signature in sector zero
      disqualifies at once, and otherwise the first sixty-four kilobytes must be zeros
      throughout. Everything else is left untouched and said plainly, and whoever
      really wants that disk says so with the `format` command.

      The confirmation is a word typed in full, YES in capitals, because a key to
      press is something a person does by reflex, while a word has to be decided on.

      **Reading the partition table** (kernel/part.c) came about on this occasion and
      is the foundation for all the rest: MBR and GPT, where detecting the latter is
      one byte. Type 0xEE in sector zero means "look at sector one". GPT keeps a
      protective entry there covering the whole disk precisely so that a program that
      knows only the old format sees a full disk instead of an empty one and does not
      kindly offer to partition it. Nothing in that file writes.

      The `disk` group asks about restraint and what it consists of: an image with a
      real GPT goes into the boot and comes out of it with the same table, read BACK ON
      THE HOST SIDE, byte by byte, not taken on the system's word. An empty disk is
      still formatted. A wrong answer to `format` erases nothing, and a typed YES
      erases, and then the table is gone, because that was what was asked for.

      **PFS in a partition**: the second step, done at once, because without it the
      first is only a refusal. The offset lives in TWO functions that touch the disk
      and nowhere else, so no code above has to know whether it stands on the whole
      disk or a piece of it. `format N` takes one partition (the table and the
      neighbours stay as they were), and on the next boot the kernel finds its PFS by
      itself, looking for a partition whose first block says PFS3. No registry of
      GUIDs, no new agreements: the system recognises itself by what it already writes.

      The test checks something that is easy to fake. Not only whether the table
      survived (that is 34 sectors) but whether the NEIGHBOURING PARTITIONS still have
      their data, because those are hundreds of megabytes and they are what really
      matters. A marker written before, read after, on the host side.

      What this is NOT: an installer. For the machine to start from this partition,
      writing to the EFI partition is still missing (i.e. FAT, which we do not have),
      and so is an EFI boot program. Those are the next steps, not this one.

- [x] **protoboot (today threadboot): a UEFI boot loader of our own**, stages 1-3.

      Until today this system was started by GRUB. GRUB is excellent and it is
      SOMEBODY ELSE'S; everything else here is written from scratch, and the thing
      that puts the kernel into memory was the last piece that was not.

      **Stage 1 removed the biggest risk**, and that was its only point: I did not know
      whether this toolchain would build an EFI application at all. An EFI application
      is a PE32+ file with the Microsoft calling convention, two things GCC on Linux
      does not do. clang targeting x86_64-unknown-windows plus lld-link does them;
      `file` says "PE32+ executable (EFI application)", and the firmware loaded and ran
      it. Two kilobytes, without gnu-efi and without EDK2. The only foreign thing is the
      file format.

      **The UEFI declarations are ours** (boot/efi/efi.h), and two things have to be
      done right with them. The ORDER of members: the firmware hands over tables of
      function pointers, and we index by position; a skipped entry shifts everything
      after it and the machine jumps into the wrong function. That is why every entry of
      the boot services table is there, in order, even those we never call. And the
      CALLING CONVENTION, which the choice of compiler takes care of, so not a single
      line has to repeat it.

      **Stages 2 and 3**: reading the kernel and the initrd, the graphics mode, ACPI. We
      do NOT NEED a FAT of our own to boot: the firmware has already mounted the
      partition we were started from and provides a file protocol. FAT will be needed to
      WRITE such a partition, i.e. when installing, not here. The file size is taken by
      seeking to the end and asking where one is, which saves EFI_FILE_INFO, a GUID and a
      buffer for an answer that is one number.

      The graphics mode is taken as the firmware already shows it: on a laptop that is
      the panel's native resolution, exactly what we want, and asking for a specific mode
      would throw it away. It is the same lesson as with the Multiboot 2 header: asking
      for something specific wins and spoils things.

      The RSDP from the firmware's configuration table, because on a UEFI machine there
      is no other way: the old search through low memory finds nothing there.

      Measured under OVMF: kernel 825028 B, initrd 448132 B, to the byte as large as the
      files, the screen 1280x800, RSDP version 2.0.

      **The loader loads the 64-bit kernel and only that.** At first it reached for the
      32-bit kernel, which was thoughtless: a UEFI machine is a 64-bit machine. The 32-bit
      kernel exists in this project so that one set of sources is compiled twice and the
      second compilation tells what the first one hid. That is a good reason to keep it,
      and no reason to run it on a laptop. Letting it in here would mean an ELF32 loader
      next to the ELF64 one for a case nobody has; generality nobody asked for is how a
      loader stops being trustworthy. GRUB still starts that kernel for whoever needs it.

      On the way, two things this established: the 64-bit kernel is ELF64 (not ELF32, as
      I had assumed when planning the loading), and the kernel takes the initrd module BY
      POSITION, not by name, so the loader has to pass one module and that is it.

      **What is still missing**: building the Multiboot 2 tag table, loading the ELF,
      ExitBootServices and the jump. That last one is the awkwardness of the whole
      undertaking: a UEFI application runs in long mode, and a Multiboot 2 kernel enters
      in 32-bit protected mode, so after saying goodbye to the firmware one has to leave
      long mode oneself, on one's own tables, with paging turned off.

      The shape of "telling the kernel" is already fixed and is not ours to change: the
      kernel is a Multiboot 2 image and wants a magic number in EAX and a tag table in
      EBX. So the loader BUILDS that table, exactly as GRUB does, and the kernel will not
      find out that anything changed. Not one of its lines had to.

- [x] **protoboot: stage 4, the kernel starts from our loader** (the
      `threadboot` test group).

      Stages 1-3 read files and asked the firmware about the screen. Stage 4 is what all
      of it was for: build the Multiboot 2 tag table, load the ELF, say goodbye to the
      firmware and jump. From the moment ExitBootServices returns, there is nobody left
      to help: the firmware console no longer exists, so any mistake in this sequence is
      a machine that goes dark without a word.

      **The tag table is built for what the kernel actually reads**, not for the whole
      specification. kernel/mb2.c recognises seven types: end, command line, module,
      memory map, framebuffer and two forms of the RSDP. The loader provides exactly
      those. Providing the rest would be code nobody reads, and so code nobody checks.

      **The UEFI memory map has a trap, and it is the most important thing in this
      stage.** The firmware returns a map of its regions; translating it to Multiboot 2
      is a mapping of types. It is tempting to treat EfiLoaderCode and EfiLoaderData as
      free: the loader is about to stop existing, after all. Except that IN THAT VERY
      MEMORY lies the initrd the loader read from the disk. Handed over as free, those
      pages would go to the kernel's allocator and be given to the first one who asks,
      and the initrd would turn into garbage before anyone opened it. So they go into the
      map as RESERVED. The test guards this not by looking at the map but by running a
      program from the initrd: if somebody gave those pages away, the program will not
      start.

      **Leaving long mode.** A UEFI application runs in long mode, and a Multiboot 2
      kernel enters in 32-bit protected mode. The order is forced and leaves no room for
      ideas of one's own: a GDT of our own (the firmware's disappears with the firmware),
      a far jump to a 32-bit segment, turning off paging in CR0, clearing EFER.LME,
      turning off PAE in CR4, and only then a jump to the entry point with EAX and EBX
      set the way GRUB sets them. The addresses in this sequence are already physical,
      because UEFI identity-maps memory; if it did not, this step could not be done in
      place.

      **It worked the first time, and that is not a reason for pride but the result of
      the shape having been fixed beforehand.** The kernel found out about nothing: the
      same magic number, the same table, the same module. Not one line of the kernel
      changed for this loader, and that is the measure of whether the interface was good.

      **The test group** starts a machine on which there is NO GRUB: an EFI partition
      with three files and the OVMF firmware. The sizes of the kernel and the initrd are
      checked to the byte against the files on the disk (the loader has to read them, not
      say it read them), the resolution from GOP is compared with the one the kernel saw
      and with the one in the screenshot. The BOOTX64.EFI file itself is read byte by
      byte: PE32+, machine 0x8664, subsystem 10, because only such a file will the
      firmware run at all.

      **What is still missing**: writing FAT, and an installer. The loader can already
      start the system from a stick prepared on the host; for ProtonOS to prepare one
      itself (next to Windows, on a laptop's disk) it has to be able to WRITE the EFI
      partition. That is the next step.
- [x] **FAT12/16/32, the only foreign filesystem in this project**
      (kernel/fat.c, kernel/fatcmd.c, the `fat` test group).

      This system keeps its files in PFS3 and has no reason to want anything
      else, except in one place. The EFI partition is FAT by definition: the
      firmware of every UEFI machine can read FAT and cannot read anything else,
      so a boot program kept anywhere else will never be started. Installing this
      system next to another one therefore means WRITING to a partition that
      already exists, already holds someone else's loader, and has to keep
      working.

      Hence the shape of the driver. Reading is complete. Writing does what an
      installer needs (a directory, a file, its contents) and nothing more:
      nothing here formats, nothing moves other people's files, nothing rewrites
      a structure it did not have to touch. Every write goes to ALL copies of the
      allocation table, because the system that starts this machine tomorrow may
      read any of them.

      **Which of the three variants it is, nothing in the boot sector says.** The
      NUMBER OF CLUSTERS says it, computed from the sizes. That is not malice in
      the specification but the only thing about FAT that is really true; reading
      the name from the string in the boot sector, which is where everyone's first
      bug lives, gets it wrong on volumes that announce "FAT16   " and are not.

      **FAT12 is not a curiosity.** Its entry has twelve bits, so every other one
      straddles the boundary of a table sector. A driver written for whole entries
      breaks exactly there. That is why all access to the table (walking a chain,
      allocating, freeing) goes through SINGLE BYTES: a byte straddles nothing.

      **Long names are read and written**, and that is not politeness. The loader
      looks for 'protonos64.bin', which does not fit in eight characters and a
      dot; bending the name to the filesystem's limits would be letting the
      filesystem decide what this system's files are called. A name that DOES fit
      in 8.3 gets one ordinary entry, exactly what anyone else would write, with
      two bits in the byte at position twelve that remember it was written in
      lower case.

      **What is on the other side of every assertion.** The FAT images in the
      tests are made by mtools, and mtools reads back everything the system wrote
      to them: the directory, the long name together with its short name
      (PROTON~1.BIN), the file contents, the disappearance of a deleted file. A
      driver that agrees only with itself has proved nothing, least of all on a
      partition that belongs to the system that starts this machine. What must not
      be done is checked separately: a foreign bootmgfw.efi on the same partition,
      the GPT and the neighbouring Windows partition are read from the host after
      everything and must be untouched.

      **fatcopy is an installer in miniature.** It copies a file from the initrd
      to the EFI partition, and installing this system is exactly that: the
      loader, the kernel and the initrd are files the running system already has.
      The command came before the installer because it is the part that can be
      checked on its own, to the byte, by a program that is not ours. The test
      copies the shared library (83 272 B) and compares it with the contents of
      the initrd.

      **The commands are in a separate file** (kernel/fatcmd.c), because both
      shells, 32- and 64-bit, should have them, and neither should have its own
      copy.

- [x] **The installer: ProtonOS onto the machine's disk** (kernel/install.c,
      the `install` test group).

      On a UEFI machine installing is smaller than the word suggests: three files
      written to the EFI partition that is already there. The firmware looks for a
      boot program on it and starts it; that is the whole mechanism. So nothing
      here creates a partition, nothing touches the table, and nothing deletes or
      moves any file belonging to the system that already lives on that disk.

      **Where those three files come from: that is the heart of it.** The kernel
      running right now is loaded segments, not the file it came from, and what
      has to be written is a file. So the loader passes all three as Multiboot 2
      modules: the initrd as it always did, and the kernel image and ITSELF, by
      name. A system started otherwise (QEMU with -kernel, GRUB from a disc not
      made for this) has nothing to install and says so plainly, instead of
      writing something approximate.

      Modules other than the initrd are taken BY NAME, not by position. The
      alternative is an agreement between the loader and the kernel about the
      order, the kind of agreement that breaks quietly.

      **Two paths, and the difference between them is the whole ethics of this
      command.** /EFI/ProtonOS is ours and we always write it.
      /EFI/BOOT/BOOTX64.EFI, the name the firmware falls back to when nothing
      else is set, we write ONLY IF IT IS NOT THERE. On a machine that has it, it
      belongs to whoever starts that machine today; taking it would be installing
      IN PLACE OF someone rather than NEXT TO them. Then the way in is an entry in
      the firmware's menu, and saying so is better than a machine that quietly
      stops starting what it used to start.

      The confirmation is a word typed on the keyboard, the same as with `format`
      and for the same reason: a key to press is a reflex, a word is a decision,
      and on the other side is somebody's laptop.

      **The test closes the loop.** The machine boots from a stick (the EFI
      partition given to QEMU as a directory), and the disk of "the other machine"
      hangs on the AHCI controller (so it is the one the system picks) and has a
      GPT, a FAT made by mtools and a foreign bootmgfw.efi on it. After installing,
      each of the three files is read from the image with mtools and compared to
      the byte, and the foreign file, the table and the neighbouring partition must
      be untouched. Then THIS DISK IS BOOTED ON ITS OWN, without the stick, and
      that is the only proof that the installation installed anything. Checked
      separately: without typing YES nothing happens; and a second disk, one that
      already has a foreign /EFI/BOOT/BOOTX64.EFI, where that file must be the same
      byte for byte afterwards.

      On the way, two fixes in the loader: it now loads files with
      AllocateMaxAddress below four gigabytes (a Multiboot 2 module address has
      thirty-two bits and nowhere to write a bigger one; until now that was a
      silent assumption), and when looking for the kernel and the initrd it also
      looks in \EFI\ProtonOS, which is where the installer puts them.

- [x] **The installer has a screen of its own, not a window on the desktop**
      (user/ginstal.c, kiosk mode in kernel/desktop.c, the `ginstal` test group).

      It was first made as an ordinary window among other windows, and that was
      wrong. Installing is not one task among others: while someone's disk is
      being written, a taskbar and other windows within reach are not a
      convenience but a way to click something else halfway through the write. So
      `install` takes the WHOLE screen.

      **Kiosk mode** is the same compositor with everything else taken away: no top
      bar, no taskbar, no icons, no menu, no desktops and no second window. One
      program in the middle of an empty screen, and the only thing the environment
      itself offers: restarting the machine in the top right corner. The window
      does not even have close, minimise and maximise buttons: there is nowhere to
      minimise it to, nothing to enlarge it into and nothing behind it, so they
      were three lies.

      The kiosk restarts the program when it ends; a kiosk with an empty screen
      looks like a broken machine. But if the program dies at once, after three
      attempts the kiosk stops and says so; flickering forever was easier to write
      and worse to watch.

      **The installer is no longer in the desktop menu**, and that is the whole
      difference between what was and what is.

      **The program knows nothing about installing.** It asks the kernel what the
      disk looks like (sys_install_look, which reads and changes nothing, so anyone
      may call it), and asks the kernel for one step at a time (sys_install_step;
      every step requires an admin, because on the other side is the disk of a
      machine that will boot without us). Deciding what installing means lives in
      one place, and that place is not the window.

      One step per loop iteration, not five at once: the kernel would write
      everything in one call and the window would freeze for the duration of the
      write. This way the bar moves, and under it stands the name of the file going
      to the disk right now; a person watching their disk being written has a right
      to see where it is.

      **A word, not a button.** A button is a reflex, and reflexes are how other
      people's disks get written. The window asks for YES to be typed, just like
      the console version. Then the kernel refuses a program that is not an admin,
      and then, only then, the window asks for the password; the same shape as in
      gsettings and for the same reason: a refusal is not the same as an error.

      **Every line lies on a twelve-pixel grid.** Not for tidiness: the font is
      eight pixels tall, and text spaced by eye (one line at twenty, another at
      fifty) is text only a person can read. A grid is text a program can read too.
      The size column stands at x=6+36*8 for the same reason, and not at 296,
      because 296 is not divisible by eight.

      The test reads the screen in the same font the kernel drew it in (a new
      `Qemu.read_text` in ptest.py; screen_fbcon is now a special case of it,
      because the console is not the only thing written in that font). It checks
      both halves: that on this screen there is NO taskbar, top bar or icons, and
      that a click in the corner where the desktop has 'Menu' opens nothing; and
      that everything the program says can be read from the screen. At the end, as
      in the `install` group, the files are read from the image with mtools to the
      byte, and the installed disk is booted on its own.

      Two small things the test revealed: a mouse cursor drawn over a letter turns
      it into a character the font does not have (the test moves the pointer away,
      as a person would), and a window's title bar starts one pixel from the frame,
      so the content is at win->y + TITLE_H + 1, not where I guessed.

      The console version stays as `install text`. Not out of sentiment: when there
      is nothing to draw on, or when the answer has to end up in a log, it is the
      only version that works.

- [x] **Contrast: dark text on light, light text on dark**, in the whole GUI, not
      only in the installer.

      This system's windows have a light grey background (227,227,227), and every
      program wrote on it in WHITE. It looked like a working program (everything was
      drawn) and could not be read. That is the worst kind of interface bug: nothing
      is broken, it is just that nobody sees anything.

      The rule is one, and it is the whole solution: **dark text on a light
      background, light text on a dark one**. So black on the grey window, white on
      the blue accent (buttons, the selected row, the title bar), white on the black
      input fields, white on the dark desktop background. Dark grey (73,73,73) for
      what is said more quietly (a file's size next to its name), and blue for
      headings, because it is the same accent the desktop uses.

      Green (0,255,0) disappeared from light surfaces, because on grey 227 it is worse
      than white; where it served to tell things apart (a directory from a file, a
      kernel thread from a program), the accent or the dark grey replaced it. The
      yellow links in gweb are now blue; links have been blue since links existed,
      and yellow on a light background is not a colour, it is an absence of colour.

      Fixed: ginstal, gtasks, gfiles, gweb, guiapp, gclock, gmon, gsettings (two
      places; the rest already knew, there is even a comment there, "Black on grey
      reads; white on grey does not") and the counter next to the 'Click' button in
      the compositor itself. gedit, gterm and gimg were right from the start: gedit
      has a white page and black text, gterm a black background and white text, gimg
      a status bar in the accent.

      A few lines that were meant to be separators were also white on grey, i.e.
      invisible. They are now in the line colour (150,150,150), the same the
      compositor uses.

- [x] **The whole disk: a partition table and a FAT32 of our own**
      (kernel/gpt.c, fat_mkfs in kernel/fat.c, the `wholedisk` test group).

      Until now installing meant three files on someone else's EFI partition. Now
      there is a second way, a disk made anew, and the difference between them is
      the whole point of this step. Beside: the table stays, other partitions stay,
      every file on the EFI partition stays. Whole disk: a new table, a new EFI
      partition, a partition for ProtonOS, and everything that was there is gone.

      There is no third way, and there will not be one that shrinks somebody else's
      partition to make room. Moving another system's data on the strength of a guess
      about what it is, is not something one does.

      **The GPT is written for what the firmware checks, not for what is enough for
      us.** A protective MBR in sector zero, the header in sector one, 128 entries
      after it, and the same entries plus a second header at the very end of the
      disk, backwards. The last 34 sectors of a GPT disk are not free space but a
      copy, thanks to which a damaged beginning can be repaired. Both headers carry a
      CRC of themselves and a CRC of the whole entry table. A mistake in those sums
      does not show loudly: the firmware quietly decides the table is damaged, and
      the machine does not start, without saying why. That is why the test reads the
      table its own way, in Python, the way the firmware reads it: with both sums and
      a comparison of the copy with the original.

      The entries go to the disk BEFORE the headers that point at them, and the
      protective MBR at the very end: as long as it is not there, sector zero keeps
      saying "empty" to anyone who reads it. That is the order in case the power goes
      in the middle.

      **Our partition's GUID is ours.** The bytes spell PROTONOS-PFS3, so a person
      looking at the table with a hex editor sees whose it is, and no other system
      will take it for its own. Borrowing Linux's GUID would be a lie someone would
      read one day.

      **fat_mkfs makes FAT32, and FAT32 is not a flag one sets.** It is a cluster
      count above 65524, exactly the same rule mounting uses. So the cluster size is
      chosen so that the count lands where it has to, and a partition too small for
      that is refused, not formatted as something else and called FAT32. The EFI
      partition is 128 MiB: much more than three files need, and the least at which
      FAT32 is comfortably FAT32.

      On the way: **the "32" in FAT32 has nothing to do with a 32-bit processor.** It
      is the width of the cluster number in an allocation table entry: FAT12 has
      twelve-bit entries, FAT16 sixteen-bit, FAT32 thirty-two-bit (28 of them used).
      It decides how large a volume can be, not what it is read on. The EFI partition
      of a 64-bit laptop with Windows is FAT32 too; there is no FAT64.

      **The word is different.** Installing beside another system asks for YES. This
      asks for ERASE, because YES is what one says to a question, and to this one
      says something else.

      The test checks the whole path: an old disk with its own valid table and a
      foreign bootmgfw.efi; a plan that says plainly what will disappear; a word other
      than YES; no change when it is not typed; after typing it, the table read in
      Python with both sums, two partitions with the right GUIDs, names and bounds,
      FAT32 read by mtools, three files to the byte and, finally, BOOTING THAT DISK.
      Separately, what this mode does NOT do: it does not zero the disk byte by byte,
      because no installer does that. What disappears is the shape (the table and the
      filesystems), not every byte, and the test says so plainly instead of claiming
      something that would not be true.
- [x] **Several disks and a choice between them** (kernel/blk.c as a list,
      ata.c master and slave, ahci.c every port, the `disks` test group).

      The block layer found one disk, and that was the disk. For a machine with
      one, that is right, and it stops being right the moment someone has to
      choose which one to install onto.

      So there is a list. The order still goes from newer to older (NVMe, then
      every AHCI port with a disk, then both drives of the old ATA bus), and the
      first on the list is the one in use, so a machine with one disk behaves
      exactly as it did before. That is not excessive caution: the whole
      filesystem, swapping and the partition table lie under this layer.

      Above the layer nothing changed: the filesystem, part.c and fat.c read and
      write THE DISK IN USE. Threading a device number through each of them would
      be a change in each of them, for a question asked once, of a person, in the
      installer.

      **LOOKING MUST NOT CHANGE ANYTHING**, and that is the only subtle thing in
      this step. Reading another disk's table requires pointing the block layer at
      it, because part.c reads the disk in use. But a machine left pointing at a
      disk someone only asked about is a filesystem reading someone else's
      sectors. So install_look() remembers what was in use and goes back there
      before returning, together with unmounting the FAT it mounted on the way.
      Pointing at the chosen disk is install_step()'s job, i.e. the moment when
      writing really happens.

      **The test found a real bug on the way, and an old one.** The `disk` command
      wrote a pattern to the LAST SECTOR of the disk and read it back, which was
      safe as long as our disks had nothing on them. On a disk with a GPT, the last
      sector IS the copy of the partition table. Writing there destroys it without
      a word: the disk still boots, and one day a repair tool says the table is
      damaged. Now a disk with a table is only read, and there is an assertion that
      holds it.

      The text version takes a disk number (`install text whole 2`) and prints the
      list when there is more than one disk. In the window, the disk line is
      clickable and says so plainly; a choice one has to guess at is not a choice.

- [x] **Closing the path to real hardware: `make stick`, `hardware` and `disks`
      in the 64-bit kernel** (scripts/mkstick.py, kernel/hwinfo.c, the `stick`
      test group).

      This whole chain (loader, FAT, installer, partitions, disk choice) exists to
      put ProtonOS on a real laptop. And not a single byte of it had run on real
      hardware: everything was checked under QEMU and OVMF. Between "works in the
      emulator" and "boots from a stick" three things were missing, and all three
      were trivial, which is the worst kind of missing thing.

      **There was no command that makes a stick.** `make iso64` makes a disc with
      GRUB, i.e. exactly what our loader does not use. To prepare a stick one had
      to assemble a GPT by hand, format an ESP and copy three files, i.e. repeat
      from memory what the system can do itself. Now `make stick` produces a raw
      image for dd, and `make run-stick` boots it under OVMF before it goes onto a
      stick.

      The table in the image is written byte by byte in Python, for the same reason
      kernel/gpt.c exists: the firmware checks the sums and the copy at the end of
      the disk and quietly decides a table is damaged. The FAT is made by mformat,
      i.e. by somebody else. That the stick is made by one implementation and read
      by another is worth more than both of them being ours.

      The image is 192 MiB with one partition. The three files take a megabyte and
      a half; the rest is there so that FAT32 is FAT32 (the cluster count) and so
      that there is room to add something.

      **`hardware` did not exist in the 64-bit kernel.** I wrote that command
      precisely for the first boot on real hardware (what is on PS/2, what on USB,
      what PCI sees, what screen the loader gave), and it sat in the 32-bit shell,
      i.e. in a kernel nobody will run on a laptop. It is the command one types
      first on a machine nobody has tried yet, and it was not where it is needed.

      It landed in kernel/hwinfo.c, shared by both shells, together with a new
      command, **`disks`**: which disks the machine has, which one it uses and what
      is on each of them, without going into the installer. Looking at another disk
      points the block layer at it, so here too it is pointed back, the same rule as
      in install_look().

      The `stick` test reads the image the way the firmware will read it: the table
      in Python with both sums, the FAT through mtools, three files to the byte, and
      then it BOOTS it and types both new commands.

- [x] **Sound: Intel HD Audio** (kernel/hda.c, the `beep` command, the `sound`
      test group).

      The only class of device this system did not have. A machine with a TCP
      stack, TLS 1.3 and a browser that cannot make a single sound is strangely
      incomplete.

      **HDA is not a device one writes samples to**, and that is the whole
      difficulty. There are TWO things in the machine. The controller sits on PCI
      and fetches bytes from memory itself: it gets a list of buffers (the BDL) and
      walks it round and round; from that moment it is the one reading, and the
      driver only has to keep up filling the half it is not in at the moment. The
      codec sits behind the controller on a serial link and is a GRAPH: numbered
      widgets (converters, jacks, mixers) one has to WALK to find out which converter
      reaches which jack. Two machines with the same controller can have completely
      different graphs.

      **Three bugs on the way, all of the kind where the device simply stays
      silent.**

      The first and most expensive: **RINTCNT set to one**. It is the count of
      responses after which the controller decides we are not keeping up, and it
      STOPS executing commands until the flag it raised is cleared. With a one, a
      response is given to exactly one question, and every next one waits for an
      interrupt we do not take. The symptom was perfectly misleading: the first
      question (about the codec's vendor) gave a correct answer, so the rings
      "worked", and the second gave nothing. It showed only after printing CORBRP:
      the read pointer stood at one while the write pointer moved on. The count is
      now pushed out, and the status is cleared after every response.

      The second: **verbs come in two lengths, and the length decides the
      position**. A four-bit one leaves sixteen bits for the argument, a twelve-bit
      one eight. Writing the short one already shifted (0x200 instead of 0x2) looks
      innocent and sends a command the codec answers with nothing. They are now
      written as the numbers they are, and one condition classifies them.

      The third came out only of the sound file: **the tone lasted 243 ms instead of
      200**. The controller walks the buffer round and round, so a half that is not
      overwritten gets played a second time. After the samples run out, filling has
      to continue (with silence) through the whole buffer, and only then can the
      stream be stopped. That is the difference between a tone of the requested
      length and a tone with a stutter at the end.

      **The test does not ask the system whether it played.** A driver that sets
      every register and produces silence looks exactly like a working one. So QEMU
      writes its audio output to a file, and the file is read here: the peak, the
      length of the loud part and the distance between zero crossings. 440 Hz for
      200 ms must come out as 440 Hz for 200 ms, and 880 for 100 as that; one number
      would be a coincidence, two are a driver. It comes out as 441 Hz / 200 ms and
      882 Hz / 100 ms. The frequency is computed as the MEDIAN of the gaps between
      crossings, not their count over the whole: one crossing missed at the edge
      shifts the count and does not shift the median.

      Separately, a machine WITHOUT a sound card: it must say plainly what is
      missing, and `beep` on it must refuse instead of pretending. That is also why
      the driver remembers WHERE it stopped: "there is no HDA controller on PCI" and
      "the codec has no output I can find" are two different things, and on real
      hardware that makes all the difference.

- [x] **A mixer: volume, mute and a window for them** (hda_set_volume in
      kernel/hda.c, the `volume` command, the user/gmikser.c program, the
      extended `sound` group).

      **A field that is easy to read backwards.** A widget's amplifier is
      described by parameter 0x12, and in it the NUMBER OF STEPS sits in bits 14..8
      and the SIZE of a step in 22..16. I read them the other way round, and it did
      not show as an error: the codec reported three steps instead of seventy-four,
      the mixer got three positions, and the only sign was that quiet and loud
      sounded the same. It showed only in the sound file: a peak of 314 at full
      volume, while before (with an accidentally larger number) it came out at 5679.

      A percentage is a percentage of what THIS widget has, because there is no
      other range. The steps are decibels, so thirty percent is much quieter than a
      third, and that cannot be fixed; it is how hearing works. The window says so
      plainly instead of pretending a linear scale.

      **And a bug that came out only through the mixer and is about the whole
      kernel.** After playing a tone from the window, the mouse stopped moving. The
      reason: in long mode a system call enters with interrupts DISABLED (the
      processor clears the flag on entry, SFMASK). So a driver spinning in a loop for
      two hundred milliseconds left the machine deaf for two hundred milliseconds:
      no clock, and a PS/2 mouse that lost the middle of a packet and never found the
      framing again. The waits in hda.c now SLEEP instead of spinning, and sleeping
      gives the core to the scheduler, i.e. to where interrupts come back. This
      concerns every long system call in this kernel, and this driver was the first
      to make one.

      **The slider reaches a hundred percent**, because it divides by the width
      MINUS ONE. A slider that cannot be pulled all the way feels broken, and the
      difference is one character in the code.

      The volume is remembered like the desktop background and the machine name,
      and it is the only setting a ring 3 program may save without asking for a
      password. The reason is simple: volume is a knob, not a change to the system.

      The test measures what came out of the card: 100% gives a peak of 8000, 30%
      gives 2353, 5% gives 314, and mute gives zero. Separately the same clicked in
      the window: the slider to the end, the 'Test' button, a measurement; the slider
      to a fifth, another measurement, and it has to be quieter.

- [x] **AES, GCM and CCM, a cipher that was not here before** (user/uaes.h, the
      user/aestest.c program, the scripts/aesref.c reference, the `aes` test group).

      All of this system's cryptography stood on ChaCha20 until now, chosen because
      it computes with additions, rotations and XORs, so it can be written without a
      single table. AES is the ticket to two places ChaCha does not reach: the TLS
      cipher suites some servers offer exclusively, and WPA2, because a wireless frame
      is encrypted with AES-CCM and nothing else.

      The cipher itself is FIPS-197 copied directly: one substitution table (256
      bytes) and its inverse, and MixColumns computed, not read from four kilobytes of
      tables. Slower, but every line can be read next to the specification. **It is
      not an implementation resistant to timing measurements** (a table read depends
      on the data), and that is written plainly in the header instead of pretending
      otherwise.

      **The thing that is easiest to get almost right.** AES with a mistake in the key
      expansion encrypts, decrypts, agrees with itself perfectly, and agrees with
      nobody in the world. That is why one line in the key expansion (an extra
      substitution every fourth word for a 256-bit key) has a comment above it saying
      that without it, exactly such an AES comes out.

      **The GCM counter is not the CTR counter.** In CTR mode the whole sixteen-byte
      block is incremented, in GCM only the last FOUR bytes; the rest of the block is
      the nonce and must not be touched. They are two functions, although they look
      like one.

      **CCM decrypts before it checks**, because its tag covers the PLAINTEXT, not the
      ciphertext (the opposite of GCM). So with a bad tag, the decrypted content is
      already in the caller's buffer, and that is why it is zeroed there. Handing over
      something unchecked would be worse than handing over nothing, and the test guards
      that separately.

      Checked from three sides, because none of them alone is enough:

        1. official vectors: FIPS-197 (three key lengths), SP 800-38A for CTR, test
           case 4 from the GCM specification and packet no. 1 from RFC 3610 (exactly
           the shape of WPA2: a thirteen-byte nonce, an eight-byte tag);
        2. against OpenSSL: the system encrypts its own material and PRINTS everything
           it used, and the host encrypts the same through libcrypto
           (scripts/aesref.c, because `openssl enc` cannot do authenticated modes) and
           compares byte for byte;
        3. the other way round: OpenSSL encrypts, the file comes into the system over
           HTTP, and the system has to decrypt it, and has to REFUSE the same file with
           one flipped bit in the tag.

      The third way is the most important: that our encryption agrees with our
      decryption says nothing about either of them. The test also checks that the
      material is not degenerate; a key made of one repeated byte agrees with
      everything.

      The same vectors go separately through the 32-bit kernel. Not because we develop
      it, but because a second compilation of the same sources is the cheapest method I
      know for catching a shift, a width or a pointer that happened to be the right size.
- [x] **WPA2: keys and frame encryption, everything except the radio**
      (user/usha1.h, user/uwpa.h, AES key wrap in user/uaes.h, the
      user/wpatest.c program, the `wpa` test group).

      This is where an honest boundary runs. There is no radio in this system
      and QEMU emulates no wireless card, but everything a station does with the
      password, the handshake and the frame can be written and checked right now.
      The driver for a specific chip cannot.

      **SHA-1 is here for one reason and one only.** It has been unfit for
      signatures for years, and it is not used for certificates. WPA2 gives no
      choice: the network password is turned into a key with PBKDF2-HMAC-SHA1
      and 4096 rounds, and the four-way handshake derives the session keys with a
      function built from HMAC-SHA1. The collisions that finished SHA-1 off do not
      touch any of these uses.

      The PBKDF2 salt is the NETWORK NAME, and that is why two networks with the
      same name and the same password have the same key, while it cannot be
      computed once for all networks. Four thousand rounds are meant to hurt:
      they are the only thing standing between a short password and a dictionary.

      **The CCMP additional data is the whole thing.** The tag covers the header,
      but with the fields that legitimately change on the way masked out: retry,
      power save, more-data, the sequence number. The addresses stay covered.
      Thanks to that a repeater can retransmit a frame while an attacker cannot
      redirect it, and the test checks exactly that: changing one bit in the A3
      address must end with the frame being dropped, with the data untouched.

      Checked with the vectors from P802.11i/D7.0 (I.7.4): real frames with a
      header, a packet number and a tag, including one with QoS and a 26-byte
      header. That is the only kind of vector that catches badly assembled
      additional data: CCMP with such a mistake agrees with itself and with no
      access point in the world.

      hashlib checks the keys: the system derives the PMK, the PTK and the
      EAPOL-Key message tag from a seed, prints everything it used, and the host
      computes the same with its own PBKDF2 and HMAC. On top of that RFC 3394
      (wrapping the group key) with the official vector and a refusal after one
      bit is changed, because the constant A6A6A6A6A6A6A6A6 is there so that a
      wrong key gives a refusal rather than garbage that looks like a key.

      What this does NOT cover: the driver. A card requires uploaded firmware and
      its own protocol on PCI, different between Intel, Broadcom and Realtek, and
      writing it blind, without a device that answers, is the same as writing
      without any test. That is left for when there is hardware, and then only the
      driver will be left, because all the cryptography will be ready and checked.

- [x] **TLS with AES-128-GCM, not only ChaCha20** (user/utls.h, the extended
      `tls64` group).

      The first gain from AES that shows at once. Until now the client offered one
      cipher suite and that was it, and some servers, especially embedded and
      hardware ones, offer nothing but AES-GCM. Now the ClientHello carries two
      suites, the server chooses, and the difference between them ends at the
      record: the key schedule, the transcript and the whole handshake are the
      same, the key is just sixteen bytes instead of thirty-two.

      ChaCha20 stays first in our order of preference, because it needs no
      substitution tables, so it has nothing that could leak the key through the
      cache. AES is there so that there is something to talk to where there is no
      choice.

      **A test that does not allow itself "it probably works".** Until now
      "connected" said nothing about which cipher was used, because there was one.
      Now there are two servers, each agreeing to EXACTLY one suite (openssl
      s_server -ciphersuites, because Python's ssl module cannot narrow the TLS 1.3
      suites), and breaking off the handshake for any other. The answer that comes
      back is therefore proof of what the record was encrypted with, separately for
      AES-GCM and for ChaCha20-Poly1305.

- [x] **A disk in a USB socket: a stick like any other disk** (bulk transfers in
      kernel/xhci.c, the kernel/usbmass.c driver, FAT from a chosen disk, a new
      `usbdisk` test group).

      The USB bus has carried a keyboard and a mouse so far, devices that answer a
      question with a few bytes. A disk is a second kind of traffic: bulk
      endpoints, data both ways and a SCSI command in every transfer. The class is
      called Bulk-Only Transport and every stick of the last twenty years speaks
      it: thirty-one bytes of command (CBW), the data, thirteen bytes of verdict
      (CSW).

      **A bug that turned up by itself and was more serious than all the rest.**
      The test booted the same machine through UEFI, and USB disappeared.
      Completely: "USB: none", although the controller was on PCI. The reason: UEFI
      firmware gives the controller a memory window ABOVE 4 GiB, and the driver
      refused such an address with one condition written when the kernel was only
      32-bit. In long mode there is no reason to refuse, and now it does not, while
      in 32-bit mode it says plainly why it cannot. **On a laptop booting through
      UEFI this meant no USB keyboard and no USB mouse**, i.e. everything a new
      laptop has. No test caught it, because the USB tests booted from a disc
      through BIOS.

      Hence a second change too: a controller that does not come up RECORDS where
      it stopped ("no xHCI controller on PCI", "the controller sits above 4 GiB",
      "it will not reset"), and `hardware` shows it. "USB: none" is not an answer
      anyone can do anything with.

      **FAT from a CHOSEN disk.** A stick is for looking at, not for moving the
      whole machine onto, so `fat 1 3` mounts a partition from the third disk, and
      PFS keeps working on its own. Every read and write of that filesystem points
      the block layer at the right disk and puts it back, exactly as when looking at
      other disks in `disks`.

      The driver adds itself to blk.c as an ordinary disk, but at the end of the
      list: a machine that has a disk of its own should boot from it, whatever is
      stuck in the socket.

      Checked so that someone else reads the result: mtools writes a file to the
      stick image and the system has to read it, and then the system writes its own
      and mtools has to find it, together with the host's file having survived.
      Finally, what a stick is for in the first place: **installing the system onto
      a stick from the running system** (`install text whole 3`), checking the GPT
      with our own reader, the loader to the byte through mtools, and booting that
      stick under UEFI, all the way to the shell.

- [x] **Bluetooth: an HCI conversation with the adapter** (kernel/bt.c, the
      `bt`, `bt scan` and `bt test` commands, the `bt` test group).

      Every Bluetooth adapter, whatever is inside it, speaks the same language:
      HCI. Commands go down, events come up, and the packets have the same shape
      whether the adapter sits on USB, a serial port or SDIO. In a laptop it sits on
      USB, internally, on the same bus as the webcam, so this driver talks through
      the USB layer that was just built for the stick. Commands do not go through
      any endpoint: they are a control transfer with request type 0x20 and
      everything else zeroed. It has been that way since 2001.

      **And here runs a boundary that has to be named.** QEMU emulates no Bluetooth
      adapter (the whole stack was thrown out of it years ago), and no other
      hypervisor has one either. So not a single line of this conversation has ever
      reached a device that answers. What is checked is what is pure bytes:
      assembling commands, reading events, and framing an HCI ACL packet, against
      recorded packets shaped as in the standard. `bt test` says so itself at the
      end: this checks packet handling, not the conversation with an adapter.

      What such a test catches all the same: an address that goes over the wire
      least significant byte first and on the screen most significant first (read
      backwards it is valid and unrecognisable); a manufacturer that stands at byte
      eight of the Read Local Version answer, not seven; a device name pulled out of
      an LE advertisement, where it is one of several pieces with their own length
      and type. Those are exactly the mistakes that are otherwise found on a laptop,
      one restart apiece.

      The second thing the test guards: **honesty on a machine without an
      adapter**. `bt` has to say there is no adapter and why, and `bt scan` has to
      refuse, not show an empty list of devices as if it were looking around. An
      empty list and no adapter are two different things, and only one of them is
      true.

      The adapter's two bulk endpoints are now discovered and configured, and the
      kernel can frame and send/receive HCI ACL packets on them. What is not here
      yet is L2CAP, link security, pairing, or the HID profiles needed to use a
      keyboard or mouse. The packet test checks framing only; the USB wire still
      needs an adapter to validate it. Intel and Broadcom adapters may also need
      firmware before they do anything beyond reporting their own address.

- [x] **A stick plugged in while running, and pulled out** (a thread watching the
      sockets in kernel/usbhc.c, disks added and struck off in kernel/blk.c, the
      extended `usbdisk` group).

      Enumerating devices at boot answers the question "what was plugged in when we
      started". A machine someone is using has a quite different question: what is
      plugged in NOW. The answer sits in the same bit (the controller keeps the state
      of every socket in a register), so noticing a stick costs one read, not a
      mechanism.

      The shape is the reverse of boot: there each driver walked the sockets itself;
      here one thread watches the sockets and the drivers report what they can
      recognise. It has to be the reverse, because now it is the SOCKET that changes,
      not the driver that asks.

      **Disk numbers must not shift.** Someone looks at `disks` and is about to type
      `install text whole 2`. So a disk that disappears stays in its place with the
      note "pulled out of its socket", and a new one is added at the end. A read from
      a pulled-out disk ends in a refusal, not garbage, and the same applies to the
      filesystem: a FAT mounted from a pulled-out disk stops being mounted WITHOUT
      writing anything, because there is nowhere to write. A filesystem that answers
      from memory after its disk disappeared is a way to overwrite somebody else's
      data.

      The slot goes back to the controller on every removal. Without that, a machine
      in which someone plugged and pulled a stick four times has no slots left, and the
      fifth time it is simply not seen, which looks like a broken socket.

      **Two threads on one controller, and that needs a lock.** Until now enumeration
      finished before the keyboard thread started. Now a stick can be greeted while the
      keyboard is being read. Putting a TRB into the ring and taking an event off are
      short; that is what a spinlock is for. A whole COMMAND is not short: it waits
      for an answer, and two commands at once would find each other's completions,
      because an event says which ring it came from, not whose it was. So commands go
      one after another.

- [x] **FAT32 on a small volume stops posing as FAT16** (kernel/fat.c, a new case
      in the `fat` group).

      Found through a stick: 32 MiB formatted by mtools as FAT32, and the system
      showed an empty directory on a full disk. The FAT type follows from the cluster
      count (that is the rule, and we keep to it), but a formatter TOLD to make FAT32
      makes it even on a volume too small to need it. Such a volume says so plainly:
      the sixteen-bit "sectors per FAT" is zero while the thirty-two-bit one is not,
      and there is no fixed-size root directory. Read by the cluster count alone it
      looks like FAT16, and everything after that is garbage.

      The proof that it really is FAT32 comes in the test from mtools ("sectors per
      fat: 0"), not from us, because our own rule was exactly what was being checked
      here.

- [x] **USB hubs: what is in a docking station** (the route string in
      kernel/xhci.c, sockets growing in kernel/usbhc.c, the extended `xhci` and
      `usbdisk` groups).

      A device behind a hub is on NO port of the controller, so walking the
      controller's ports does not find it at all. That is exactly what a docking
      station looked like from this side: empty.

      So a socket stopped meaning the same as a controller port. The list of sockets
      starts with the controller's ports and GROWS: every hub found appends its own at
      the end. A driver walking "the ports" thereby reaches behind hubs without knowing
      anything about them, and that is the point.

      The hub itself is handled with ordinary control transfers: ask for the class
      descriptor (type 0x29, the only place where the number of ports stands), power
      every port, wait as long as the hub asked for (a port not yet powered answers
      "nothing here", and the device behind it is never found), then a port reset and
      reading its status, which also holds two speed bits.

      **The controller then has to be told the ROUTE.** The route string: four bits for
      each tier of hubs, the first tier in the lowest bits. On top of that, two fields
      that are easy to overlook: a device slower than the hub above it travels through
      that hub's transaction translator, so the controller has to know WHICH hub and
      which of its ports is responsible. Without that the device gets an address and
      stays silent, and that is something one stares at for a long time before seeing
      it. And the hub itself has to be marked as a hub in its context, or else the
      controller thinks it has no ports, and a command about a route through it refers
      to a route that does not exist for it.

      Checked with two devices behind a hub: a keyboard (and what is typed on it has to
      reach the shell) and a stick (it has to be on the list of disks like any other
      and be readable). What is not there yet: noticing devices plugged into a hub while
      running. Asking the hub about every port twice a second would cost more than it is
      worth, and that is written plainly instead of a comment pretending otherwise.
- [x] **Plugging into a hub, and an interval that means something other than it
      seems** (kernel/usbhc.c, kernel/xhci.c, the extended `usbdisk` group).

      A stick plugged into the docking station, not into the machine. A hub has an
      endpoint whose whole job is to say "something changed on port N", and that
      is how it is watched, instead of asking about every port twice a second. The
      answer is a bitmap: bit one is port one.

      **A bug that hid behind a working keyboard.** The interval in the descriptor
      and the interval in the endpoint context are NOT the same number. A
      high-speed descriptor gives an exponent and a full-speed one milliseconds,
      and the hub asks for two hundred and fifty-five. Written in without
      conversion it gives 2^255 microframes, i.e. polling rarer than the lifetime
      of the machine: an endpoint that never answers. The keyboard worked despite
      the same bug, because it asked for eight, and that came out as thirty-two
      milliseconds instead of eight: enough for nobody to notice. The conversion
      is now the one in the standard and depends on the device's speed, so the
      controller remembers the speed of every slot.

      **And a second one, one state too far.** The thread watching the sockets
      remembers what was plugged in, to know what changed. A hub socket that was
      looked at during boot and turned out empty was remembered as OCCUPIED,
      because "looked at" gets confused with "something is in it" only if the two
      are confused in the code. The effect: the first insertion was seen, and the
      next ones were not.

      Every change the hub reports is now acknowledged, all the flags, not only the
      one we came for. An unacknowledged flag is reported over and over, and the
      endpoint says "something changed" forever, twice a second, about something
      that changed once.

- [x] **A network card on USB (RNDIS)** (kernel/usbnet.c, netdev.c, a new
      `usbnet` test group).

      The second half of what sits in a docking station. It differs from the two
      cards this system already knows in a way that goes deeper than registers:
      there are no registers. Everything is a conversation ("start yourself",
      "what is your address", "pass me broadcasts") carried through endpoint zero
      as "encapsulated commands", and every frame gets a forty-four-byte header in
      front of it.

      The protocol is RNDIS, i.e. NDIS (the Windows driver interface) stretched down
      a wire. That is why its messages ask about "OIDs" instead of hardware: an OID
      is a number naming a property, and 0x01010101 happens to mean "your permanent
      Ethernet address".

      **The thing that cost the most time fits in one field.** In a query without
      data, the OFFSET of that data must be zero, not the twenty a message WITH data
      carries. The card checks whether the offset points inside the message, and a
      twenty-eight-byte query whose data starts at byte twenty-eight is a query it
      refuses to answer. From outside it looks exactly like a card that does not
      work.

      **And a structural thing: nothing can be done from an interrupt here.** A PCI
      card raises an interrupt and the handler reads the ring; a USB card has to be
      ASKED, and asking is a transfer, and a transfer must not be done in an
      interrupt handler. So frames are collected by a thread that spends its life
      waiting on a bulk read, and that is what the thread is for. On the way, waiting
      in the xHCI driver stopped being pure spinning: after a few hundred spins it
      hands the machine to the scheduler, because a driver spinning for five seconds
      waiting for a packet that may never come stops the whole machine.

      The USB card is last on the list of attempts in netdev.c: a machine with a card
      of its own stays with it, and a machine without any can reach the network
      through the docking station. The test sets up a machine WITHOUT a PCI card and
      checks the whole way up: an address from DHCP, an answer to a ping, and a page
      fetched over TCP from a server on the host, from ring 3.

- [x] **Widgets: what every program wrote anew** (user/ugui.h, the mixer rewritten
      on that layer).

      Every graphical program in this system had the same thing: a rectangle drawn
      where a button is meant to be, a label on it, and then arithmetic in the event
      loop (is the click within these coordinates, is it the first button or the
      second, which row of the list is (y - 20) / 12). Six programs, six copies of the
      same arithmetic and six places where a button can land a pixel away from where
      the click is expected.

      So a window becomes a LIST OF THINGS instead of a series of drawing calls. The
      program says what it has (a label here, a button there, a slider from zero to a
      hundred), and the layer draws, decides what a click hit, keeps the keyboard focus
      and reports "the third one was pressed".

      What it deliberately is NOT: it does not take over the loop, it does not keep the
      program's data, and it lays nothing out by itself. The program gives the
      positions, in pixels, because that is what every window here already does, and a
      layout engine would be a second framework on top of the first.

      **The proof that the rewrite changed nothing comes from outside.** The mixer test
      clicks the slider and the buttons and then MEASURES what came out of the sound
      card into a WAV file: a hundred percent gives a peak of 8000, a fifth 1506, mute
      zero. The same test passes on the rewritten window with the same numbers. A
      framework that compiles and a framework that works differ in exactly that.

- [x] **A question in the window instead of "click again"** (ug_modal in
      user/ugui.h, the file manager asks before deleting).

      "Are you sure?" is what every program with a destructive button needs, and until
      now each one made it up for itself: gfiles asked by making the SECOND click on
      the same button mean something different from the first. That works, and nobody
      will guess it.

      The panel is drawn IN the window, not as a second window. A program whose
      question has its own window has to keep both alive, move them together and
      answer for both when the desktop asks to close; a panel over the content gets all
      of that for free and, on top of that, cannot be lost behind something else. While
      it is there, it takes every event, and that is the difference between a question
      and a suggestion.

      On the way the test got an assertion that did not exist before and could not be
      written: **"Keep" has to keep the file**. A question whose both answers mean yes
      is not a question.

- [x] **Choosing a file from a list instead of typing a name blind** (ug_pick in
      user/ugui.h, Ctrl+O in the editor).

      The editor asked "Open:" and waited for a name. That works if one remembers what
      is on the disk, and otherwise it is guesswork: the only place in the graphical
      system where a person was worse off than in the shell, because the shell at least
      completes names.

      Now there is a list of what exists, a line to type a name that does not exist yet,
      and two buttons. Clicking a row puts the name into the line, so whichever way one
      gets there, the same line is opened, and that is why the old way of using it (type
      and Enter) still works, which the test checks separately from the new one.

      **An afternoon went on a bug that was not in the system.** The test stopped
      finding the file manager, and the cause was one line I had added to the test:
      moving the pointer out of the way to (700, 420) on a 640x480 screen. The mouse
      driver clamps the pointer to the edge, and the test kept assuming it was where it
      had asked, so from then on every click landed sixty pixels off. A press on an icon
      was therefore not a press on the icon, and nothing started. It showed only after
      adding output to the kernel saying which icon had been released.

- [x] **The system prepared for release on GitHub as open source** (LICENSE,
      CONTRIBUTING.md, .github/, screenshots in docs/, README).

      The code was on GitHub, but it was not open in any useful sense: without a
      licence nobody has the right to use it (no licence does not mean "do what you
      like" but "all rights reserved"), without written rules every submitted change
      was a lottery, and without CI the first such change would only be checked on my
      computer.

      **The MIT licence**: the shortest of those that allow everything and require
      nothing beyond keeping the name.

      **CONTRIBUTING.md** writes down the one rule that really governs this
      repository: **every feature brings a test that would not pass without it**, and
      the test has to check the system from outside, through the screen, the serial
      port, mtools reading our FAT, OpenSSL computing our ciphers, firmware that boots
      from the disk or does not. It also has what someone from outside would have to
      guess: why the code is in English while the system spoke Polish; why there are two
      kernels from one source; and never to write `time.sleep(3)` and read the screen
      once; half the flaky tests this project ever had were exactly that.

      **CI in GitHub Actions** is split: every push builds both kernels, the boot
      loader, the disc and the stick image, and then runs a dozen groups that touch
      most of the system (booting both kernels, ring 3, PFS, FAT, AES, WPA, USB, sound,
      UEFI). The whole suite (ninety-five groups and about a hundred virtual machines)
      runs once a day, because a GitHub runner has two cores and no KVM, so a full run
      would take longer than anyone wants to wait on a pull request. On failure it
      uploads the serial logs as an artifact, because without them a remote failure is
      just a red cross.

      **The screenshots** are made by a script that starts the 64-bit kernel in QEMU,
      clicks through the system and dumps the framebuffer, the same machinery as the
      tests, so the picture shows what the kernel really drew, not a mock-up. Two of
      them: the desktop with the file manager, the clock and the memory monitor, and the
      installer on the full screen, at the moment it has listed what it will write and
      has not yet written anything.

      On the way it turned out that the README lied in its numbers: "nearly 800
      assertions" and "50 groups" were true several dozen commits ago. Now there are
      more than 1600 assertions and 95 groups.

      **The first CI run measured what I feared.** The set of a dozen groups took a
      GitHub runner **six minutes**: two cores, no KVM. If that is all, the list was too
      short: the groups easiest to break by accident were added (the desktop, the file
      manager, the installer, the ring 3 shell, TLS 1.3, the network, the stick). A full
      local run after these changes: **1714 assertions, zero failures, 95 groups,
      1229 s**.

- [x] **The first CI failure was a failure of the environment, not of the system**
      (port 53 in scripts/ptest.py).

      The extended set of groups fell over on `tls64` after a second, with a
      `PermissionError` at `bind(("0.0.0.0", 53))`. The network groups set up **their
      own DNS server on the host's port 53**, because that is exactly where QEMU's user
      network sends the guest's questions (10.0.2.2:53), and it cannot be remapped:
      `guestfwd` handles TCP, not UDP. My tests run as root, so it did not show; a
      GitHub runner is not root.

      Two things at once. First, `start_dns_server` now returns `(stop, ok)` instead of
      blowing up: a group that has nothing to check without names says **what it is
      giving up and why**, with a ready recipe
      (`sudo sysctl -w net.ipv4.ip_unprivileged_port_start=53`). The stack trace looked
      like a system bug and was a missing permission, and that is exactly the difference
      a test is supposed to show.

      Second, CI got that one sysctl line before the tests, because **giving up
      assertions is a loss of coverage**, not a solution. A green CI that quietly does
      not check TLS would be worse than a red one.

      Both paths checked locally: the normal one (as root, four network groups pass) and
      the refusal (port 53 taken by another process; the group prints SKIPPED and ends
      green).

      Confirmation from outside: the CI run after this change is green, **559 assertions
      instead of 539**; those twenty are exactly `tls64`, which used to fall over after a
      second. Nine minutes on a two-core runner.

---

- [x] **The whole system speaks English now** (both kernels, every ring 3
      program, protoboot, the command names, the settings, the tests and
      the harness).

      The repository went open two commits ago, and a system that says
      `instaluj: nie ma dysku` is a wall for everyone who does not read
      Polish. Not only the messages: the commands themselves were
      Polish, and a person who cannot guess that `dyski` lists the disks
      cannot use the machine at all.

      What changed, in order:

      **The system.** Every `printk`, every window label, every icon,
      the installer's screen and protoboot's four lines. About nine
      hundred strings.

      **The vocabulary.** `kim -> whoami`, `wyloguj -> logout`,
      `haslo -> passwd`, `ekran -> screen`, `glosnosc -> volume`,
      `sprzet -> hardware`, `dyski -> disks`, `formatuj -> format`,
      `instaluj -> install`, `pulpit -> desktop`; the settings keys
      `nazwa/kolor/tlo/tapeta/pasek/ikony/skala/glosnosc/haslo` became
      `name/color/background/wallpaper/taskbar/icons/scale/volume/password`.
      The words a person types before a disk is erased changed too:
      `TAK -> YES`, `KASUJ -> ERASE`.

      **The tests.** Every assertion description, and - the part that
      actually matters - every string the tests EXPECT to see. Those are
      the same sentences the kernel prints, so the two had to move
      together or nothing would pass. Twelve test groups had Polish
      names as well (`dysk`, `zrzut`, `kolko`, `instaluj`, `caly`,
      `dyski`, `dzwiek`, `usbdysk`, `usbsiec`, `kolory`, `skala`,
      `usbmysz`); they are now `foreigndisk`, `screenshot`, `wheel`,
      `install`, `wholedisk`, `disks`, `sound`, `usbdisk`, `usbnet`,
      `colours`, `scale`, `usbmouse`, and `scripts/run_tests.py`, the CI
      workflow and the documentation know the new names.

      **Two bugs the translation itself made**, both worth writing down:

      `hello` printed a counter by patching a digit into a fixed string
      at index 15 - which was the digit's place in `[hello] licze: 0`
      and lands in the middle of a word in `[hello] counting: 0`. The
      fix is not a new number but `sizeof(counter_msg) - 3`: the digit
      is the last character before the newline, whatever the sentence
      says. A constant that depends on the length of a message is a trap
      set for the next person who edits the message.

      And a whole test run was wasted because I edited `ptest.py` while
      the suite was running. That is the second time; the rule stands
      and is in CONTRIBUTING: nothing gets rebuilt or edited while a run
      is in flight.

      The journal itself stayed Polish at that point; its early entries
      were translated later (see the introduction at the top).


- [x] **ProtonOS is ThreadOS**

      The system is called ThreadOS now, down to the magic on the disk.
      The loader is threadboot, the shared library libthread.so, the
      filesystem TFS with "TFS3" in its superblock, and the type GUID of
      our partition spells THREADOS-TFS3 in plain bytes. Our own file
      magics followed: TIRD (initrd), TSCR (screenshot), TWL2
      (wallpaper).

      That is a break on purpose: a disk formatted under the old name is
      not ours any more, and the system leaves it alone - which is what
      the foreigndisk group exists to guarantee. The machine's name and
      default password were the word 'proton' and are 'thread'; the
      password is FNV-1a over the system's name as a salt and the word,
      so the stored number was recomputed - after reproducing the old
      number with the same arithmetic first, to know the new one is
      right and not merely different.

      Almost every name kept its length (ProtonOS/ThreadOS, PFS/TFS,
      libproton/libthread), so no column or index moved. What broke was
      eight assertions, every one a place where the name was data rather
      than text: keys injected letter by letter in t_boot, a recorded
      Bluetooth reply with the adapter name spelled out byte by byte,
      and user/certdata.h, where the rename edited the message under the
      PSS and ECDSA signatures. That fixture had no generator;
      scripts/mkcertdata.py makes it now.

- [x] **EHCI, and the companion beside it**

      Between UHCI and xHCI there are ten years of machines with EHCI,
      and on those the system saw a keyboard at best and never a stick.
      kernel/ehci.c fills that gap.

      EHCI speaks only to high-speed devices. A USB 1.1 keyboard is
      handed, port by port, to a companion UHCI controller on the same
      sockets - so in EHCI mode usbhc.c runs two controllers at once:
      EHCI's ports first, the companion's after them, and every device
      remembers which one carries it. One shared queue head for control
      and bulk, rewritten only with the asynchronous schedule stopped
      (changing a queue head the controller may have cached is the one
      thing the specification forbids); toggles kept by the caller and
      written into each descriptor; interrupt endpoints with queue heads
      of their own in the periodic schedule, re-armed the moment a
      report is collected so the device is asked every millisecond
      between the caller's rounds. Hubs on EHCI are refused out loud: a
      slow device behind one needs split transactions, and QEMU has no
      high-speed hub to test them with.

      Nothing in the system reads more than a kilobyte at a time, so the
      path for a large transfer would never have run. 'disks read N LBA
      COUNT' reads up to 128 KiB in one command and prints a CRC-32; the
      test computes the same CRC with zlib over the image file. 128 KiB
      is a chain of eight descriptors, and a 33-sector read is two with
      the second one short - both match.

      Two bugs that were not EHCI's, found on the way. The 32-bit kernel
      never noticed a stick plugged in while it ran: the disk driver
      asks for the socket-watching thread while the disks are being
      found, which in that kernel is before the scheduler exists, so the
      thread was created into nothing. It had only ever been tested on
      the 64-bit kernel, where the order is the other way round. And
      t_usbdisk checked a copy with `"skopiowano" in out or "B" in out`
      - a Polish word no longer printed, or a capital B that is there
      whether the copy worked or not.

      t_ehci: 36 assertions. The fourteen groups the change touches,
      rerun: 303 passed, 0 failed.

- [x] **TrueType, drawn in the kernel**

      kernel/ttf.c reads TrueType fonts and draws their glyphs with
      anti-aliasing, and fonts/ carries two of them: DejaVu Sans and
      DejaVu Sans Mono, cut down by scripts/mkfonts.py to Latin-1,
      Latin Extended-A (the Polish letters among them) and a handful
      of dashes and quotes - 30 and 25 KB, with the hinting programs
      dropped because nothing here runs them.

      The outline is read as FreeType reads it and scaled with
      FreeType's own rounding, point by point, component offsets
      scaled separately; arcs are cut into lines exactly as FreeType's
      rasteriser cuts them; and coverage is the exact area under the
      outline, accumulated per row in the manner of font-rs. All in
      integers: the 64-bit kernel has no SSE and the 32-bit one no
      libgcc, so the few 64-bit divisions go through a long division of
      their own.

      The point of doing it FreeType's way is that FreeType can then be
      the judge. 'font sheet' prints glyphs as hex, and t_ttf draws the
      same glyphs from the same file with FreeType (hinting off): 328
      characters at 12 and 16 px, the mono font at 13, a hard selection
      at 48 and 100. Glyph numbers against fontTools' cmap, advances to
      1/64 px, bitmap boxes - all equal. Coverage: worst 2/255, mean
      under a tenth. Both kernels give identical numbers, which also
      says the two ways of dividing agree.

      And the parser went into make fuzz: 500 damaged fonts, every
      glyph of each drawn at two sizes under the address and
      undefined-behaviour sanitizers, nothing found.

      Not done yet, on purpose: the desktop still draws with the 8x8
      bitmap. That is the next step - window titles, menus and the
      taskbar first, together with teaching the tests' screen reader
      the new letters.

- [x] **The end of a session: locking, logging out, restarting, shutting
      down**

      Ctrl+Alt+L (or 'lock' in the shell) puts up a lock: the background
      darkened, the time, the machine's name and a field. Keys go to the
      field and nowhere else - the shell behind it gets nothing, which
      t_lock checks by typing a command at the lock and finding it not
      run - and the pointer reaches nothing. The admin's password opens
      it, checked by the desktop's thread rather than in the keyboard
      interrupt.

      The menu under the mark has a second part below a line: Lock
      Screen, Log Out..., Restart..., Shut Down... The dots mean what
      they have meant for forty years - a question first. It is a card
      in the middle of a dimmed screen with the question, what will
      happen, Cancel and the action; Enter is the action, Esc is Cancel,
      and a click beside the buttons is no answer at all, not even a
      no. Log Out asks every program to end as its red light would (one
      that does not answer is ended anyway), keeps the kernel's own
      windows, and leaves a lock that says "to log in".

      t_session holds it to the machine itself: the four entries read
      off the screen, each question in words, Cancel and Esc leaving the
      machine running with its windows, the program's tile gone from the
      dock after a log out - and Restart and Shut Down really ending
      QEMU (it runs with -no-reboot), the serial port saying which.

- [x] **The panel's own resolution, drawn sharp at a scale**

      The desktop used to start at 640x480 under QEMU and at whatever
      the loader set elsewhere, and on a big panel it drew everything
      at half size and doubled every pixel. Both are gone.

      The mode: QEMU's standard VGA carries the screen's EDID in its
      second memory window, and the kernel reads the preferred timing
      out of it - the panel's own mode - and sets that. The 'resolution'
      setting overrides it. Our UEFI loader asks the firmware for the
      EDID (the active or the discovered one), switches GOP to the
      native mode when the firmware did not start in it, and passes the
      panel's size in millimetres to the kernel in a tag of its own.

      The scale: from the EDID's size where it is known (dots an inch
      against 96), from the panel's height where it is not (720 rows is
      one, 1080 one and a half), always to a quarter, or the 'scale'
      setting in per cent.

      The drawing: the buffer the compositor draws into is the panel's
      size, and every call in kernel/gfx.c takes logical numbers and
      draws at the surface's own size - a gfx_surface now carries its
      scale in sixteenths. Edges are worked out in the panel's pixels;
      the letters are rendered from the outline at one and a half times
      the size, the pen starting on a whole pixel. The background, the
      bar's glass, the dock's kept pixels and the icons are kept at the
      panel's size; a program's window keeps its buffer in the panel's
      pixels too, and its drawing calls are scaled the same way, so a
      calculator's digits are as sharp as the title above them. What
      writes single pixels still - the 8x8 letters, pictures from
      win_blit32 - gets each logical pixel as the block it covers. At a
      scale of one every call passes straight through and the pixels
      are the ones they always were - the whole suite says so.

      t_scale (rewritten): with EDID 1920x1080 the desktop starts in
      that mode and the aurora is painted at that size; the setting
      wins over the screen; with nothing said, 1080 rows give 1280x720
      drawn at 1920x1080, and a window's title and the calculator's
      display read pixel for pixel as FreeType's letters at one and a
      half times their size - which no enlargement could produce -
      while clicks and keys still land where the layout says; at two,
      the title bar's pixels are not squares of four.

- [x] **The display while it runs, and what the machine is**

      'set scale' and 'set resolution' - or the new Scale row in
      Appearance - are taken up within a second, not at the next start.
      A new scale keeps the panel and changes the logical size: icons,
      the dock's kept glass, the bar's strip and every program's window
      buffer are made again at the new size, windows are brought inside
      the desktop (a maximised one fills it again), and each program is
      told its size and draws itself again - until it does, what it had
      drawn is carried over resampled. A new resolution, where the
      kernel set the mode itself, sets the mode again, gives the old
      buffers back and paints everything for the new size. The scale
      never leaves a desktop smaller than 640x480.

      Windows on their way (opening, minimising) are drawn into a
      picture at the panel's size too, so the last frame of the way is
      the window itself rather than a softened copy.

      The menu's system part starts with About ThreadOS: a card with the
      kernel (32- or 64-bit) and its processors, the memory and how much
      of it is free, the screen and its scale, and how long the machine
      has been up. Its rows are a little lower than the programs' so
      that the menu still fits a 640x480 screen.

      The tests' screen reader learned one thing: a letter that leans
      past its advance into the next one (an f before an o) is tried a
      second time with only its own ink held against the screen. And
      the system's entries are read the way a person reads a menu, with
      the pointer on them - on the accent, not on glass that is
      whatever lies under the menu.

- [x] **Polish letters from the keyboard**

      The fonts had them; nothing could type them. AltGr (the right
      Alt, E0 38 on PS/2, the right-Alt bit on USB) with a c e l n o s
      x z gives ą ć ę ł ń ó ś ź ż, with Shift the capitals - the Polish
      programmer's layout. A letter is two bytes of UTF-8: kbdmap hands
      out the first and keeps the second for kbdmap_more(), which both
      kernels' drivers drain; the USB driver injects both.

      Everything that takes typing had to learn that two bytes can be
      one letter. The kernel's cell grid (WD_CELLS) gives a character a
      cell, not a byte. The editor keeps byte offsets - that is what the
      file is - and turns them into columns for the caret, the scroll
      and the status line; Left and Right step over a letter whole,
      Up and Down keep the column, Backspace takes a letter whole and
      Ctrl+Z gives it back whole (the steps of one letter are marked to
      be undone together). The terminal stores what programs write as
      UTF-8, wraps by columns, copies selections by columns, and edits
      its command line letter by letter; the password stars are one a
      letter. The widget kit's fields, its questions and its file picker,
      and the desktop's search, rub out a letter at a time.

      t_polish types "Zażółć gęślą jaźń" and reads it back off the screen
      against FreeType's own glyphs, steps the caret and rubs letters
      out, saves the file and has 'cat' print the UTF-8 back, echoes
      Polish through the terminal, and types "Łódź" on a USB keyboard.

- [x] **A C library, and JavaScript on top of it**

      Until now every ThreadOS program carried what little of a C
      library it needed. Software written elsewhere is written against C
      and POSIX, and needs the real thing - so the 64-bit programs get
      musl 1.2.5 (third_party/musl, unchanged). It is built with eight
      of its files replaced (libc/overlay): the ones that issued the
      syscall instruction with Linux's numbers now call one function,
      __tos_syscall, which translates every Linux call into ThreadOS's.
      musl never learns it is not on Linux.

      Much of the translation is renaming. The rest is built out of what
      the kernel has: stat opens the file and asks about the descriptor
      (a directory is a place one can go to and come back from); lseek
      to the end is to the size; O_APPEND moves to the end before each
      write; mmap's freed ranges are kept and handed out again, because
      the kernel's mapping area only grows; descriptor 2, which the
      kernel does not know, goes wherever 1 goes; the console gets a
      terminal's line discipline (echo, Backspace taking a whole UTF-8
      letter, Ctrl+D) since in ThreadOS the echo is the reader's job;
      signals keep a table per signal behind the kernel's one handler;
      the wall clock is the CMOS second once and the timer from then on.
      A buffer is touched page by page before the kernel sees it - an
      untouched heap page is not in the page tables yet, and the kernel
      checks those. Threads, sockets and poll answer ENOSYS rather than
      a Linux number landing on an unrelated ThreadOS call.

      The kernel got three small things for it: SYS64_FDINFO (what is
      behind a descriptor, a file's size and position), an O_TRUNC flag
      for open, and a per-task FS base (SYS64_SET_FS, loaded on every
      switch) - musl's thread pointer, where errno lives. A task's
      structure is now zeroed when it is made, so a field nobody set is
      a zero. And the stack may grow to 4 MiB instead of 256 KiB.

      On top runs the first program nobody wrote for ThreadOS: QuickJS
      (third_party/quickjs, the core only). apps/js/js.c is its host -
      'js' alone is a console, 'js file.js' runs a script, 'js -e ...'
      runs the rest of the line; there are files (readFile, writeFile),
      timers and a real event loop, exit codes. console.log is written
      in JavaScript (prelude.js) and prints values exactly as Node.js
      does, columns of numbers included - checked against util.inspect
      while it was written.

      t_libc builds libctest (plain portable C: printf's rounding,
      strtod, the heap, files truncated, appended, sought from the end
      and renamed, errno's messages, setjmp, fork and an exit code) a
      second time against the host's glibc and wants the same 52 lines.
      Then it fetches scripts over HTTP onto the ThreadOS disk and wants
      js to print exactly what Node.js prints for them - 92 lines of
      the language (numbers, Unicode, regular expressions, JSON, sorts,
      classes, generators, typed arrays, Dates) and 20 of the event
      loop's order (microtasks, timers, intervals, async iteration). The
      one place the two engines may differ by the standard - the error
      a too-deep recursion throws - is printed as what both agree on.
      Last, the console is typed into on the keyboard, Polish letters
      included.


- [x] **Installing from the disc**

      People put a disc image on a stick with whatever program they
      have - Rufus, Etcher, Ventoy, dd - not with ours. Started that
      way, the system came up through GRUB, and GRUB handed over only
      the initrd: the installer, which writes the kernel's file and
      threadboot onto the disk, had nothing to write and said so. Now
      threados64.iso carries threadboot as a plain file
      (/boot/threadboot.efi), and every entry of grub64.cfg has GRUB
      hand over the kernel's file and threadboot as modules, under the
      names threadboot gives them ("the kernel", "loader") - the
      installer cannot tell the difference. The disc's menu has an
      entry of its own for it: 'install' on the kernel's line starts
      the installer on the whole screen, the way 'desktop' starts the
      desktop.

      t_isoinstall takes the disc 'make iso64' really builds (make test
      builds it now): its menu is grub64.cfg, threadboot is on it byte
      for byte; started under OVMF with somebody else's disk, 'install
      text' writes the files, which mtools reads back equal to the
      build's own, with Windows' loader and the partition table left
      alone; that disk then boots by itself through threadboot; and the
      install entry brings up the installer's own screen.

- [x] **A boot entry for a ThreadOS installed beside Windows**

      The first install on real hardware - a Lenovo IdeaPad with
      InsydeH2O firmware and Windows - went as designed: the installer
      found the disk through AHCI, wrote the three files onto Windows'
      EFI partition and left \EFI\BOOT\BOOTX64.EFI alone, because it was
      Windows'. And then the machine had no way to start ThreadOS: that
      firmware has no screen for adding a boot entry by hand.

      threadboot now has a second mode. Started with the word
      "register" (the disc's new menu entry, "Add the installed ThreadOS
      to this computer's boot menu", chainloads it so), it looks through
      every filesystem the firmware has mounted for
      \EFI\ThreadOS\BOOTX64.EFI, writes a Boot#### variable named
      ThreadOS pointing at it, and appends it to BootOrder - the machine
      goes on starting Windows, and ThreadOS is a choice in its boot
      menu. Run again, it rewrites its own entry. The path starts at the
      partition (HD(n,GPT,guid)/file, the form efibootmgr writes), not at
      the PCI root: the test caught that the full path did not survive
      a controller moving to another slot, and a firmware finds a
      partition by its GUID wherever it is. The installer's last words
      for this case now name the disc's entry.

      t_isoinstall installs onto a disk whose fallback name is taken,
      adds the entry from the disc, reads it back with OVMF's own shell
      ('bcfg boot dump'), sets BootNext to it the way a laptop's F12 menu
      does, and boots the disk with no disc in a machine laid out
      differently.

- [x] **The screen's memory is write-combining**

      On the IdeaPad the desktop animated in jerks. The firmware's MTRRs
      mark the framebuffer uncached, so every store of a frame went to
      the graphics card on its own - a 1366x768 frame is four megabytes
      of them. Under QEMU the "card" is RAM and nothing showed.

      paging64_pat_init() rewrites entry 1 of the page attribute table
      (PWT alone, write-through by default and used by nothing here) to
      write-combining, on the first core and on every secondary one,
      since each has its own PAT MSR; the framebuffer is mapped with PWT.
      A PAT entry of WC beats an MTRR of UC, so the processor now
      gathers stores into whole lines. The boot line says which it got.

      t_wc reads QEMU's own page tables ('info tlb'): the framebuffer's
      pages carry PWT and not PCD, and nothing else carries PWT.

- [x] **threadboot on a machine with gigabytes of memory**

      Installed on the IdeaPad, ThreadOS started over and over right
      after "handing the machine to the kernel". OVMF with the tests'
      few hundred megabytes never showed it; with 6 GiB it did, three
      ways at once:

      - The loader asked for memory "below 4 GiB" for the initrd and
        the tag table, and got the highest pages there - at 3 GiB,
        outside the 512 MiB the kernel identity-maps and manages. Even
        "below 512 MiB" was not enough: the highest pages under that
        lie past the last free memory the kernel is told about. Now
        alloc_low() takes the lowest free stretch above 16 MiB, as
        GRUB does.
      - The firmware loaded the loader itself above 4 GiB, and the way
        out of long mode is 32-bit code: the far jump went to an
        address with its top cut off. The trampoline is now copied
        into a low page and its two absolute addresses patched; and
        CR4.PCIDE is cleared first, since with it set turning paging
        off is a fault.
      - "handing the machine to the kernel" was printed between taking
        the memory map and ExitBootServices: printing may allocate, the
        key goes stale, and the retry asked for a map into a buffer of
        size nought. The line comes first now, and the retry takes the
        map again properly and rebuilds the kernel's copy of it.

      The loader also says where it was loaded ("at: 0x..."), which is
      the first thing worth knowing from a photo of a screen that did
      not boot. And installing again over an earlier ThreadOS replaces
      its \EFI\BOOT\BOOTX64.EFI - recognised by the loader's own
      UTF-16 greeting - instead of keeping it as somebody else's, so a
      machine stuck with the old loader is fixed by installing again.

      t_threadboot boots a 6 GiB machine too: the loader lands above
      4 GiB and the kernel still runs programs from its initrd.

- [x] **The installer puts ThreadOS into the firmware's boot menu**

      On the IdeaPad, even with the loader fixed, the installed system
      started only once the user had added a boot entry by hand: a
      laptop's firmware boots what its menu lists, and ThreadOS was not
      on the list. Every other system's installer adds itself; now this
      one does too, as its last step ("boot menu entry (UEFI)").

      The menu is a set of firmware variables - Boot#### and BootOrder -
      and the only way to them once the loader has left is the UEFI
      runtime services. kernel64/efirt.c calls them: the system table
      and the firmware's memory map arrive as Multiboot 2 tags (GRUB
      always passed them; threadboot does now), the regions marked for
      runtime are identity-mapped into the kernel's own space (code
      executable, device registers uncached), and the call itself runs
      on that space, with interrupts off and the SSE registers of
      whoever was running saved around it, in the firmware's calling
      convention (ms_abi). The entry names the partition by its GPT
      GUID and the file on it, like efibootmgr; it goes first when
      ThreadOS took the whole disk and last when it sits beside another
      system. An entry already called ThreadOS is rewritten, so
      installing again does not pile them up - and the disc's "Add ..."
      entry finds the same one.

      Where there is no way to do it (a 32-bit kernel, a BIOS start)
      the step says so and the files are written all the same.

      t_isoinstall installs beside "Windows" from the disc and reads the
      entry back with OVMF's own shell before booting it; t_wholedisk
      wants it first, and boots on the firmware that now lists it.

- [x] **Updates: a newer ThreadOS from the internet, signed**

      A machine with ThreadOS installed should not need a new stick for
      every new build. 'update' (user/update.c) fetches a release: three
      files - kernel, initrd, loader - and update.txt saying which
      version they are, how long each is and its SHA-256, with an ECDSA
      P-256 signature over all of that on the last line. The public key
      is built into the program; update.key on the disk (a system file
      now - only an admin writes it) can name another. A file changed on
      the way or on the server does not match its hash, and update.txt
      cannot be changed without the key; HTTPS is the channel's privacy,
      not its trust. 'update check' only reports.

      The kernel's part is small: SYS64_UPDATE_WRITE writes a file over
      the installed one on the EFI partition of the disk the system runs
      from (admin only; the fallback loader too when it is ours), and
      SYS64_INITRD_READ lets a program read a file of the initrd - the
      human-facing version label and the separate 'version-code'
      sequence used to order updates.

      Releases are made by .github/workflows/release.yml after every push
      to main: they are labelled 0.1.0-alpha.<run>, while the run number
      remains the monotonic update sequence. Hand-built images use
      0.1.0-dev. scripts/mkupdate.py writes and signs update.txt with the
      key from a repository secret, and the release carries the disc and
      the stick image too. Getting there
      from ThreadOS meant room for GitHub's redirects - a signed download
      link is a kilobyte; uhttp's Location, request and header buffers
      grew - and the roots GitHub's chains end in (USERTrust ECC, Sectigo
      E46 and R46) in the built-in trust store.

      t_update installs ThreadOS onto a whole disk, starts from it, and
      against a server on the host signed with the test's own key: the
      built-in key refuses that release, update.key makes it check out,
      'check' changes nothing, without sudo nothing is written, with
      sudo all three files land on the disk byte for byte, the restarted
      system is update sequence 7 and up to date, and a release signed by
      another key or with an initrd that does not match its hash is
      refused, leaving the disk as it was. Manual rollback restores the
      previous system files; it is not an automatic fallback boot.

- [x] **Files: a folder tree, new folders, cut, copy, paste, double click**

      The file manager had a list, walking into folders, Open, Rename and
      Remove. What a person expects after any other one was missing. Now
      (user/gfiles.c): the folders as a tree down the left - the disk,
      every folder on the way to the one shown (marked), and any opened by
      its arrow; a second bar with Up, New folder, Cut, Copy and Paste,
      and what is waiting to be pasted beside it; a double click on a row
      opens it; an empty folder can be removed after the same question,
      and one with something in it is refused before asking.

      The kernel reads only the directory a program stands in, so the
      tree is read by walking: into each open folder, one readdir each,
      children put straight after their parent in a flat array, then
      back to the folder of the list. Cut and Paste is a rename across
      folders (the blocks stay; the kernel refuses a folder into itself);
      Copy and Paste reads and writes the bytes, and a copy beside its
      original is called "name-copy.ext".

      On the way: the 64-bit side of the shared programs dropped O_TRUNC
      before the kernel (user64/compat64.h now passes OPEN64_TRUNC), so
      saving a shorter text over a longer one left the old end behind;
      and httpget never asked to empty the file it downloads into, on
      either kernel.

      t_filetree checks the tree (marked folder, a click going there, the
      arrow closing it), New folder, Copy+Paste beside the original (a
      new name, the same size), Cut+Paste into another folder (and gone
      from the first), a double click walking in, a non-empty folder
      refused, and the kernel's own cat reading the copy. t_net64
      downloads a long file and then a short one under the same name and
      finds only the short one. t_files, t_wheel, t_desktop2, t_gedit
      and t_gui64 follow the list to where it now starts.

- [x] **Tidying up before the repository goes public**

      A new README: shorter, in sections someone arriving from outside
      reads in order - what ThreadOS is, how to try it on a PC (Rufus in
      DD mode, Secure Boot off, the installer adds the boot entry), how
      to update, what works, how to build and test, where things are, and
      what comes next. The old one had grown one paragraph per feature
      until the 32-bit kernel's was a page long, and it still spoke of
      an "Edytuj" button and of six applications.

      The top of the tree holds only what a visitor looks for first: the
      GRUB menus moved to boot/ (grub.cfg, grub64.cfg), each kernel's
      linker script next to its code (kernel/linker.ld,
      kernel64/linker64.ld), and this journal to docs/. The C++
      groundwork for litehtml (apps/browser/layouttest.cpp, libc/cxx/)
      left the tree: nothing built it, and it named a test that does not
      exist. It is in the history (4a591f0) for when the browser engine
      comes back. The last Polish in code and messages went too
      (BIALY/CZARNY in two tests, the reasons ptest gives for skipping,
      a message of the fuzz harness), and the fuzz stubs learned the
      kernel's irqflags_t and spin_lock_irq, which the TrueType harness
      had needed since the font code took a lock.

- [x] **Instructions for AI agents, a technical README, the journal in English**

      The repository is meant to be read by people who do not speak Polish,
      and more and more of the people who change it will work through an AI
      coding assistant. Three things followed.

      AGENTS.md (with CLAUDE.md importing it) gives such an assistant the
      rules this repository is reviewed by, and the commands: how to build
      both kernels, how to run one test group against a build that is not
      in build/, never to rebuild build/ under a running suite, where a
      system call, a program or a driver is added, and that test helpers
      read layout constants straight out of the C sources.

      The README became a technical one: the architecture of the two
      kernels and the two boot paths, prerequisites, make targets and
      variables, running in QEMU, the test suite, the release pipeline and
      how to rotate the update signing key, the generated files and how to
      update third-party code.

      And this journal, kept in Polish for its first five thousand lines,
      was translated into English and renamed docs/JOURNAL.md. Its old
      introduction (a file tree, requirements and build commands that had
      long gone stale and that the README now covers) was replaced by a
      short one; the entries themselves were translated as written,
      including the names things had at the time.


- [x] **C++ on ThreadOS, and litehtml laying out pages exactly as on
      Linux** (libc/cxx, third_party/litehtml, apps/browser, the `browser`
      test group).

      The browser so far turns HTML into lines of text (user/uhtml.h).
      A real one needs a layout engine: CSS, floats, tables, flexbox. The
      smallest serious one is litehtml - and it is C++, the first C++ this
      system has run.

      C++ here is the host compiler's own libstdc++ on top of musl: its
      headers in front of musl's, and its static library for the few
      things that are not templates. Programs are built without
      exceptions. The hard part was the link, and it was one file: the
      library's ready-made std::string (string-inst.o) was compiled with
      exceptions, so its members catch and rethrow, and pulling it in
      pulled the whole exception runtime and an unwinder that is not
      there. libc/cxx/cxxrt.cpp now instantiates std::string itself, in a
      file built without exceptions, and supplies the whole operator new
      and delete family (the library's nothrow ones had the same
      problem). What is left from the library is RTTI, the red-black tree
      and the list node - none of them throws.

      litehtml is in third_party/ exactly as upstream has it (commit
      8848b99), with gumbo, its HTML5 parser, inside. Two of its files
      need a change for this runtime - one throw used to abandon the
      encoding prescan, which becomes a longjmp, and two functions that
      use string streams, which become plain string operations - and
      those changes live as patches in apps/browser/patches, applied to a
      copy at build time, each with its reason at the top.

      The check is a comparison. apps/browser/layouttest.cpp lays out four
      pages (blocks with collapsing margins, floats, lists and a table,
      flexbox with selectors and relative positioning) with a font whose
      widths are known exactly, and prints every box and every word. The
      same source and library are built for this machine too; the
      ThreadOS build is fetched onto the ThreadOS disk over HTTP and run
      from there, and all 220 lines have to agree. They did the first
      time - strings, maps, shared pointers, virtual calls, dynamic_cast
      and floating point all behaving the same on musl as on glibc.

      The test program is not in the initrd: a megabyte and a half of it
      would be memory every boot spends, and the swap test already showed
      how close that budget is. A program on the disk is a program too.

      Next: a document_container that draws into a window with the
      system's own fonts, and the browser on top of it.

- [x] **The browser draws pages** (apps/browser/web.cpp, a new `web`
      group, the browser part of `tls64`).

      `web` is litehtml with a document_container that draws through
      udraw.h: the desktop's TrueType lettering, its rounded boxes, a
      white page in a true-colour window. Pages come from the disk or,
      through httpget, over HTTP and HTTPS - one place that resolves
      names, follows redirects and checks certificates, not two. The
      64-bit Web tile now opens it; the 32-bit system keeps gweb, which
      needs no C++ runtime. A click in the address field picks the old
      address whole, and a bare `example.com/` means the web, as in any
      browser.

      **It had to fit in sixteen megabytes.** The swap test gives the
      machine 16 MiB and asks for 17; whatever the initrd holds is not
      free, and the first build of the browser added 1.5 MB and broke it.
      Three things brought it back: C++ built with -Os and
      --gc-sections; litehtml's Chinese, Japanese and Korean legacy
      decoders left out (370 KB of tables for text there is no font
      for; the same pages in UTF-8 are unaffected), as a patch with its
      reason; and every musl program stripped, with QuickJS at -Os. The
      initrd is 2.5 MB, against 2.1 MB before the browser.

      The test reads the window from outside: the start page's title,
      an address typed and fetched from the host, the heading and the
      floated box in the exact colours the page's CSS gives them, the
      box 216 pixels wide (200 and the padding) on the right half, its
      words read back off it, a click on the link's underline following
      it, and the back arrow returning. litehtml places every word on
      its own at a whole pixel, so the screen reader got `read_page`,
      which reads a line a word at a time.

- [x] **Settings in sections, and system files out of sight**
      (user/gsettings.c, user/gfiles.c, a new `settings` group).

      The settings window was the settings file drawn as a table:
      `background 3` says nothing to someone who wants the green one. It
      is now split the usual way - General, Appearance, Display, Sound,
      Network, Files and About down the left - and each setting is shown
      as what it is: a row of buttons with the current one lit, the
      backgrounds as the kernel draws them, a box to tick, a field. The
      network and the machine's figures are shown there too. Every change
      is written at once; the first asks for the admin's password in a
      panel, and the right is given back after each change.

      The file manager no longer lists swap.sys, system.cfg and the rest
      of what the system keeps in the root, nor names starting with a dot;
      the new `hidden` setting (Settings, Files) shows them.

      The test checks every change through the kernel's own shell (`set
      <name>`), the network figures against QEMU's fixed addresses and the
      memory against `mem`. Tests that used the old table as a generic
      window now use its new layout, or the terminal where they needed a
      window that fills its whole content.

- [x] **UEFI starts at the desktop, not a hidden shell.** The installed
      loader now passes `desktop` to the kernel. The shell stays alive
      behind the desktop for commands routed from the keyboard, but its
      framebuffer output (and other kernel console writes) no longer
      flashes over the compositor; installer boots do not start shell
      threads they cannot use.
      The threadboot test checks that the UEFI handoff reaches the
      desktop at the firmware's screen resolution. The musl build
      normalizes its configure script, shell scripts, sed program and
      input templates after copying sources: Git's CRLF conversion
      breaks shell parsing and sed's end-of-line matches.
      The litehtml patch rule likewise normalizes temporary copies of
      both source and patch, so GNU patch applies cleanly when Git checks
      either file out with CRLF. The generated source depends on the
      Makefile too, so changing the patch recipe rebuilds an earlier
      failed output; patching into a temporary output prevents a rejected
      patch from leaving a compilable-looking stale target.

- [ ] **Accounts, login and private home directories.** The desktop now
      opens at an account login screen. Accounts have stable UIDs, admin
      or user roles, salted PBKDF2 password hashes, and private folders
      under `/home`; the `gusers` window can add, remove, reset passwords
      and change roles, with a fresh admin password for account changes.
      `passwd` verifies the current account's password. Logging out ends
      that UID's ring 3 processes before another account can sign in.
      Role changes, password resets for another account and account deletion
      terminate that UID's ring 3 processes. Deleted homes are archived under
      their old UID so a later account reusing the name starts with an empty
      private folder.
      The legacy fast password hash is removed from public `system.cfg`
      after the protected account database is loaded or migrated.
      TFS stores the owner in the inode type byte without changing the
      inode size, so old files remain root-owned and old TFS3 images can
      still be read. File and directory access checks run in the shared
      filesystem code used by both kernels. Writable memory mappings check
      ownership when created and again before writing pages back, so a
      plain session cannot use `mmap` to bypass file checks or keep system
      write access after dropping admin rights. Build and QEMU verification
      for both kernels remains to be run.

- [ ] **Open common media from the desktop.** The file manager and desktop
      search route audio and video files to a 64-bit player. FFmpeg 9.0 is
      built from its pinned upstream archive with a small LGPL codec set
      for MP3, WAV, AAC, FLAC, Ogg, M4A, MP4/MOV, Matroska and AVI. The player
      decodes audio to the HDA driver's 48 kHz stream and scales video into
      a desktop window, paced by frame timestamps. The existing picture
      viewer continues to open PNG, JPEG and BMP images. A stream is stopped
      if its owning task exits. Build and playback verification remain to
      be run on Linux.

- [ ] **A first-run profile instead of a surprise login.** On a fresh
      graphical installation the desktop now asks for a login name, a
      separate profile name, an optional password and a theme before
      showing the desktop. Leaving both password fields empty signs in
      automatically on this and later boots. The account database accepts
      the chosen login name as its initial administrator, and the profile
      name is retained for the logged-out screen. The login and session
      tests cover profile creation, rejection of a wrong password, saved
      credentials after restart, the displayed profile name, and
      passwordless sign-in with a saved light theme. Linux build and QEMU
      verification remain to be run.

- [ ] **Check and install updates from Settings.** The 64-bit Settings
      window now checks signed releases through the existing updater and
      offers installation only after a newer release is verified. It
      asks for the administrator password before starting the installer.
      The kernel refuses update writes unless ThreadBoot identified its
      exact installed EFI path; the Live USB boot entries and unknown boot
      sources remain read-only. The 64-bit graphical test checks the
      section and its disabled-install state without an installed boot
      marker, and the update test verifies a signed release cannot be
      written during a GRUB Live boot. Linux build and QEMU verification
      remain to be run.

- [ ] **Remove desktop placeholders and use Paint drawings as wallpaper.**
      The desktop no longer opens pretend Terminal and Files windows, and
      file, memory and process-size displays use IEC units. Paint can export
      its canvas as the desktop's RGB wallpaper format; Settings can also
      select a wallpaper file, and the compositor reloads it while running.
      The application test checks the exported drawing behind the desktop.
      Linux builds and QEMU verification remain to be run.

- [ ] **Add desktop shortcuts, Trash, and desktop actions.** Each profile
      receives a Desktop folder and a private Trash. Home, Files, and Trash
      shortcuts are shown on the desktop; files and folders created in
      Desktop appear there and open through Files. The compositor follows
      the active account's filesystem identity across sign-in and logout, so
      it can enumerate only that profile's Desktop. Removing an item moves
      it to Trash with its original path recorded, where it can be restored
      or permanently emptied. The desktop right-click menu has separate New
      Folder, New Text File, and Refresh actions, and the menu bar hides
      after three seconds of inactivity until the pointer reaches the top
      edge. Files and both desktop-kernel tests now cover per-profile
      Desktop enumeration after sign-in; desktop-menu and bar tests cover
      the new actions and hiding behavior. Per the user's request, builds
      and QEMU tests were not run.

- [ ] **Identify PCI and network devices from the desktop terminal.**
      `lspci` (also available as `hardware`) lists each PCI device's bus,
      vendor/device IDs, and class, including devices behind PCI bridges;
      `ifconfig` displays the configured IPv4 address, mask, gateway and
      DNS or clearly reports when none is configured.
      A read-only syscall exposes one checked device record in both kernel
      ABIs. The IDs and class make it possible to identify a Wi-Fi chipset
      without claiming that ThreadOS has a driver for it. The 32- and
      64-bit terminal tests exercise both commands; the 64-bit test also
      checks that the `hardware` alias does not exit with `[code -1]`.
      The reported real-laptop failure still needs its kernel exception
      output to identify the cause. Builds and QEMU tests were not run
      locally.

- [ ] **Move and open desktop shortcuts; skip install passwords on live
      media.** Home, Files, Trash, and Desktop item icons can be dragged,
      with each profile's layout saved in its home directory. A click
      selects an icon; a double-click opens its target, while dragging
      does not accidentally launch it. The kernel no longer requests an
      account password for an install started from live media: the
      installer's explicit YES/ERASE confirmation remains, and an installer
      running from the installed system still requires recent admin
      authentication. The 32- and 64-bit desktop tests cover moving and
      opening shortcuts; the installer test covers the live-media flow.
      Builds and QEMU tests were not run locally.

- [ ] **A graphical boot screen instead of kernel messages**
      (kernel/bootsplash.c, kernel/printk.c, kernel/kmain.c,
      kernel64/kmain64.c).

      Graphical boots now draw a ThreadOS title and a progress bar whose
      stages follow memory, system files, storage, network and device
      initialization. Kernel messages remain on the serial port; text
      boots and systems without a loader framebuffer keep the console.
      A fatal kernel exception restores visible diagnostics. The new
      `bootsplash` test checks the framebuffer colour after GRUB starts
      both kernel modes. Builds and QEMU tests were not run locally.

- [ ] **English and Polish system language**
      (kernel/locale.c, kernel/uitext.c, user/gsettings.c).

      Region settings now select and persist `language=en` or `language=pl`.
      Fixed interface labels use explicit localized drawing operations;
      file names, editor contents and other user data use raw text paths and
      are never translated by matching their contents. The first coverage
      batch includes Settings, desktop menus, the file manager, editor,
      terminal, clock, calendar, calculator, appearance, account manager,
      system monitor, task manager, image viewer, Paint, the graphical
      installer, and the browser's fixed status messages; window titles
      follow language changes. The installer keeps disk labels, file paths,
      and typed confirmation words unchanged while translating its fixed
      warnings and actions. Tests switch languages, verify localized UI,
      check that a file named `Files` stays unchanged, and run the installer
      test in Polish. Broader application and kernel-message coverage
      remains. Builds and QEMU tests were not run locally.

- [ ] **Keep one previous system for manual update recovery.**
      Before replacing the installed kernel, initrd, or ThreadOS loader,
      the kernel copies the current files on the EFI partition and records
      whether the ThreadOS fallback loader also needs restoring. The
      update UI and `sudo run update rollback` restore that snapshot;
      incomplete updates stay marked until rollback succeeds. This snapshot
      does not include personal files, and manual rollback requires a
      bootable installed system. The update test checks byte-for-byte
      restoration and boots the restored system. Builds and QEMU tests were
      not run locally.

- [ ] **Pre-1.0 versions, with update ordering kept separate.**
      Hand-built images display `0.1.0-dev`; CI releases display
      `0.1.0-alpha.<run>`. The initrd stores this human-facing label in
      `version` and keeps the monotonic signed-update sequence in
      `version-code`. The updater reads the sequence for ordering and still
      accepts older images that only have a numeric `version`. Release tags
      follow the same pre-1.0 label. The update test checks the displayed
      label and sequence. Builds and QEMU tests were not run locally.

- [ ] **ThreadCode, a native project workspace.**
      The Code dock/menu app reuses the editor's editing, undo, and search
      code while adding a project file/folder list, up to three saved-file
      tabs, C-oriented syntax colouring, brace-aware indentation, and a
      Terminal button that opens in the project directory. ThreadOS does
      not yet bundle a compiler, so the workspace reports that clearly
      instead of pretending to build. `t_gcode` covers 32- and 64-bit
      project opening, highlighting, tabs, indentation, saving, and the
      terminal launch. Builds and QEMU tests were not run locally.

- [ ] **Pre-1.0 versions, with update ordering kept separate.**
      Hand-built images display `0.1.0-dev`; CI releases display
      `0.1.0-alpha.<run>`. The initrd stores this human-facing label in
      `version` and keeps the monotonic signed-update sequence in
      `version-code`. The updater reads the sequence for ordering and still
      accepts older images that only have a numeric `version`. Release tags
      follow the same pre-1.0 label. The update test checks the displayed
      label and sequence. Builds and QEMU tests were not run locally.

- [ ] **ThreadCode, a native project workspace.**
      The Code dock/menu app reuses the editor's editing, undo, and search
      code while adding a project file/folder list, up to three saved-file
      tabs, C-oriented syntax colouring, brace-aware indentation, and a
      Terminal button that opens in the project directory. ThreadOS does
      not yet bundle a compiler, so the workspace reports that clearly
      instead of pretending to build. `t_gcode` covers 32- and 64-bit
      project opening, highlighting, tabs, indentation, saving, and the
      terminal launch. Builds and QEMU tests were not run locally.

- [ ] **Desktop actions and command entry points.**
      The 64-bit desktop now splits app command lines into argv before
      spawning Files, so shortcuts and right-click creation actions receive
      their paths and options. The `lspci`/`hardware` and `ifconfig` programs
      now use the required freestanding `_start` entry point and exit syscall,
      rather than returning through an invalid stack. `t_desktop64` and the
      terminal groups cover these paths. Builds and QEMU tests were not run
      locally.

- [ ] **Legacy VirtIO network support.**
      A ThreadOS-native PCI driver handles VirtIO-net's legacy transport,
      negotiates the MAC address, and moves ordinary Ethernet frames through
      split receive/transmit queues. Checksum and segmentation offloads stay
      disabled. The `virtio` QEMU test verifies DHCP and ICMP in both kernel
      modes; modern-only VirtIO PCI devices are not supported yet.

- [ ] **ThreadCode Quick Open.**
      Ctrl+O opens a project picker that filters filenames as text is typed.
      Enter opens the first match, while an unmatched name can still be
      entered directly. `t_gcode` covers filtering and opening a matching
      project file in both kernel modes.

- [ ] **ThreadCode terminal panel.**
      The Code window has a TERMINAL panel that runs a shell in the project
      directory (`sh` on the 32-bit kernel, `ush64` on the 64-bit one). The
      process launch and incremental output reading that `gterm` already
      had now live in `user/uterm.h`, so both programs share them. The
      panel keeps its output in hidden `.code-term.*` files. `t_gcode` types
      a command into the panel and checks that it ran in the project
      directory; `ush64` has no `pwd`, so the 64-bit run uses `uls64`.
      `t_gterm` now creates its Tab-completion files from inside the
      terminal, because keys typed with no window focused no longer reach
      the kernel shell once a profile is logged in.

- [ ] **ThreadCode in a VS Code layout.**
      The chrome follows the VS Code layout: an activity rail, an EXPLORER
      sidebar with file and folder icons, a tab strip, a breadcrumb bar, a
      status bar, and a bottom panel with PROBLEMS / OUTPUT / DEBUG CONSOLE
      / TERMINAL tabs. The terminal prints inline, with the prompt line
      carrying the typed input. `t_gcode` reads the geometry from the
      `IDE_*` defines in `user/gedit.c`.

- [ ] **Layout and tab primitives in the GUI toolkit.**
      `user/ugui.h` gained a fixed/flex layout (`ug_layout_*`), a workspace
      splitter (`ug_workspace_layout`) that produces the toolbar, activity
      rail, sidebar, tabs, breadcrumbs, editor, panel and status rectangles,
      and a `UG_TABS` widget with click and Left/Right key selection. The
      existing widget API is unchanged. The `udtest` program draws all of
      it, and `t_udraw` checks the rectangles, the tab underline, clicking
      and arrow keys on both kernels. ThreadCode is the next consumer.

- [x] **A terminal with the usual commands** (user/core.c, user/gterm.c,
      a new `terminal` group).

      `ls`, `ps`, `free`, `date` and the rest were the kernel shell's own
      commands; on the 64-bit system the terminal answered `ls` with "no
      such program". They are now one program, `core`, built for both
      kernels (about 20 KB, instead of twenty files of the same code in
      an initrd that is all memory): ls, pwd, cat, head, wc, cp, mv, rm,
      mkdir, touch, echo, ps, kill, free, uptime, date, uname, hostname
      and set. The terminal runs `core ls -l` when `ls -l` is typed,
      pipes included.

      The test runs them in the terminal on both kernels and checks each
      against something else: files made and moved there are read by the
      kernel shell's `cat` and listed by its `fls`, `free` against `mem`,
      `date` against QEMU's clock, a `set` against what the kernel then
      holds, and `echo one two three | wc` counts 1, 3, 14.

- [x] **The dock without the clock and the demonstration program**
      (kernel/desktop.c, scripts/ptest.py).

      The time is on the bar, so a Clock tile was a second clock; the App
      tile was the first ring 3 demonstration, not something a person
      uses. Both left the dock and stay in the menu. Tests now start
      programs by name (`launch_dock("gterm")`, read from icons[] in
      desktop.c) instead of by tile number, and the dock helpers default
      to no windows, as the desktop now starts with none.

- [x] **Network and Bluetooth on the bar** (kernel/desktop.c, a new
      `netbar` group).

      Two icons left of 'show the desktop', lit when there is something
      behind them: a wired card with an address, a Bluetooth adapter that
      answered. A click hangs a card from the bar with the card's name and
      the address, an honest line on Wi-Fi (no driver yet), or the reason
      there is no Bluetooth adapter. With an adapter, the card searches
      for devices in a thread of its own - the search listens for seconds,
      and the compositor must not stop drawing for that long.

      The test opens both cards against what QEMU gives: an RTL8139 with
      10.0.2.15, and no USB controller, hence no Bluetooth.

- [x] **The browser shows pictures and reads style sheets from files**
      (apps/browser/web.cpp, the `web` group).

      `<img>` and CSS backgrounds: PNG (eight bits a channel, any colour
      type, not interlaced; through uinflate.h) and baseline JPEG (the
      picture viewer's ujpeg.h), fetched while the page is read - the
      layout needs each size before it can place what follows - and
      drawn scaled to the box it gets. `<link rel=stylesheet>` is fetched
      like the page, and what it names is relative to where it came from.
      A long page shows a scroll thumb at the right edge.

      The test serves a page with a PNG drawn at twice its size, a style
      sheet in a file of its own and forty lines: each quarter of the
      picture is exactly 64x48 pixels of its own colour, in its place;
      the style sheet's box is in its colour, 150 pixels and the padding
      wide; the thumb is there, and PgDn moves it down.

- [x] **The browser goes forward, reloads, searches and follows `#links`**
      (apps/browser/web.cpp, kernel/config.c, the `web` group).

      Next to back there is forward (the pages gone back past, dropped
      once a new one is opened) and reload, which fetches the page again
      and keeps the place on it. What is typed into the address field and
      does not look like an address - a space in it, or no dot and no
      scheme - goes to a search engine: the new `search` setting names
      the start of its address (DuckDuckGo's plain HTML page by default),
      and the words go after it, encoded. A link to `#name` on the same
      page scrolls to the element with that id, or the `<a name>`.

      The test sets `search` to its own server from the terminal, types
      two words and gets that server's results page; follows a link to a
      heading forty lines down and finds the heading's colour on the
      screen, where it was not before; and reloads a page the server has
      changed in the meantime.

- [x] **Forms in the browser** (apps/browser/web.cpp, kernel64/proc64.h,
      the `web` group).

      `<input>` (text, password, hidden, checkbox, radio, submit) and
      `<button>`: litehtml lays them out and leaves what they hold to
      us. A click in a field gives it the keys, a box ticks, a radio
      button clears the others of its name; Enter in a field or a click
      on a button sends the form as `action?name=value&...`, with the
      pressed button and without the unticked boxes. POST forms go the
      same way - httpget only GETs, and the search pages that use POST
      take a GET as well.

      Sending the first form showed the 64-bit kernel cut every program
      argument at 31 characters: the browser hands httpget each address
      it fetches, so any longer address was being fetched without its
      end. Arguments may now be 255 characters.

      The test finds the field, the box and both buttons on the screen by
      colours the page gives them; checks the placeholder is drawn until
      the field is clicked and a caret after; types two words and sends
      them with Enter, unticks the box and sends with the button, and
      sends a second form through its `<button>` - each time the server
      answers only for exactly the expected query.

- [x] **Lists, text boxes and POST in the browser** (apps/browser/web.cpp,
      user/uhttp.h, user/httpget.c, scripts/ptest.py, the `web` group).

      `<select>` shows its chosen option and, clicked, a list under it to
      choose from (with the mouse, or the arrows and Enter); it is as wide
      as its longest option unless the page says otherwise. `<textarea>`
      holds its text as its value, wraps it at the box's edge and takes
      Enter as a new line; it is sized by its cols and rows. A form that
      says POST is now posted: httpget learned `-p <file>`, which sends
      the file as an application/x-www-form-urlencoded body (a file, not
      an argument, because a form can be longer than an argument may be),
      and after a 301, 302 or 303 it fetches the answer with a GET, as
      browsers do. httpget also keeps paths up to 511 characters, not 127.

      The test server answers a POST only for exactly the expected body.
      The test opens a list by a click, checks the chosen option is lit
      under the box, picks the third, adds a line to a text box and posts
      the form: the body must be `size=l&note=first%0D%0Asecond`. The
      earlier `<button>` check now goes through a real POST too.

- [x] **Cookies: signing in to sites** (user/ucookie.h, user/uhttp.h,
      user/httpget.c, apps/browser/web.cpp, the `web` group, `make fuzz`).

      The browser keeps a cookie jar (`.web-cookies.txt`) and every fetch
      goes with it - pages, pictures, style sheets. httpget learned
      `-c <jar>`: it sends what applies (Cookie) and keeps what the
      server sets (Set-Cookie), through redirects too, because a login
      usually answers with a cookie and a redirect in one response. The
      rules are RFC 6265's: a cookie goes back only to its host, or with
      Domain= to that domain (never to one the host is not in); only to
      paths under its Path=; Secure ones only over https; Max-Age=0 or a
      past Expires= removes one. Other expiry dates are not kept. httpget
      also got `-o <file>`, so the browser's eight arguments still fit.

      The test signs in through a POST form whose answer sets a session
      cookie, a cookie for another path and one for another domain, and
      redirects: the next page and its picture come with exactly
      `session=abc123`. Signing out (Max-Age=0 with a redirect) brings a
      page with no cookie at all. `make fuzz` gained a cookie target: on
      its first run it found that a Path= with a space in it broke the
      jar file's lines; such cookies are now refused, and a jar written,
      read and written again must come out the same.

- [x] **Selecting and copying text in the browser; Tab and paste in
      forms; round radio buttons** (apps/browser/web.cpp, the `web`
      group).

      litehtml has no selection, but every run of text passes through
      the container's draw_text with its place. The browser keeps those
      runs from each drawing; a drag lights the ones between where it
      started and where it is, a word at a time in reading order, and on
      release puts them on the system clipboard - selecting is copying,
      as in the terminal (Ctrl+Insert copies again). Shift+Insert pastes
      into a form field, Tab moves to the next field. Radio buttons are
      drawn round by the browser, since CSS borders here are square.

      The test drags across a paragraph in a colour of its own: the
      selection colour covers the words and the copied text is exactly
      "copy these words". It then types into one field, Tabs to the next
      and pastes with Shift+Insert: the server answers only to
      `t=a&u=copy+these+words`, so the words went through the kernel's
      clipboard and into the right field.

      Found on the way: Escape and Shift+Insert reach programs as the
      same code (0x1B), so Escape in the terminal pastes. Left for a
      change of its own; in a browser field 0x1B means paste.

- [x] **Find on the page, plain text, downloads** (apps/browser/web.cpp,
      the `web` group).

      Ctrl+F opens a find field in the status line. The page's text is
      gathered in reading order from litehtml's text elements (hidden
      parts left out, whitespace as one space), searched without regard
      to case, and every match is lit - the current one in another
      colour - by finding where its elements were placed when the page is
      drawn. Enter goes to the next match and scrolls it into sight;
      Escape closes the field. A `.txt` address is shown as text in a
      <pre>, its < and & as letters. A link to anything that is not a
      page, plain text or a picture - or one with a `download` attribute -
      saves the file in the browser's directory (the person's home)
      under the name its address ends in, with -1, -2 added rather than
      over a file already there.

      The test finds "apple" three times in three cases, walks to the
      third far down the page and sees it lit on the screen, and sees the
      light go with Escape; finds "<b>" in a .txt file, which is there
      only if the text was not taken for markup; and downloads a file
      twice: both are saved whole, the second as report-1.dat, the page
      stays, and the kernel shell's own 'cat' reads the file's three
      lines from the disk.

- [x] **Bookmarks and history in the browser** (apps/browser/web.cpp,
      the `web` group).

      A star at the end of the address field keeps the page among the
      bookmarks, and takes it out again; it is full for a page that is
      one. Every page opened goes to the top of the history (fifty kept).
      Both are files in the person's home, lines of address and title,
      so they outlive the browser. The start page lists the bookmarks
      and the ten pages seen last, as links. The star is drawn, not
      lettered: the system's font has no ★.

      The test stars a page and sees the star fill with the accent
      colour; finds the page's title twice on the start page (once in
      each list); closes the browser, opens it again and finds both
      still there; opens the page from its bookmark, its star full; and
      empties the star again.

- [x] **The browser answers while a page loads, and can stop it**
      (apps/browser/web.cpp, scripts/ptest.py, the `web` group).

      Fetching used to wait for httpget with the window frozen. Now the
      browser polls it (waitpid with WNOHANG) and keeps reading the
      window meanwhile: the status line counts the kilobytes come so far,
      the reload button becomes a cross, and the cross or Escape kills
      httpget. A stopped page leaves the one before it on the screen; a
      stop while a page's pictures come shows the page without the rest.
      Only the toolbar and the status line are drawn while loading, since
      litehtml may be in the middle of building the next page.

      The test server can now send a page a piece at a time. The test
      opens one that takes twelve seconds, presses the cross after five:
      the browser reports how much had come (some, not all of it), and
      the page from before is still the one shown.

- [x] **Links in a new window; the right and middle buttons reach
      programs** (kernel/desktop.c, user/usys.h, user64/usys64.h,
      apps/browser/web.cpp, the `web` group).

      A window's right button belonged to the program that drew it, the
      comment said - but nothing was sent; the middle button was not
      looked at at all. A press of either over a window's content is now
      a WEV_ALTCLICK (val: 2 right, 4 middle) - a type of its own, so a
      program that knows only WEV_CLICK does not take it for the left
      button; the old gweb, the one program that does not ignore unknown
      types, skips anything but keys. The browser opens a link pressed so
      in a new browser window: fork and exec 'web' with the address; the
      kernel reaps it when it ends, so nobody waits.

      The test right-clicks a link: a second window opens one cascade
      step on, titled with the linked page, and the first, brought
      forward again, still shows the page it had. winmgr, clip, gtermsel,
      snap and desktop64 pass on both kernels; wheel and desks fail the
      same nine assertions with and without this change.

- [x] **Zoom in the browser** (apps/browser/web.cpp, the `web` group).

      "- 100% +" at the right end of the status line zooms the page in
      the steps browsers use, 50% to 200%; a click on the percentage
      brings it back to 100%. litehtml lays the page out in its own
      pixels for a window narrower or wider by the zoom (so text wraps
      as it would on a smaller or larger screen), and only the drawing
      is scaled: text at the zoomed size, boxes, borders (at least a
      pixel), pictures, form fields, list markers. Clicks, drags and the
      scroll are turned back into the page's pixels.

      The test zooms the test page three steps: the red heading becomes
      1.5 times as wide (within 4%); a click on the link where the zoomed
      page drew it follows it; and the percentage puts the heading back
      exactly where it was.

- [x] **Character sets in the browser** (apps/browser/web.cpp,
      user/uhttp.h, user/httpget.c, kernel64/proc64.h, `webtools`).

      litehtml already follows the WHATWG sniffing for a byte order mark
      and a <meta charset>. What it could not know was the server's word
      and the system's default. httpget learned `-t <file>`, which writes
      the answer's Content-Type, and the browser takes its charset as
      certain. A page that says nothing and is not valid UTF-8 is read
      in the old single-byte set of the system's language: windows-1250
      in Polish, windows-1252 otherwise, as browsers decide it. A .txt
      file is decoded to UTF-8 before it is shown. The browser's call of
      httpget now has ten arguments, so the 64-bit kernel takes sixteen
      (4 KiB in all, on a 16 KiB kernel stack).

      The test serves the same Polish words in windows-1250 with a
      <meta>, in ISO-8859-2 with only the server's header, and French
      words in windows-1252 with nothing at all; each page's title must
      come out as Python's own codecs say. A windows-1250 .txt file,
      selected and copied, must give the Polish words in UTF-8.

- [x] **The browser's tests in three groups** (scripts/tests/webtest.py,
      t_web, t_webform, t_webtools).

      t_web had grown to ten minutes in one QEMU. The pages, the server
      and the boot are now in webtest.py, and the scenario in three
      groups that run side by side: pages (`web`), forms, cookies and
      the clipboard (`webform`), and the tools (`webtools`). With -j3 the
      lot takes under four minutes.

- [x] **Forms sent in the page's character set** (apps/browser/web.cpp,
      the `webform` group).

      A form on a windows-1250 page was sent in UTF-8, which a server
      written for that page reads as the wrong letters. It now goes in
      the set the page was read in, as browsers send it: the set's own
      decoding turned round into a table, and what the set lacks as a
      numeric reference ("&#9731;"). Which set that is: the server's,
      else litehtml's own sniffing, else - since litehtml parses a page
      again when a <meta> names another set, and does not say so - the
      <meta> found in the parsed page. (Its prescan did not find
      `<meta charset=windows-1250>` where the parse did; the test caught
      it.)

      The test sends a hidden field "żółw" and a snowman from a
      windows-1250 page: the query must be exactly what Python's
      quote_plus gives for "żółw&#9731;" in cp1250.

- [x] **Tabs in the browser** (apps/browser/web.cpp, the `webtools`
      group).

      A plus in the toolbar opens a tab on the start page; the middle
      button opens a link in a tab behind the current one (the right
      button still opens a window). With two tabs or more a strip shows
      under the toolbar - each tab with its title and a cross - and the
      page moves down for it. Each tab keeps its own page, address,
      history, scroll and form fields; switching is a swap of those with
      what the window shows, so the rest of the browser still works on
      "the page" as before.

      The test opens a tab with the plus, loads a page in it, middle-
      clicks a link (three tabs, still at the second, its page still
      shown), shows the third and then the first, each with its own page,
      and closes two with their crosses back to one.

- [x] **Site icons, and the tabs kept between runs** (apps/browser/web.cpp,
      the `webtools` group).

      Each page's site icon shows before its address and on its tab: the
      one its <link rel=icon> (or "shortcut icon") names, else the
      site's /favicon.ico. PNG and JPEG go through the decoders pages
      use; .ico is new - a directory of pictures from which the one
      nearest 16 pixels is taken, each a PNG or a headerless bitmap (32
      bits with alpha, or 24 with a one-bit mask). Transparent parts are
      laid on the toolbar's colour rather than the page's white. An icon
      is fetched once a run for each address.

      The browser started from the dock writes its tabs to a file in the
      home directory as they change, and opens them again on its next
      start, at the tab that was in front. A browser opened for a link
      (with an address) neither reads nor writes it.

      The icons in the test are made by Pillow, not by us: a bitmap .ico
      as the site's /favicon.ico, a PNG and a PNG-holding .ico named by
      <link>s; each shows in its colour beside the address. Two tabs,
      the browser closed and opened from the dock: both come back, the
      second in front, the first with its page.

- [x] **Cookies live as long as they say** (user/ucookie.h,
      user/httpget.c, tests/fuzz/harness.c, scripts/fuzz_parsers.py,
      the `webform` group).

      Only "remove" was understood before: Max-Age=0, or an Expires= in a
      year before 2020; every other cookie lived until replaced. Now a
      cookie keeps the second it ends - now plus Max-Age, or the date
      Expires= gives, Max-Age winning - measured against the clock
      (httpget reads the RTC). Dates are read by RFC 6265's own forgiving
      rules (a time, a day, a month and a year among the tokens, two-
      digit years 70-99 in the 1900s), which take RFC 1123, RFC 850 and
      asctime dates alike. A cookie whose time is over is not sent, and
      leaves the jar when it is written. The jar file has a version line
      now and an expiry field; a jar from before still loads.

      `make fuzz` holds the date reader against Python's calendar: 900
      dates in the three forms, 1970 to 2069, must give the seconds
      timegm gives. The QEMU test sets a cookie for twenty seconds, one
      for a year on and one dated the day before the clock (after 2020,
      so only the clock can tell): the first two come back at once and
      only the year-long one after the twenty seconds; the stale one
      never. Before this change all three came back both times.

- [x] **GIF pictures** (user/ugif.h, apps/browser/web.cpp, `make fuzz`,
      the `web` group).

      A GIF decoder of its own header, like ujpeg.h: the screen, the
      global and local palettes, a transparent index from the graphic
      control block, interlaced rows, and LZW rebuilt code by code up to
      twelve bits with its clear code. The first frame of an animation is
      what a page shows. Every index and length out of the file is
      checked; a file cut short gives what was decoded. The browser
      takes GIF wherever it takes PNG and JPEG - pictures, backgrounds,
      site icons - and shows .gif addresses rather than saving them.

      `make fuzz` gained a gif target, and holds the decoder to Pillow:
      a 256-colour gradient plain and interlaced and a four-colour one
      with a transparent index, written by Pillow, must decode to the
      very RGBA bytes Pillow reads back. The QEMU test draws a Pillow
      GIF at twice its size: three quarters 32x24 each in their colours,
      side by side, and the transparent one not drawn at all.

- [x] **TLS 1.2, for the servers that have not moved on** (user/utls.h,
      user/uecdsa.h, user/uhttp.h, user/httpget.c, scripts/ptest.py,
      `tls64`).

      Only TLS 1.3 was spoken, and a server without it could not be
      reached at all. The ClientHello now offers 1.2 as well, and a
      ServerHello without 1.3's supported_versions takes the 1.2 road:
      the Certificate (1.2's bare list), the ServerKeyExchange - the
      server's ephemeral key, signed with its certificate's key over both
      randoms, checked like 1.3's CertificateVerify by the same code -
      ServerHelloDone; our ClientKeyExchange, ChangeCipherSpec and
      Finished; the server's. Keys come from 1.2's PRF (P_SHA256), with
      the master secret bound to the handshake (RFC 7627) when the server
      agrees. Records carry their number and header in the AEAD's
      additional data; AES-GCM takes eight bytes of its nonce from the
      record. Suites: ECDHE with RSA or ECDSA, ChaCha20-Poly1305 or
      AES-128-GCM. A 1.3-capable server pushed to 1.2 shows it in its
      random ("DOWNGRD"), and we stop.

      Testing with an ECDSA certificate showed that 1.2 reads the
      supported groups as the curves a certificate may be on: x25519
      alone made such servers refuse us. P-256 and P-384 are offered now,
      and since a 1.2 server may then exchange keys on them, uecdsa.h
      gained ECDH (d*G, d*Q) on both - variable-time, which is acceptable
      for keys used once.

      The test runs OpenSSL's s_server at TLS 1.2 with one suite and one
      group each - RSA with ChaCha20 on x25519, RSA with AES-GCM on
      P-256, ECDSA with ChaCha20 on P-384, ECDSA with AES-GCM on x25519 -
      with its certificate as the guest's only root: every time "TLS 1.2
      agreed", the certificate and the signature checked, the page whole.
      A 1.2 server with another certificate is refused. All the TLS 1.3
      checks still pass; httpget now says which version was agreed.

- [x] **JavaScript in the browser, first stage** (apps/browser/webjs.cpp,
      apps/browser/webjs.h, apps/browser/web.cpp,
      apps/browser/patches/document.patch, Makefile, the `webjs` group).

      Pages' scripts run in QuickJS - the engine `js` already had - one
      context per page (per tab), one runtime for the browser. The
      scripts in a page run in order once it is laid out, from their src
      or inline; then DOMContentLoaded and load. The DOM is split in two:
      what has to touch litehtml is native and small (find elements by
      id or selector, read and write text, markup and attributes, add and
      remove elements, timers); what pages expect to find - textContent
      and innerHTML as properties, style as an object writing the style
      attribute, classList, dataset, addEventListener and onclick (as a
      property or an attribute), events bubbling to the document,
      setTimeout and setInterval, console, alert, location, document.title,
      an in-memory localStorage - is JavaScript on top of it. One JS
      object per element, so === and properties a script puts on one
      hold. A click on the page reaches the scripts first and
      preventDefault() keeps a link or a button from acting; a script
      sending the window elsewhere does so after it returns. A script
      that runs on for four seconds is stopped, and memory is capped at
      64 MiB.

      After a script changes the tree, the page is styled again from its
      style sheets and laid out. litehtml keeps, per element, the
      selectors that matched it - a list a script's changes cannot
      follow (a class added can make a selector match that never did) -
      so the browser builds litehtml's document.cpp from a patch whose
      media_changed() drops those lists, matches the sheets afresh and
      rebuilds the render tree. Nothing in litehtml calls it. litehtml's
      own <script> element kept neither its attributes nor its text
      where anything could reach them; the browser makes <script> a
      plain tag instead.

      Not yet: document.write, events other than click (no input,
      change, submit, keys), XMLHttpRequest and fetch, getBoundingClientRect,
      and innerHTML read back carries only the common attributes.

      The test page takes a function from a script file and calls it to
      set the title; at DOMContentLoaded writes a paragraph's text and
      adds one of its own in a colour no style sheet has - it shows; a
      click on a link adds a class the page's sheet paints (the box
      appears, 120 pixels wide) and prevents the link; a timer paints
      another box later; console.log reaches the serial line; and a
      `while (true)` is stopped while its page still shows.

- [x] **The browser's own pages: menu, history, cookies, settings**
      (apps/browser/web.cpp, scripts/tests/t_webui.py)
      The toolbar is drawn anew: flat icons, a rounded address field
      with a padlock for https, and at its end a menu (⋮) with New tab,
      History, Bookmarks, Cookies, Settings, Find and About. All but New
      tab and Find open a page of the browser's own, built in memory:
      about:history (searchable, entries removable, or all of it),
      about:bookmarks, about:cookies (the jar as it is, with expiry
      dates, removable one by one or all at once), about:settings and
      about:version. The settings - JavaScript on or off, images on or
      off, the search engine, the home page, the zoom - are a form that
      submits to about:settings itself and are kept in .web-settings.txt.

      These pages act on what their address says (?clear=all empties the
      history), so a page from the network must not reach them: a link,
      or a form's action, to about: from any page that is not about: is
      refused and said so on the status line. The browser's pages run no
      scripts, are not remembered in the history and cannot be
      bookmarked.

      The test opens the menu and picks History: it lists the two pages
      seen, and its "Clear the history" empties it. Cookies lists the one
      a site set; "Remove all cookies" removes it, and the site's next
      request carries no Cookie header, by the server's own account. In
      Settings a click unticks JavaScript and the button saves it; the
      next page's scripts do not run, and once ticked again they do. A
      served page's link to about:history?clear=all is refused and the
      history is still there.
