# ThreadOS - the build
#
# Targets (BUILD=<dir> puts everything somewhere other than build/):
#   make            - the 32-bit kernel and its initrd
#   make kernel64   - the 64-bit kernel and its initrd
#   make efi        - threadboot, the UEFI loader (needs clang + lld)
#   make iso64      - the hybrid BIOS + UEFI disc, the installer included
#   make iso        - a disc with the 32-bit kernel
#   make stick      - a bootable USB image (GPT + an EFI partition)
#   make run        - the 64-bit system in QEMU, from its disc, to the desktop
#   make run32      - the 32-bit kernel in QEMU
#   make test       - the test suite (T=group,... selects, J=n runs n at once)
#   make fuzz       - feeds the parsers corrupted data (under sanitizers)
#   make clean      - removes what was built
#
# VERSION=<n> is the monotonic update sequence; VERSION_LABEL is the
# human-facing pre-1.0 version shown by the system.

CC      := gcc
CXX     := g++
AS      := gcc
LD      := gcc

# -Wshadow earns its place: renaming a variable to 'count' inside a
# function whose parameter is already 'count' compiled silently and
# broke the NVMe driver.
CFLAGS  := -m32 -std=gnu11 -ffreestanding -fno-stack-protector -fno-pic \
           -Wall -Wextra -Wshadow -O2 -g
ASFLAGS := -m32
# One day it will be worth linking with -lgcc (which needs gcc-multilib
# with a 32-bit libgcc), for instance when 64-bit division turns up. So
# far the kernel does without it.
# --no-warn-rwx-segments: the .user section deliberately holds code and
# data together (RWX)
LDFLAGS := -m32 -ffreestanding -nostdlib -static -no-pie -T kernel/linker.ld \
           -Wl,--build-id=none -Wl,--no-warn-rwx-segments

BUILD   := build
# The fonts in both initrds - cut out of DejaVu by scripts/mkfonts.py and
# committed, so building needs neither fonttools nor the DejaVu package
FONTS   := fonts/sans.ttf fonts/bold.ttf fonts/mono.ttf
KERNEL  := $(BUILD)/threados.bin
ISO     := $(BUILD)/threados.iso
INITRD  := $(BUILD)/initrd.img
DISK    := $(BUILD)/disk.img

# The user programs (ELF files loaded out of the initrd)
USER_CFLAGS := -m32 -std=gnu11 -ffreestanding -fno-stack-protector \
               -fno-pic -no-pie -static -nostdlib -O2 -Wall -Wextra \
               -Wl,--build-id=none -Wl,--no-warn-rwx-segments
# The 32-bit ABI lowers some uint64_t division to libgcc helpers. These
# freestanding programs link the compiler runtime, but still no libc.
LIBGCC32 := $(shell $(CC) -m32 -print-libgcc-file-name)
USER_ELFS := $(BUILD)/user/hello.elf $(BUILD)/user/evil.elf \
             $(BUILD)/user/evil2.elf \
             $(BUILD)/user/sh.elf $(BUILD)/user/echo.elf \
             $(BUILD)/user/memtest.elf $(BUILD)/user/fileio.elf \
             $(BUILD)/user/cat.elf $(BUILD)/user/wc.elf \
             $(BUILD)/user/seq.elf $(BUILD)/user/grep.elf \
             $(BUILD)/user/head.elf $(BUILD)/user/ls.elf \
             $(BUILD)/user/rm.elf $(BUILD)/user/date.elf \
             $(BUILD)/user/cp.elf $(BUILD)/user/mv.elf \
             $(BUILD)/user/mkdir.elf $(BUILD)/user/spin.elf \
             $(BUILD)/user/core.elf \
             $(BUILD)/user/guiapp.elf $(BUILD)/user/gclock.elf \
             $(BUILD)/user/mmaptest.elf $(BUILD)/user/gfiles.elf \
             $(BUILD)/user/shmtest.elf $(BUILD)/user/shmread.elf \
             $(BUILD)/user/mkwall.elf \
             $(BUILD)/user/gmon.elf $(BUILD)/user/threads.elf $(BUILD)/user/prio.elf $(BUILD)/user/gterm.elf \
             $(BUILD)/user/gedit.elf $(BUILD)/user/gtasks.elf \
             $(BUILD)/user/gcode.elf \
             $(BUILD)/user/udtest.elf $(BUILD)/user/gappear.elf \
             $(BUILD)/user/gcalc.elf $(BUILD)/user/gcal.elf \
             $(BUILD)/user/gpaint.elf \
             $(BUILD)/user/gusers.elf \
             $(BUILD)/user/gimg.elf \
             $(BUILD)/user/udpecho.elf $(BUILD)/user/udptest.elf \
             $(BUILD)/user/nslookup.elf \
             $(BUILD)/user/httpget.elf $(BUILD)/user/tcpserv.elf \
             $(BUILD)/user/gweb.elf \
             $(BUILD)/user/cryptotest.elf \
             $(BUILD)/user/certtest.elf \
             $(BUILD)/user/aestest.elf \
             $(BUILD)/user/wpatest.elf \
             $(BUILD)/user/forktest.elf $(BUILD)/user/ptree.elf $(BUILD)/user/sigtest.elf $(BUILD)/user/lazymem.elf $(BUILD)/user/swaptest.elf \
             $(BUILD)/user/mkmap.elf $(BUILD)/user/mmapfile.elf \
             $(BUILD)/user/deep.elf $(BUILD)/user/mmapzap.elf \
             $(BUILD)/user/tlbtest.elf $(BUILD)/user/faults.elf \
             $(BUILD)/user/gsettings.elf $(BUILD)/user/gdeaf.elf \
             $(BUILD)/user/ginstal.elf \
             $(BUILD)/user/gmikser.elf \
             $(BUILD)/user/dynlib.elf $(BUILD)/user/cores.elf \
             $(BUILD)/user/mutex.elf $(BUILD)/user/prodcons.elf \
             $(BUILD)/user/syscalls.elf \
             $(BUILD)/user/lspci.elf $(BUILD)/user/ifconfig.elf

OBJS := $(BUILD)/boot.o \
        $(BUILD)/isr_stubs.o \
        $(BUILD)/kmain.o \
        $(BUILD)/vga.o \
        $(BUILD)/serial.o \
        $(BUILD)/printk.o \
        $(BUILD)/locale.o \
        $(BUILD)/bootsplash.o \
        $(BUILD)/string.o \
        $(BUILD)/gdt.o \
        $(BUILD)/idt.o \
        $(BUILD)/isr.o \
        $(BUILD)/pic.o \
        $(BUILD)/pit.o \
        $(BUILD)/keyboard.o \
        $(BUILD)/kbdmap.o \
        $(BUILD)/pmm.o \
        $(BUILD)/paging.o \
        $(BUILD)/heap.o \
        $(BUILD)/shell.o \
        $(BUILD)/switch.o \
        $(BUILD)/task.o \
        $(BUILD)/syscall.o \
        $(BUILD)/usermode.o \
        $(BUILD)/initrd.o \
        $(BUILD)/exec.o \
        $(BUILD)/prog.o \
        $(BUILD)/ata.o \
        $(BUILD)/fs.o \
        $(BUILD)/kfile.o \
        $(BUILD)/pipe.o \
        $(BUILD)/sem.o \
        $(BUILD)/rtc.o \
        $(BUILD)/mouse.o \
        $(BUILD)/video.o \
        $(BUILD)/desktop.o \
        $(BUILD)/pci.o \
        $(BUILD)/shm.o \
        $(BUILD)/rtl8139.o \
        $(BUILD)/e1000.o \
        $(BUILD)/virtio_net.o \
        $(BUILD)/netdev.o \
        $(BUILD)/smp.o \
        $(BUILD)/ap.o \
        $(BUILD)/net.o \
        $(BUILD)/dhcp.o \
        $(BUILD)/tcp.o \
        $(BUILD)/signal.o \
        $(BUILD)/swap.o \
        $(BUILD)/ahci.o \
        $(BUILD)/nvme.o \
        $(BUILD)/blk.o \
        $(BUILD)/uhci.o \
        $(BUILD)/ehci.o \
        $(BUILD)/ttf.o \
        $(BUILD)/fonts.o \
        $(BUILD)/uitext.o \
        $(BUILD)/gfx.o \
        $(BUILD)/aurora.o \
        $(BUILD)/xhci.o \
        $(BUILD)/usbhc.o \
        $(BUILD)/usbkbd.o \
        $(BUILD)/usbmouse.o \
        $(BUILD)/usbmass.o \
        $(BUILD)/bt.o \
        $(BUILD)/usbnet.o \
        $(BUILD)/part.o \
        $(BUILD)/fat.o \
        $(BUILD)/gpt.o \
        $(BUILD)/fatcmd.o \
        $(BUILD)/install.o \
        $(BUILD)/hwinfo.o \
        $(BUILD)/hda.o \
        $(BUILD)/dynlink.o \
        $(BUILD)/smpmem.o \
        $(BUILD)/smpthreads.o \
        $(BUILD)/biglock.o \
        $(BUILD)/tlb.o \
        $(BUILD)/config.o \
        $(BUILD)/user.o \
        $(BUILD)/clip.o \
        $(BUILD)/acpi.o \
        $(BUILD)/mb2.o \
        $(BUILD)/fbcon.o

.PHONY: all iso iso64 initrd64 libc64 browser run run32 run-iso run-iso64-uefi test fuzz clean \
        kernel64 efi stick run-stick

all: $(KERNEL) $(INITRD)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/boot.o: boot/boot.S | $(BUILD)
	$(AS) $(ASFLAGS) -c $< -o $@

# The secondary cores' trampoline goes through gcc rather than as: it
# has preprocessor macros in it that compute addresses at the place it
# is copied to
$(BUILD)/ap.o: boot/ap.S | $(BUILD)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/isr_stubs.o: kernel/isr.S | $(BUILD)
	$(AS) $(ASFLAGS) -c $< -o $@

$(BUILD)/switch.o: kernel/switch.S | $(BUILD)
	$(AS) $(ASFLAGS) -c $< -o $@

# The dependency on the headers is spelled out here: without it a
# change in kernel/include/ rebuilt nothing and make ended green, while
# the objects still remembered the old prototype.
KERNEL_HEADERS := $(wildcard kernel/include/*.h)
USERCTL_HEADER := user/userctl.h

$(BUILD)/%.o: kernel/%.c $(KERNEL_HEADERS) $(USERCTL_HEADER) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(KERNEL): $(OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(OBJS) -o $@
	@grub-file --is-x86-multiboot $@ 2>/dev/null \
	    && echo "OK: $@ is a valid Multiboot image" \
	    || echo "INFO: no grub-file - skipping the Multiboot check"

$(BUILD)/user: | $(BUILD)
	mkdir -p $(BUILD)/user

# The userspace headers are pulled in by many programs (the syscall
# library, the network, the cryptography), so a change to any of them
# has to rebuild everything - hence a wildcard instead of a hand-kept
# list.
USER_HEADERS := $(wildcard user/*.h)

$(BUILD)/user/%.elf: user/%.c $(USER_HEADERS) user/user.ld | $(BUILD)/user
	$(CC) $(USER_CFLAGS) -T user/user.ld $< -o $@ $(LIBGCC32)

$(BUILD)/user/gcode.elf: user/gedit.c

# --- The shared library ----------------------------------------------
#
# The cryptography, TLS, HTTP and DNS live in the user/*.h headers as
# 'static inline' functions. Every program used to carry them inside
# itself - httpget and gweb are eighty kilobytes each of the same code.
#
# There is still one source. scripts/split_headers.py makes two
# versions of every header: one for the library (the same functions,
# but visible from outside) and one for a program (declarations only,
# no bodies). Neither is written by hand, so neither can drift from the
# source.
#
# Copying the sources into build/ is not decoration: '#include
# "utls.h"' looks first in the directory of the file that includes it.
# If libthread.c lived in user/, it would find the original instead of
# the generated version.
LIB_HEADERS := ubignum.h usha256.h usha512.h uchacha.h uaes.h urand.h ux25519.h \
               ursa.h uecdsa.h ux509.h ucert.h utls.h uhttp.h uhtml.h \
               udns.h caroots.h uthread.h
LIB_SRC  := $(patsubst %,$(BUILD)/libsrc/%,$(LIB_HEADERS))
LIB_DECL := $(patsubst %,$(BUILD)/decl/%,$(LIB_HEADERS))
LIBTHREAD := $(BUILD)/libthread.so

$(BUILD)/libsrc $(BUILD)/decl $(BUILD)/dyn:
	mkdir -p $@

$(BUILD)/libsrc/%.h: user/%.h scripts/split_headers.py | $(BUILD)/libsrc
	python3 scripts/split_headers.py --lib $< > $@

$(BUILD)/decl/%.h: user/%.h scripts/split_headers.py | $(BUILD)/decl
	python3 scripts/split_headers.py --decl $< > $@

$(BUILD)/libsrc/libthread.c: user/libthread.c | $(BUILD)/libsrc
	cp $< $@

# -Bsymbolic: calls inside the library are bound at link time. Without
# it they would go through its own jump table, and that is filled in at
# load time - which here there is nobody to do.
$(LIBTHREAD): $(BUILD)/libsrc/libthread.c $(LIB_SRC) user/libthread.ld \
              $(USER_HEADERS) Makefile
	$(CC) -m32 -std=gnu11 -ffreestanding -fno-stack-protector -fPIC -O2 \
	    -Wall -Wextra -I$(BUILD)/libsrc -Iuser -c $< -o $(BUILD)/libthread.o
	$(CC) -m32 -shared -nostdlib -Wl,--build-id=none -Wl,-Bsymbolic \
	    -Wl,-soname,libthread.so -Wl,--no-warn-rwx-segments \
	    -T user/libthread.ld -o $@ $(BUILD)/libthread.o

$(BUILD)/dyn/%.c: user/%.c | $(BUILD)/dyn
	cp $< $@

# The programs that take the cryptography from the library instead of
# carrying it themselves
USER_DYN_CFLAGS := -m32 -std=gnu11 -ffreestanding -fno-stack-protector \
                   -fno-pic -no-pie -nostdlib -O2 -Wall -Wextra \
                   -Wl,--build-id=none -Wl,--no-warn-rwx-segments

# A pattern rule limited to those five names: the rest of the programs
# build as they always did, with everything inside
USER_DYN := cryptotest certtest httpget gweb nslookup dynlib
USER_DYN_ELFS := $(patsubst %,$(BUILD)/user/%.elf,$(USER_DYN))

$(USER_DYN_ELFS): $(BUILD)/user/%.elf: $(BUILD)/dyn/%.c $(LIB_DECL) \
                     $(LIBTHREAD) user/userdyn.ld Makefile | $(BUILD)/user
	$(CC) $(USER_DYN_CFLAGS) -I$(BUILD)/decl -Iuser -T user/userdyn.ld \
	    $< $(LIBTHREAD) $(LIBGCC32) -o $@

$(INITRD): $(USER_ELFS) $(LIBTHREAD) scripts/mkinitrd.py $(FONTS)
	python3 scripts/mkinitrd.py $@ \
	    libthread.so=$(LIBTHREAD) \
	    sans.ttf=fonts/sans.ttf bold.ttf=fonts/bold.ttf mono.ttf=fonts/mono.ttf \
	    hello=$(BUILD)/user/hello.elf evil=$(BUILD)/user/evil.elf \
	    evil2=$(BUILD)/user/evil2.elf \
	    sh=$(BUILD)/user/sh.elf echo=$(BUILD)/user/echo.elf \
	    memtest=$(BUILD)/user/memtest.elf fileio=$(BUILD)/user/fileio.elf \
	    cat=$(BUILD)/user/cat.elf wc=$(BUILD)/user/wc.elf \
	    seq=$(BUILD)/user/seq.elf grep=$(BUILD)/user/grep.elf \
	    head=$(BUILD)/user/head.elf ls=$(BUILD)/user/ls.elf \
	    rm=$(BUILD)/user/rm.elf date=$(BUILD)/user/date.elf \
	    cp=$(BUILD)/user/cp.elf mv=$(BUILD)/user/mv.elf \
	    mkdir=$(BUILD)/user/mkdir.elf spin=$(BUILD)/user/spin.elf \
	    core=$(BUILD)/user/core.elf \
	    guiapp=$(BUILD)/user/guiapp.elf gclock=$(BUILD)/user/gclock.elf \
	    mmaptest=$(BUILD)/user/mmaptest.elf gfiles=$(BUILD)/user/gfiles.elf \
	    shmtest=$(BUILD)/user/shmtest.elf shmread=$(BUILD)/user/shmread.elf \
	    gmon=$(BUILD)/user/gmon.elf threads=$(BUILD)/user/threads.elf \
	    prio=$(BUILD)/user/prio.elf gterm=$(BUILD)/user/gterm.elf \
	    gedit=$(BUILD)/user/gedit.elf gtasks=$(BUILD)/user/gtasks.elf \
	    gcode=$(BUILD)/user/gcode.elf \
	    udtest=$(BUILD)/user/udtest.elf gappear=$(BUILD)/user/gappear.elf \
	    gcalc=$(BUILD)/user/gcalc.elf gcal=$(BUILD)/user/gcal.elf \
	    gpaint=$(BUILD)/user/gpaint.elf \
	    gusers=$(BUILD)/user/gusers.elf \
	    gimg=$(BUILD)/user/gimg.elf \
	    udpecho=$(BUILD)/user/udpecho.elf udptest=$(BUILD)/user/udptest.elf \
	    nslookup=$(BUILD)/user/nslookup.elf \
	    httpget=$(BUILD)/user/httpget.elf tcpserv=$(BUILD)/user/tcpserv.elf \
	    gweb=$(BUILD)/user/gweb.elf \
	    cryptotest=$(BUILD)/user/cryptotest.elf \
	    certtest=$(BUILD)/user/certtest.elf \
	    aestest=$(BUILD)/user/aestest.elf \
	    wpatest=$(BUILD)/user/wpatest.elf \
	    forktest=$(BUILD)/user/forktest.elf ptree=$(BUILD)/user/ptree.elf \
	    sigtest=$(BUILD)/user/sigtest.elf \
	    lazymem=$(BUILD)/user/lazymem.elf \
	    swaptest=$(BUILD)/user/swaptest.elf \
	    mkmap=$(BUILD)/user/mkmap.elf mmapfile=$(BUILD)/user/mmapfile.elf \
	    deep=$(BUILD)/user/deep.elf tlbtest=$(BUILD)/user/tlbtest.elf \
	    faults=$(BUILD)/user/faults.elf \
	    mkwall=$(BUILD)/user/mkwall.elf \
	    gsettings=$(BUILD)/user/gsettings.elf gdeaf=$(BUILD)/user/gdeaf.elf \
	    ginstal=$(BUILD)/user/ginstal.elf \
	    gmikser=$(BUILD)/user/gmikser.elf \
	    mmapzap=$(BUILD)/user/mmapzap.elf \
	    dynlib=$(BUILD)/user/dynlib.elf \
	    cores=$(BUILD)/user/cores.elf \
	    mutex=$(BUILD)/user/mutex.elf prodcons=$(BUILD)/user/prodcons.elf \
	    syscalls=$(BUILD)/user/syscalls.elf \
	    lspci=$(BUILD)/user/lspci.elf hardware=$(BUILD)/user/lspci.elf \
	    ifconfig=$(BUILD)/user/ifconfig.elf

# --- The 64-bit kernel (long mode) -----------------------------------
#
# A second image built from the same sources: only the entry point
# (boot64/boot64.S enters long mode) and the kmain64 skeleton differ.
# The rest is the same files compiled a second time - what separates
# the two modes fits in the io.h header and a handful of casts.
KERNEL64 := $(BUILD)/threados64.bin
CFLAGS64 := -m64 -std=gnu11 -ffreestanding -fno-stack-protector -fno-pic \
            -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -Wall -Wextra -Wshadow -O2 -g
LDFLAGS64 := -m64 -ffreestanding -nostdlib -static -no-pie -T kernel64/linker64.ld \
             -Wl,--build-id=none -Wl,--no-warn-rwx-segments

SHARED64 := string serial printk locale bootsplash mb2 fbcon vga pic pit pmm heap initrd \
            kbdmap pci ata ahci nvme blk fs rtl8139 net dhcp tcp video \
            mouse desktop acpi uhci ehci xhci usbhc ttf fonts uitext gfx aurora usbkbd usbmouse usbmass bt usbnet part fat gpt fatcmd install hwinfo hda swap prog shm sem rtc \
            dynlink smpmem smpthreads biglock tlb config user clip \
            e1000 virtio_net netdev smp
OBJS64 := $(BUILD)/64/boot64.o $(BUILD)/64/isr64_stubs.o \
          $(BUILD)/64/switch64.o \
          $(BUILD)/64/kmain64.o $(BUILD)/64/idt64.o $(BUILD)/64/isr64.o \
          $(BUILD)/64/paging64.o $(BUILD)/64/task64.o \
          $(BUILD)/64/gdt64.o $(BUILD)/64/syscall64.o \
          $(BUILD)/64/elf64.o $(BUILD)/64/proc64.o \
          $(BUILD)/64/keyboard64.o $(BUILD)/64/shell64.o \
          $(BUILD)/64/signal64.o $(BUILD)/64/fpu64.o \
          $(BUILD)/64/pipe64.o $(BUILD)/64/compat64.o $(BUILD)/64/efirt.o \
          $(BUILD)/64/syscall64_stub.o \
          $(BUILD)/64/ap64.o \
          $(addprefix $(BUILD)/64/,$(addsuffix .o,$(SHARED64)))

$(BUILD)/64:
	mkdir -p $(BUILD)/64

$(BUILD)/64/boot64.o: boot64/boot64.S | $(BUILD)/64
	$(CC) -m64 -c $< -o $@

$(BUILD)/64/isr64_stubs.o: boot64/isr64.S | $(BUILD)/64
	$(CC) -m64 -c $< -o $@

$(BUILD)/64/ap64.o: boot64/ap64.S | $(BUILD)/64
	$(CC) -m64 -c $< -o $@

$(BUILD)/64/switch64.o: boot64/switch64.S | $(BUILD)/64
	$(CC) -m64 -c $< -o $@

$(BUILD)/64/syscall64_stub.o: boot64/syscall64.S | $(BUILD)/64
	$(CC) -m64 -c $< -o $@

# The long-mode user programs. They build just like the 32-bit ones,
# only with a different linker script and their own syscall library -
# in long mode the kernel is entered with the syscall instruction, not
# through an interrupt gate.
INITRD64 := $(BUILD)/initrd64.img
# The user programs are built with SSE - on x86-64 it is part of the
# calling convention, and the kernel can already save and restore those
# registers when it switches tasks. The kernel itself stays without
# SSE: there is nothing to compute in an interrupt handler, and every
# such instruction would mean saving the interrupted program's state.
USER64_CFLAGS := -m64 -std=gnu11 -ffreestanding -fno-stack-protector \
                 -fno-pic -no-pie -static -nostdlib -mno-red-zone \
                 -O2 -Wall -Wextra \
                 -Wl,--build-id=none -Wl,-z,max-page-size=0x1000
USER64_ELFS := $(BUILD)/user64/hello64.elf $(BUILD)/user64/nx64.elf \
               $(BUILD)/user64/wild64.elf $(BUILD)/user64/sep64.elf \
               $(BUILD)/user64/fork64.elf $(BUILD)/user64/spawn64.elf \
               $(BUILD)/user64/sigtest64.elf $(BUILD)/user64/spin64.elf \
               $(BUILD)/user64/ush64.elf $(BUILD)/user64/fileio64.elf \
               $(BUILD)/user64/uecho64.elf $(BUILD)/user64/ucat64.elf \
               $(BUILD)/user64/heap64.elf $(BUILD)/user64/fpu64test.elf \
               $(BUILD)/user64/uls64.elf $(BUILD)/user64/uupper64.elf \
               $(BUILD)/user64/unet64.elf \
               $(BUILD)/user64/guiapp64.elf \
               $(BUILD)/user64/swaptest64.elf \
               $(BUILD)/user64/shmwrite64.elf \
               $(BUILD)/user64/shmread64.elf \
               $(BUILD)/user64/gclock.elf \
               $(BUILD)/user64/gfiles.elf \
               $(BUILD)/user64/gmon.elf \
               $(BUILD)/user64/gterm.elf $(BUILD)/user64/core.elf \
               $(BUILD)/user64/gedit.elf \
               $(BUILD)/user64/gcode.elf \
               $(BUILD)/user64/gtasks.elf \
               $(BUILD)/user64/udtest.elf \
               $(BUILD)/user64/gcalc.elf $(BUILD)/user64/gcal.elf \
               $(BUILD)/user64/gpaint.elf \
               $(BUILD)/user64/gusers.elf \
               $(BUILD)/user64/gsettings.elf $(BUILD)/user64/gappear.elf \
               $(BUILD)/user64/ginstal.elf \
               $(BUILD)/user64/gmikser.elf \
               $(BUILD)/user64/gimg.elf \
               $(BUILD)/user64/gmedia.elf \
               $(BUILD)/user64/cryptotest.elf \
               $(BUILD)/user64/certtest.elf \
               $(BUILD)/user64/aestest.elf \
               $(BUILD)/user64/wpatest.elf \
               $(BUILD)/user64/nslookup.elf \
               $(BUILD)/user64/httpget.elf \
               $(BUILD)/user64/gweb.elf \
               $(BUILD)/user64/mkmap64.elf \
               $(BUILD)/user64/tlbtest64.elf \
               $(BUILD)/user64/mmapfile64.elf \
               $(BUILD)/user64/mmapzap64.elf \
               $(BUILD)/user64/dynlib.elf \
               $(BUILD)/user64/cores.elf \
               $(BUILD)/user64/mutex.elf \
               $(BUILD)/user64/prodcons.elf \
               $(BUILD)/user64/syscalls.elf \
               $(BUILD)/user64/lspci.elf $(BUILD)/user64/ifconfig.elf

$(BUILD)/user64:
	mkdir -p $(BUILD)/user64

# The Makefile is a dependency here for a reason: a change to the
# compilation flags has to rebuild the programs, and without this entry
# make decides that since the sources did not change there is nothing
# to do.
$(BUILD)/user64/%.elf: user64/%.c user64/usys64.h user64/user64.ld \
                       $(USER_HEADERS) Makefile | $(BUILD)/user64
	$(CC) $(USER64_CFLAGS) -T user64/user64.ld $< -o $@

# The programs shared by both kernels: the cryptography, TLS, HTTP and
# DNS live in user/ and know nothing about the processor mode. They
# speak to the kernel through user/uapi.h, which picks the right side -
# hence this second rule, building the same sources again, this time
# for 64 bits.
$(BUILD)/user64/%.elf: user/%.c user/uapi.h user64/usys64.h \
                       user64/compat64.h user64/user64.ld $(USER_HEADERS) \
                       Makefile | $(BUILD)/user64
	$(CC) $(USER64_CFLAGS) -T user64/user64.ld $< -o $@

$(BUILD)/user64/gcode.elf: user/gedit.c

# --- The shared library in long mode ---------------------------------
#
# The same trick as in the 32-bit kernel and out of the same headers -
# only the address the library sits at and the width of the registers
# differ. Nobody writes the source a second time.
LIBTHREAD64 := $(BUILD)/libthread64.so
LIB_DECL64  := $(patsubst %,$(BUILD)/decl/%,$(LIB_HEADERS))

$(LIBTHREAD64): $(BUILD)/libsrc/libthread.c $(LIB_SRC) user64/libthread64.ld \
                $(USER_HEADERS) Makefile
	$(CC) -m64 -std=gnu11 -ffreestanding -fno-stack-protector -fPIC \
	    -mno-red-zone -O2 -Wall -Wextra -I$(BUILD)/libsrc -Iuser \
	    -c $< -o $(BUILD)/libthread64.o
	$(CC) -m64 -shared -nostdlib -Wl,--build-id=none -Wl,-Bsymbolic \
	    -Wl,-soname,libthread.so -Wl,--no-warn-rwx-segments \
	    -Wl,-z,max-page-size=0x1000 \
	    -T user64/libthread64.ld -o $@ $(BUILD)/libthread64.o

USER64_DYN_CFLAGS := -m64 -std=gnu11 -ffreestanding -fno-stack-protector \
                     -fno-pic -no-pie -nostdlib -mno-red-zone \
                     -O2 -Wall -Wextra \
                     -Wl,--build-id=none -Wl,-z,max-page-size=0x1000

USER64_DYN_ELFS := $(patsubst %,$(BUILD)/user64/%.elf,$(USER_DYN))

$(USER64_DYN_ELFS): $(BUILD)/user64/%.elf: $(BUILD)/dyn/%.c $(LIB_DECL64) \
                     $(LIBTHREAD64) user64/userdyn64.ld Makefile \
                     | $(BUILD)/user64
	$(CC) $(USER64_DYN_CFLAGS) -I$(BUILD)/decl -Iuser \
	    -T user64/userdyn64.ld $< $(LIBTHREAD64) -o $@

# --- A C library for the 64-bit programs: musl ----------------------
#
# The programs above speak to the kernel through usys64.h and carry
# what little of a C library they need with them. Software written
# elsewhere - a JavaScript engine, later an HTML engine - is written
# against C and POSIX, and needs the real thing. That is musl
# (third_party/musl, unchanged upstream 1.2.5), built for x86_64 with a
# handful of its files replaced (libc/overlay): the ones that issued
# the syscall instruction with Linux's numbers now call one function,
# which translates every Linux call into ThreadOS's own
# (libc/overlay/src/threados/syscall.c). musl itself never learns it is
# not on Linux.
#
# The tree is copied into the build directory, the overlay put over the
# copy, and musl's own configure and Makefile do the rest - so a change
# to the overlay rebuilds only what it touched.
MUSL_TREE    := $(BUILD)/musl/tree
MUSL_ROOT    := $(BUILD)/musl/root
MUSL_OVERLAY := $(shell find libc/overlay -type f)
LIBC64       := $(MUSL_ROOT)/lib/libc.a
MUSL_CFLAGS  := -O2 -mno-red-zone -fno-stack-protector -fno-pic \
                -fno-asynchronous-unwind-tables

# A header of the overlay is included all over musl, and musl's
# Makefile does not track headers - so a changed one starts the copy
# afresh.
$(MUSL_TREE)/config.mak: Makefile third_party/musl/VERSION \
                         third_party/musl/tools/mkalltypes.sed \
                         $(wildcard third_party/musl/tools/*.sh) \
                         third_party/musl/arch/x86_64/bits/alltypes.h.in \
                         third_party/musl/include/alltypes.h.in \
                         $(filter %.h,$(MUSL_OVERLAY))
	rm -rf $(MUSL_TREE) && mkdir -p $(MUSL_TREE)
	cp -r third_party/musl/. $(MUSL_TREE)/
	sed -i 's/\r$$//' $(MUSL_TREE)/configure \
	    $(MUSL_TREE)/VERSION \
	    $(MUSL_TREE)/tools/mkalltypes.sed \
	    $(MUSL_TREE)/tools/*.sh \
	    $(MUSL_TREE)/arch/x86_64/bits/alltypes.h.in \
	    $(MUSL_TREE)/include/alltypes.h.in
	cd $(MUSL_TREE) && CC="$(CC)" AR=ar RANLIB=ranlib CFLAGS="$(MUSL_CFLAGS)" ./configure \
	    --target=x86_64 --prefix=/ --disable-shared >/dev/null

$(LIBC64): $(MUSL_TREE)/config.mak $(MUSL_OVERLAY)
	cp -r libc/overlay/. $(MUSL_TREE)/
	$(MAKE) -s -C $(MUSL_TREE) lib/libc.a lib/crt1.o lib/crti.o lib/crtn.o
	$(MAKE) -s -C $(MUSL_TREE) install-libs install-headers \
	    DESTDIR=$(abspath $(MUSL_ROOT))

libc64: $(LIBC64)

# FFmpeg is built as a small static decoder set for the 64-bit media
# player. Its upstream source archive is pinned in third_party; only the
# containers and codecs the desktop opens are enabled.
FFMPEG_TREE := $(BUILD)/ffmpeg64
FFMPEG_CONFIG := $(FFMPEG_TREE)/config.h
FFMPEG_LIBS := $(FFMPEG_TREE)/libavformat/libavformat.a \
               $(FFMPEG_TREE)/libavcodec/libavcodec.a \
               $(FFMPEG_TREE)/libswscale/libswscale.a \
               $(FFMPEG_TREE)/libswresample/libswresample.a \
               $(FFMPEG_TREE)/libavutil/libavutil.a
FFMPEG_STAMP := $(FFMPEG_TREE)/threados-libs.stamp
FFMPEG_COMMA := ,

$(FFMPEG_CONFIG): third_party/ffmpeg-9.0.tar.xz Makefile $(LIBC64)
	rm -rf $(FFMPEG_TREE) && mkdir -p $(FFMPEG_TREE)
	tar -xf $< -C $(FFMPEG_TREE) --strip-components=1
	cd $(FFMPEG_TREE) && ./configure --prefix=/ --arch=x86_64 \
	    --target-os=none --cc="$(CC)" --ar=ar --ranlib=ranlib \
	    --disable-programs --disable-doc --disable-network \
	    --disable-everything --disable-asm --disable-x86asm \
	    --disable-pthreads --disable-w32threads --disable-os2threads \
	    --disable-autodetect --disable-debug --disable-gpl \
	    --disable-version3 --enable-static --disable-shared \
	    --enable-demuxer=mov,mp3,wav,aac,flac,ogg,matroska,avi \
	    --enable-decoder=h264,hevc,vp8,vp9,aac,mp3,flac,vorbis,opus,pcm_s16le,pcm_s24le,pcm_s32le,pcm_u8,mjpeg,png,bmp \
	    --enable-parser=h264,hevc,aac,mpegaudio,vorbis,opus \
	    --enable-swscale --enable-swresample \
	    --extra-cflags="-m64 -std=gnu11 -O2 -mno-red-zone -fno-stack-protector -fno-pic -fno-asynchronous-unwind-tables -nostdinc -isystem $(abspath $(MUSL_ROOT)/include) -isystem $(shell $(CC) -print-file-name=include) -D_GNU_SOURCE" \
	    --extra-ldflags="-no-pie" --disable-logging \
	    > configure.log 2>&1 || { cat configure.log; exit 1; }

$(FFMPEG_STAMP): $(FFMPEG_CONFIG)
	$(MAKE) -s -C $(FFMPEG_TREE) $(patsubst $(FFMPEG_TREE)/%,%,$(FFMPEG_LIBS))
	touch $@

$(FFMPEG_LIBS): $(FFMPEG_STAMP)

$(BUILD)/user64/gmedia.elf: user64/gmedia.c user/uapi.h user/udraw.h \
                             user64/usys64.h user64/compat64.h \
                             $(FFMPEG_LIBS) $(LIBC64) libc/libc64.ld \
                             Makefile | $(BUILD)/user64 $(BUILD)/libc64
	$(CC) $(LIBC64_CFLAGS) -Iuser -I$(FFMPEG_TREE) -D_GNU_SOURCE \
	    -DTHREADOS_LIBC \
	    -Wall -Wextra -c $< -o $(BUILD)/libc64/gmedia.o
	$(call LIBC64_LINK,$(BUILD)/libc64/gmedia.o \
	    -Wl$(FFMPEG_COMMA)--start-group $(FFMPEG_LIBS) \
	    -Wl$(FFMPEG_COMMA)--end-group \
	    $(MUSL_ROOT)/lib/libm.a) -o $@

# Programs against musl: its headers first, the compiler's own after
# them (for what a C library does not carry - stdatomic.h, the vector
# intrinsics), musl's start files around the objects, libgcc for the
# arithmetic the processor has no instruction for.
LIBC64_CFLAGS := -m64 -std=gnu11 -O2 -mno-red-zone -fno-stack-protector \
                 -fno-pic -fno-asynchronous-unwind-tables \
                 -nostdinc -isystem $(MUSL_ROOT)/include \
                 -isystem $(shell $(CC) -print-file-name=include)
LIBC64_LINK = $(CC) -m64 -static -no-pie -nostdlib -s \
              -Wl,--build-id=none -Wl,-z,max-page-size=0x1000 \
              -T libc/libc64.ld $(MUSL_ROOT)/lib/crt1.o \
              $(MUSL_ROOT)/lib/crti.o $(1) $(LIBC64) \
              $(shell $(CC) -print-libgcc-file-name) $(MUSL_ROOT)/lib/crtn.o

$(BUILD)/libc64:
	mkdir -p $(BUILD)/libc64

# The C programs that test the library itself
$(BUILD)/user64/libctest.elf: libc/tests/libctest.c $(LIBC64) libc/libc64.ld \
                              | $(BUILD)/user64 $(BUILD)/libc64
	$(CC) $(LIBC64_CFLAGS) -Wall -Wextra -Wno-infinite-recursion \
	    -Wno-format-truncation -c $< -o $(BUILD)/libc64/libctest.o
	$(call LIBC64_LINK,$(BUILD)/libc64/libctest.o) -o $@

# --- JavaScript: QuickJS ----------------------------------------------
#
# Fabrice Bellard's engine (third_party/quickjs, upstream 2026-06-04),
# its core only - the parser, the bytecode interpreter, the regular
# expressions, Unicode and number formatting. What it reads and prints
# is the host's business: apps/js/js.c, a console with a few functions
# of ThreadOS's own.
QJS_SRC  := quickjs dtoa libregexp libunicode cutils
QJS_OBJS := $(patsubst %,$(BUILD)/libc64/qjs-%.o,$(QJS_SRC))
QJS_HDRS := $(wildcard third_party/quickjs/*.h)
# -Os: a third smaller, and the initrd is memory (see CXX64_FLAGS)
QJS_CFLAGS := $(LIBC64_CFLAGS) -Os -D_GNU_SOURCE -fwrapv \
              -DCONFIG_VERSION=\"$(shell cat third_party/quickjs/VERSION)\" \
              -Wno-sign-compare -Wno-unused-parameter

$(BUILD)/libc64/qjs-%.o: third_party/quickjs/%.c $(QJS_HDRS) $(LIBC64) \
                         | $(BUILD)/libc64
	$(CC) $(QJS_CFLAGS) -c $< -o $@

# The prelude (console.log, timers - apps/js/prelude.js) is built into
# the program as one C string
$(BUILD)/libc64/prelude.h: apps/js/prelude.js | $(BUILD)/libc64
	python3 -c 'import sys, json; open(sys.argv[2], "w").write( \
	    "static const char prelude_js[] = " + \
	    json.dumps(open(sys.argv[1]).read()) + ";\n")' $< $@

$(BUILD)/user64/js.elf: apps/js/js.c $(BUILD)/libc64/prelude.h $(QJS_OBJS) \
                        $(LIBC64) libc/libc64.ld | $(BUILD)/user64
	$(CC) $(QJS_CFLAGS) -Wall -Ithird_party/quickjs -I$(BUILD)/libc64 \
	    -c $< -o $(BUILD)/libc64/js.o
	$(call LIBC64_LINK,$(BUILD)/libc64/js.o $(QJS_OBJS)) -o $@

USER64_ELFS += $(BUILD)/user64/libctest.elf $(BUILD)/user64/js.elf \
               $(BUILD)/user64/update.elf

# --- C++, and litehtml ------------------------------------------------
#
# C++ programs use the host compiler's own libstdc++: its headers in
# front of musl's (they #include_next the C ones), and its static
# library for the few pieces that are not templates - the red-black
# tree behind std::map, the list node, RTTI for dynamic_cast. They are
# built without exceptions; libc/cxx/cxxrt.cpp is the runtime's last
# pieces (operator new, std::string, the __throw_* family ending the
# program) and libc/cxx/compat.h the glibc names the headers expect.
CXX64_INCS := $(shell echo | $(CXX) -xc++ -E -v - 2>&1 | sed -n 's/^ \(.*c++.*\)$$/-isystem \1/p')
LIBSTDCXX  := $(shell $(CXX) -print-file-name=libstdc++.a)
# -Os and a section per function: whatever is in the initrd is memory
# the machine never gets back, and the swap test runs in 16 MiB
CXX64_FLAGS := -m64 -std=gnu++17 -Os -ffunction-sections -fdata-sections \
               -mno-red-zone -fno-stack-protector \
               -fno-pic -fno-asynchronous-unwind-tables -fno-exceptions \
               -fno-threadsafe-statics -nostdinc -nostdinc++ $(CXX64_INCS) \
               -isystem $(MUSL_ROOT)/include \
               -isystem $(shell $(CC) -print-file-name=include) \
               -include libc/cxx/compat.h -DTHREADOS_LIBC

$(BUILD)/libc64/cxxrt.o: libc/cxx/cxxrt.cpp libc/cxx/compat.h $(LIBC64) \
                         | $(BUILD)/libc64
	$(CXX) $(CXX64_FLAGS) -Wall -c $< -o $@

# litehtml (third_party/litehtml, upstream unchanged) lays out HTML and
# CSS; gumbo, inside it, parses the HTML. Three of its files are built
# from a patched copy - apps/browser/patches says what and why.
LH_DIR     := third_party/litehtml
LH_PATCHED := encodings url document
LH_SRC     := $(filter-out $(LH_PATCHED),$(basename $(notdir $(wildcard $(LH_DIR)/src/*.cpp))))
GUMBO_SRC  := $(basename $(notdir $(wildcard $(LH_DIR)/src/gumbo/src/*.c)))
LH_OBJS    := $(patsubst %,$(BUILD)/litehtml/lh-%.o,$(LH_SRC) $(LH_PATCHED)) \
              $(patsubst %,$(BUILD)/litehtml/gumbo-%.o,$(GUMBO_SRC))
LH_HDRS    := $(wildcard $(LH_DIR)/include/*.h $(LH_DIR)/include/litehtml/*.h \
                         $(LH_DIR)/src/*.h)
LH_INC     := -I$(LH_DIR)/include -I$(LH_DIR)/include/litehtml \
              -I$(LH_DIR)/src -I$(LH_DIR)/src/gumbo/src

$(BUILD)/litehtml:
	mkdir -p $(BUILD)/litehtml

$(BUILD)/litehtml/%.cpp: $(LH_DIR)/src/%.cpp apps/browser/patches/%.patch Makefile \
                         | $(BUILD)/litehtml
	@set -e; \
	src="$@.src"; patchfile="$@.patch"; output="$@.tmp"; \
	trap 'rm -f "$$src" "$$patchfile" "$$output"' 0; \
	sed 's/\r$$//' "$<" > "$$src"; \
	sed 's/\r$$//' "apps/browser/patches/$*.patch" > "$$patchfile"; \
	patch -s -o "$$output" "$$src" "$$patchfile"; \
	mv "$$output" "$@"

# (static patterns: a plain pattern rule would lose to the one below,
# whose source exists without being made, and build the unpatched file)
$(patsubst %,$(BUILD)/litehtml/lh-%.o,$(LH_PATCHED)): \
$(BUILD)/litehtml/lh-%.o: $(BUILD)/litehtml/%.cpp $(LH_HDRS) $(LIBC64) \
                          | $(BUILD)/litehtml
	$(CXX) $(CXX64_FLAGS) -w $(LH_INC) -c $< -o $@

$(BUILD)/litehtml/lh-%.o: $(LH_DIR)/src/%.cpp $(LH_HDRS) $(LIBC64) \
                          | $(BUILD)/litehtml
	$(CXX) $(CXX64_FLAGS) -w $(LH_INC) -c $< -o $@

$(BUILD)/litehtml/gumbo-%.o: $(LH_DIR)/src/gumbo/src/%.c $(LIBC64) \
                             | $(BUILD)/litehtml
	$(CC) $(LIBC64_CFLAGS) -w -c $< -o $@

# The C++ link: musl's start files, the program, libstdc++ for what
# the headers do not carry, and the C library after it
CXX64_LINK = $(call LIBC64_LINK,$(1) $(BUILD)/libc64/cxxrt.o \
                                $(LIBSTDCXX)) -Wl,--gc-sections

# layouttest lays pages out with a font of known sizes and prints every
# box; scripts/tests/t_browser.py holds what it prints on ThreadOS
# against the same program built for this machine. It is not in the
# initrd - the test fetches it onto the disk, where a program can be
# run from too, which keeps the initrd (and the memory every boot
# spends on it) the size it was.
$(BUILD)/user64/layouttest.elf: apps/browser/layouttest.cpp $(LH_OBJS) \
                                $(BUILD)/libc64/cxxrt.o libc/libc64.ld \
                                | $(BUILD)/user64
	$(CXX) $(CXX64_FLAGS) -Wall $(LH_INC) -c $< -o $(BUILD)/litehtml/layouttest.o
	$(call CXX64_LINK,$(BUILD)/litehtml/layouttest.o $(LH_OBJS)) -o $@

# web: the browser - litehtml's layout drawn into a window
# The browser's JavaScript is QuickJS, the same objects as js's
$(BUILD)/litehtml/webjs.o: apps/browser/webjs.cpp apps/browser/webjs.h \
                           $(QJS_HDRS) $(LH_HDRS) $(LIBC64) | $(BUILD)/litehtml
	$(CXX) $(CXX64_FLAGS) -Wall $(LH_INC) -Ithird_party/quickjs -c $< -o $@

$(BUILD)/user64/web.elf: apps/browser/web.cpp apps/browser/webjs.h $(LH_OBJS) \
                         $(BUILD)/litehtml/webjs.o $(QJS_OBJS) \
                         $(BUILD)/libc64/cxxrt.o libc/libc64.ld \
                         user/udraw.h user64/usys64.h user64/compat64.h \
                         | $(BUILD)/user64
	$(CXX) $(CXX64_FLAGS) -Wall $(LH_INC) -Iuser -Iuser64 -c $< \
	    -o $(BUILD)/litehtml/web.o
	$(call CXX64_LINK,$(BUILD)/litehtml/web.o $(BUILD)/litehtml/webjs.o \
	    $(LH_OBJS) $(QJS_OBJS)) -o $@

USER64_ELFS += $(BUILD)/user64/web.elf

# ... and the same source and library built for this machine
LH_HOST_OBJS := $(patsubst $(BUILD)/litehtml/%.o,$(BUILD)/litehtml/host/%.o,$(LH_OBJS))

$(BUILD)/litehtml/host:
	mkdir -p $(BUILD)/litehtml/host

$(patsubst %,$(BUILD)/litehtml/host/lh-%.o,$(LH_PATCHED)): \
$(BUILD)/litehtml/host/lh-%.o: $(BUILD)/litehtml/%.cpp $(LH_HDRS) \
                               | $(BUILD)/litehtml/host
	$(CXX) -std=gnu++17 -O2 -w $(LH_INC) -c $< -o $@

$(BUILD)/litehtml/host/lh-%.o: $(LH_DIR)/src/%.cpp $(LH_HDRS) \
                               | $(BUILD)/litehtml/host
	$(CXX) -std=gnu++17 -O2 -w $(LH_INC) -c $< -o $@

$(BUILD)/litehtml/host/gumbo-%.o: $(LH_DIR)/src/gumbo/src/%.c \
                                  | $(BUILD)/litehtml/host
	$(CC) -O2 -w -c $< -o $@

$(BUILD)/litehtml/host/layouttest: apps/browser/layouttest.cpp $(LH_HOST_OBJS)
	$(CXX) -std=gnu++17 -O2 -Wall $(LH_INC) $< $(LH_HOST_OBJS) -o $@

browser: $(BUILD)/user64/layouttest.elf $(BUILD)/litehtml/host/layouttest \
         $(BUILD)/user64/web.elf

# VERSION orders signed updates monotonically. VERSION_LABEL is the
# SemVer-style version shown to people; releases stay in the 0.x
# pre-release series until ThreadOS is ready for daily use.
VERSION ?= 0
VERSION_LABEL ?= 0.1.0-dev
VERSION_FILE := $(BUILD)/version.txt
VERSION_CODE_FILE := $(BUILD)/version-code.txt
$(shell mkdir -p $(BUILD) && { [ "$$(cat $(VERSION_FILE) 2>/dev/null)" = "$(VERSION_LABEL)" ] || echo "$(VERSION_LABEL)" > $(VERSION_FILE); } && { [ "$$(cat $(VERSION_CODE_FILE) 2>/dev/null)" = "$(VERSION)" ] || echo "$(VERSION)" > $(VERSION_CODE_FILE); })

$(INITRD64): $(USER64_ELFS) $(LIBTHREAD64) scripts/mkinitrd.py $(FONTS) \
             $(VERSION_FILE) $(VERSION_CODE_FILE)
	python3 scripts/mkinitrd.py $@ \
	    libthread.so=$(LIBTHREAD64) \
	    sans.ttf=fonts/sans.ttf bold.ttf=fonts/bold.ttf mono.ttf=fonts/mono.ttf \
	    hello64=$(BUILD)/user64/hello64.elf \
	    nx64=$(BUILD)/user64/nx64.elf \
	    wild64=$(BUILD)/user64/wild64.elf \
	    sep64=$(BUILD)/user64/sep64.elf \
	    fork64=$(BUILD)/user64/fork64.elf \
	    spawn64=$(BUILD)/user64/spawn64.elf \
	    sigtest64=$(BUILD)/user64/sigtest64.elf \
	    spin64=$(BUILD)/user64/spin64.elf \
	    ush64=$(BUILD)/user64/ush64.elf \
	    fileio64=$(BUILD)/user64/fileio64.elf \
	    uecho64=$(BUILD)/user64/uecho64.elf \
	    ucat64=$(BUILD)/user64/ucat64.elf \
	    heap64=$(BUILD)/user64/heap64.elf \
	    fpu64test=$(BUILD)/user64/fpu64test.elf \
	    uls64=$(BUILD)/user64/uls64.elf \
	    lspci=$(BUILD)/user64/lspci.elf hardware=$(BUILD)/user64/lspci.elf \
	    ifconfig=$(BUILD)/user64/ifconfig.elf \
	    uupper64=$(BUILD)/user64/uupper64.elf \
	    unet64=$(BUILD)/user64/unet64.elf \
	    guiapp=$(BUILD)/user64/guiapp64.elf \
	    swaptest64=$(BUILD)/user64/swaptest64.elf \
	    shmwrite64=$(BUILD)/user64/shmwrite64.elf \
	    shmread64=$(BUILD)/user64/shmread64.elf \
	    gclock=$(BUILD)/user64/gclock.elf \
	    gfiles=$(BUILD)/user64/gfiles.elf \
	    gmon=$(BUILD)/user64/gmon.elf \
	    gterm=$(BUILD)/user64/gterm.elf core=$(BUILD)/user64/core.elf \
	    gedit=$(BUILD)/user64/gedit.elf \
	    gcode=$(BUILD)/user64/gcode.elf \
	    gtasks=$(BUILD)/user64/gtasks.elf \
	    udtest=$(BUILD)/user64/udtest.elf \
	    gcalc=$(BUILD)/user64/gcalc.elf gcal=$(BUILD)/user64/gcal.elf \
	    gpaint=$(BUILD)/user64/gpaint.elf \
	    gusers=$(BUILD)/user64/gusers.elf \
	    gsettings=$(BUILD)/user64/gsettings.elf \
	    gappear=$(BUILD)/user64/gappear.elf \
	    ginstal=$(BUILD)/user64/ginstal.elf \
	    gmikser=$(BUILD)/user64/gmikser.elf \
	    gimg=$(BUILD)/user64/gimg.elf \
	    gmedia=$(BUILD)/user64/gmedia.elf \
	    cryptotest=$(BUILD)/user64/cryptotest.elf \
	    certtest=$(BUILD)/user64/certtest.elf \
	    aestest=$(BUILD)/user64/aestest.elf \
	    wpatest=$(BUILD)/user64/wpatest.elf \
	    nslookup=$(BUILD)/user64/nslookup.elf \
	    httpget=$(BUILD)/user64/httpget.elf \
	    gweb=$(BUILD)/user64/gweb.elf \
	    web=$(BUILD)/user64/web.elf \
	    mkmap64=$(BUILD)/user64/mkmap64.elf \
	    tlbtest64=$(BUILD)/user64/tlbtest64.elf \
	    mmapfile64=$(BUILD)/user64/mmapfile64.elf \
	    mmapzap64=$(BUILD)/user64/mmapzap64.elf \
	    dynlib=$(BUILD)/user64/dynlib.elf \
	    cores=$(BUILD)/user64/cores.elf \
	    mutex=$(BUILD)/user64/mutex.elf prodcons=$(BUILD)/user64/prodcons.elf \
	    syscalls=$(BUILD)/user64/syscalls.elf \
	    libctest=$(BUILD)/user64/libctest.elf \
	    js=$(BUILD)/user64/js.elf \
	    update=$(BUILD)/user64/update.elf \
	    version=$(VERSION_FILE) version-code=$(VERSION_CODE_FILE)
	@echo "Done: $@ ($$(stat -c%s $@) B)"

initrd64: $(INITRD64)

KERNEL64_HEADERS := $(wildcard kernel64/*.h)

$(BUILD)/64/%.o: kernel64/%.c $(KERNEL_HEADERS) $(KERNEL64_HEADERS) $(USERCTL_HEADER) | $(BUILD)/64
	$(CC) $(CFLAGS64) -c $< -o $@

$(BUILD)/64/%.o: kernel/%.c $(KERNEL_HEADERS) $(USERCTL_HEADER) | $(BUILD)/64
	$(CC) $(CFLAGS64) -c $< -o $@

$(KERNEL64): $(OBJS64) kernel64/linker64.ld
	$(CC) $(LDFLAGS64) $(OBJS64) -o $@

kernel64: $(KERNEL64) $(INITRD64)
	@echo "Done: $(KERNEL64) (long mode, loaded by GRUB)"

# An ISO with the 64-bit kernel that boots under an old BIOS and under
# UEFI alike. One disc serves both roads: grub-mkrescue puts two sets
# of GRUB files and an EFI partition on it, and the firmware picks the
# one it understands. That is what was missing for booting on newer
# hardware - the old image knew only the BIOS road.
ISO64 := $(BUILD)/threados64.iso

# The disc carries threadboot too (when clang can build it): not to
# boot from - GRUB does that - but for the installer to write onto a
# disk, which then boots without GRUB (see boot/grub64.cfg)
ISO64_LOADER := $(if $(shell command -v clang 2>/dev/null),$(BUILD)/BOOTX64.EFI)

$(ISO64): $(KERNEL64) $(INITRD64) boot/grub64.cfg $(ISO64_LOADER)
	@command -v grub-mkrescue >/dev/null || { \
	    echo "ERROR: no grub-mkrescue."; \
	    echo "  Debian/Ubuntu: apt install grub-pc-bin grub-efi-amd64-bin \
grub-common xorriso mtools"; \
	    exit 1; }
	mkdir -p $(BUILD)/isodir64/boot/grub
	cp $(KERNEL64) $(BUILD)/isodir64/boot/threados64.bin
	cp $(INITRD64) $(BUILD)/isodir64/boot/initrd64.img
	cp boot/grub64.cfg $(BUILD)/isodir64/boot/grub/grub.cfg
	$(if $(ISO64_LOADER),cp $(ISO64_LOADER) $(BUILD)/isodir64/boot/threadboot.efi)
	grub-mkrescue -o $@ $(BUILD)/isodir64
	@echo "Done: $@ (BIOS + UEFI)"

iso64: $(ISO64)

# Booting through UEFI needs firmware - OVMF plays that part in QEMU
# just as a real motherboard does
run-iso64-uefi: $(ISO64)
	cp /usr/share/OVMF/OVMF_VARS_4M.fd $(BUILD)/ovmf_vars.fd
	qemu-system-x86_64 -cdrom $(ISO64) -boot d \
	    -drive if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
	    -drive if=pflash,format=raw,unit=1,file=$(BUILD)/ovmf_vars.fd

# An empty 16 MiB disk image for the TFS filesystem (the kernel will
# format it). TFS3 hands out blocks from a bitmap, so the room goes
# where it is needed.
$(DISK): | $(BUILD)
	dd if=/dev/zero of=$@ bs=1M count=16 status=none

# An ISO to burn or to start with 'qemu -cdrom'. GRUB loads the kernel
# through Multiboot and hands it the initrd as a module; the menu lets
# you pick the desktop or the text console (boot/grub.cfg).
iso: $(KERNEL) $(INITRD) boot/grub.cfg
	@command -v grub-mkrescue >/dev/null || { \
	    echo "ERROR: no grub-mkrescue."; \
	    echo "  Debian/Ubuntu: apt install grub-pc-bin grub-common xorriso"; \
	    echo "  Fedora:        dnf install grub2-tools xorriso"; \
	    exit 1; }
	mkdir -p $(BUILD)/isodir/boot/grub
	cp $(KERNEL) $(BUILD)/isodir/boot/threados.bin
	cp $(INITRD) $(BUILD)/isodir/boot/initrd.img
	cp boot/grub.cfg $(BUILD)/isodir/boot/grub/grub.cfg
	grub-mkrescue -o $(ISO) $(BUILD)/isodir
	@echo "Done: $(ISO) - 'make run-iso', or write it to a stick"

# QEMU can load a Multiboot kernel directly - with no ISO and no GRUB.
# The disk (-hda) lands on the first ATA bus as the master.
# Network: an RTL8139 card in QEMU's user mode (the address comes from
# DHCP: 10.0.2.15, gateway 10.0.2.2). hostfwd puts the TCP server from
# ThreadOS in front of the host: after 'run tcpserv' one can type 'nc
# localhost 5555' in a terminal.
NETOPTS := -netdev user,id=n0,hostfwd=tcp::5555-:5555 \
           -device rtl8139,netdev=n0

# 'make run' is the 64-bit system - the one everything new goes into -
# started off the disc the way a real machine starts it: GRUB, the
# desktop straight away (the first menu entry; the second stays at the
# shell). The machine has what the system has drivers for: a SATA disk
# behind AHCI, an e1000 network card, HD Audio and two cores. The disk
# image is the system's own and survives between runs.
#
#   make run              the 64-bit system
#   make run AUDIO=pa     with sound (pa, pipewire, alsa - whatever the
#                         host has; 'none' by default, because a machine
#                         without a sound server would refuse to start)
#   make run KVM=1        with hardware virtualisation, if /dev/kvm is
#                         there - it starts in a second instead of ten
#   make run QEMU_EXTRA="..."  anything else QEMU should be told
#   make run32            the 32-bit kernel, loaded by QEMU directly
AUDIO ?= none
DISK64 := $(BUILD)/disk64.img
QEMU64_ACCEL := $(if $(KVM),-enable-kvm -cpu host,)

$(DISK64): | $(BUILD)
	dd if=/dev/zero of=$@ bs=1M count=64 status=none

run: $(ISO64) $(DISK64)
	qemu-system-x86_64 -m 512 -smp 2 $(QEMU64_ACCEL) \
	    -cdrom $(ISO64) -boot d \
	    -device ich9-ahci,id=ahci \
	    -drive id=d0,file=$(DISK64),if=none,format=raw \
	    -device ide-hd,drive=d0,bus=ahci.0 \
	    -netdev user,id=n0,hostfwd=tcp::5555-:5555 -device e1000,netdev=n0 \
	    -audiodev $(AUDIO),id=snd0 -device intel-hda \
	    -device hda-output,audiodev=snd0 $(QEMU_EXTRA)

run32: $(KERNEL) $(INITRD) $(DISK)
	qemu-system-i386 -kernel $(KERNEL) -initrd $(INITRD) -hda $(DISK) $(NETOPTS)

# Starting off the ISO the way a real machine would: a disk (TFS + the
# swap file) and a network card, because without them half the system
# has nothing to do.
run-iso: iso $(DISK)
	qemu-system-i386 -cdrom $(ISO) -boot d -hda $(DISK) $(NETOPTS)

# The automatic test: a boot in QEMU with no window, and the VGA screen
# read back. The test makes a fresh disk image itself and checks that
# what was written survives between boots. The groups run in parallel,
# because each has a QEMU machine of its own and spends most of its
# time waiting anyway: for the boot, for an answer, for the screen to
# be redrawn.
#
#   make test            every group
#   make test T=k64      the 64-bit kernel only (or k64,uefi)
#   make test J=1        one after another, for debugging
# The disc too, where GRUB can make it: t_isoinstall installs from the
# one 'make iso64' builds, not from a copy of its own
test: $(KERNEL) $(INITRD) $(KERNEL64) $(INITRD64) browser \
      $(if $(shell command -v grub-mkrescue 2>/dev/null),$(ISO64))
	python3 -u scripts/run_tests.py $(if $(T),-t$(T)) $(if $(J),-j$(J)) \
	    $(KERNEL) $(INITRD) $(KERNEL64) $(INITRD64)

# The parsers get their data straight off the network - this target
# runs them under sanitizers on the host, feeding them corrupted input
# (certificates, HTML, HTTP and the frames that reach net.c and tcp.c
# in the kernel).
fuzz:
	python3 scripts/fuzz_parsers.py 500

clean:
	rm -rf $(BUILD)

# --- Our own UEFI boot loader ----------------------------------------
#
# An EFI application is a PE32+ file with Microsoft's calling
# convention - two things GCC on Linux does not do. clang with lld-link
# does them, and that is the only borrowed thing here: the file format.
# The code is all ours, with no gnu-efi and no EDK2.
EFI_OUT := $(BUILD)/BOOTX64.EFI
EFI_CC  := clang -target x86_64-unknown-windows -ffreestanding \
           -fshort-wchar -mno-red-zone -Wall -Wextra -O2

$(EFI_OUT): boot/efi/efiboot.c boot/efi/efi.h | $(BUILD)
	@command -v clang >/dev/null && command -v lld-link >/dev/null || { \
	    echo "ERROR: no clang or no lld-link (apt install clang lld)"; \
	    exit 1; }
	$(EFI_CC) -c boot/efi/efiboot.c -o $(BUILD)/efiboot.o
	lld-link -subsystem:efi_application -entry:efi_main -nodefaultlib \
	    -out:$@ $(BUILD)/efiboot.o
	@echo "Done: $@"

efi: $(EFI_OUT)

# UEFI firmware for QEMU. The same files run-iso64-uefi uses.
OVMF_CODE := /usr/share/OVMF/OVMF_CODE_4M.fd
OVMF_VARS := /usr/share/OVMF/OVMF_VARS_4M.fd

# --- A stick this machine boots from ---------------------------------
#
# What a UEFI machine boots from is neither a disc nor a kernel: it is
# a disk with a GPT, an EFI partition inside it, a FAT in that and a
# loader under a name the firmware knows. This target makes exactly
# that image - raw, to be written to a stick with dd. No GRUB and no El
# Torito.
STICK := $(BUILD)/threados-stick.img

$(STICK): $(EFI_OUT) $(KERNEL64) $(INITRD64) scripts/mkstick.py
	@command -v mformat >/dev/null || { \
	    echo "ERROR: no mtools (apt install mtools)"; exit 1; }
	python3 scripts/mkstick.py $@ $(EFI_OUT) $(KERNEL64) $(INITRD64)

stick: $(STICK)

# The same image started under OVMF - before it goes onto a stick, it
# is worth seeing it boot.
run-stick: $(STICK)
	@test -f $(OVMF_CODE) || { echo "ERROR: no OVMF"; exit 1; }
	cp $(OVMF_VARS) $(BUILD)/ovmf_vars.fd
	qemu-system-x86_64 -m 512 \
	    -drive if=pflash,format=raw,unit=0,readonly=on,file=$(OVMF_CODE) \
	    -drive if=pflash,format=raw,unit=1,file=$(BUILD)/ovmf_vars.fd \
	    -drive file=$(STICK),format=raw,media=disk
