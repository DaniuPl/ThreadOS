# ThreadOS - the build
#
# Targets (BUILD=<dir> puts everything somewhere other than build/):
#   make            - the kernel and its initrd (make kernel64 is the same)
#   make efi        - threadboot, the UEFI loader (needs clang + lld)
#   make iso64      - the hybrid BIOS + UEFI disc, the installer included
#   make stick      - a bootable USB image (GPT + an EFI partition)
#   make run        - the system in QEMU, from its disc, to the desktop
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

BUILD   := build
# The fonts in the initrd - cut out of DejaVu by scripts/mkfonts.py and
# committed, so building needs neither fonttools nor the DejaVu package
FONTS   := fonts/sans.ttf fonts/bold.ttf fonts/mono.ttf

.PHONY: all iso64 initrd64 libc64 libs64 browser run run-iso64-uefi test fuzz clean \
        kernel64 efi stick run-stick kernelarm64 run-arm64 run-phone webkit \
        libcarm64 webkit-libs arm64-libs weft-arm64

all: kernel64

$(BUILD):
	mkdir -p $(BUILD)

# The dependency on the headers is spelled out here: without it a
# change in kernel/include/ rebuilt nothing and make ended green, while
# the objects still remembered the old prototype.
KERNEL_HEADERS := $(wildcard kernel/include/*.h)
USERCTL_HEADER := user/userctl.h

# The userspace headers are pulled in by many programs (the syscall
# library, the network, the cryptography), so a change to any of them
# has to rebuild everything - hence a wildcard instead of a hand-kept
# list.
USER_HEADERS := $(wildcard user/*.h)

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

$(BUILD)/libsrc $(BUILD)/decl $(BUILD)/dyn:
	mkdir -p $@

$(BUILD)/libsrc/%.h: user/%.h scripts/split_headers.py | $(BUILD)/libsrc
	python3 scripts/split_headers.py --lib $< > $@

$(BUILD)/decl/%.h: user/%.h scripts/split_headers.py | $(BUILD)/decl
	python3 scripts/split_headers.py --decl $< > $@

$(BUILD)/libsrc/libthread.c: user/libthread.c | $(BUILD)/libsrc
	cp $< $@

$(BUILD)/dyn/%.c: user/%.c | $(BUILD)/dyn
	cp $< $@

# The programs that take the cryptography from the library instead of
# carrying it themselves (libthread64.so, below)
USER_DYN := cryptotest certtest httpget gweb nslookup dynlib

# --- The kernel (long mode) -------------------------------------------
#
# kernel/ holds what does not depend on the processor mode (drivers,
# the filesystem, the network, the desktop); kernel64/ and boot64/ hold
# what does: paging, interrupts, the context switch, system calls and
# the entry into long mode. There used to be a 32-bit kernel built from
# the same kernel/ files; it was removed, and kernel/ still carries the
# names that came with it (task.h, for one), which kernel64/compat64.c
# answers.
#
# The processor everything is built for: the first x86-64 one, by name.
# Leaving it to the compiler is not the same thing - distributions build
# GCC to aim at newer processors (Ubuntu's GCC 15 defaults to
# x86-64-v3, Haswell's), and then the kernel is full of BMI2 and MOVBE
# and every program of AVX. The kernel never enables AVX's register
# state, so on real hardware the first such instruction in a program is
# an Invalid Opcode, and on a processor without BMI2 the kernel itself
# does not start. scripts/checkisa.py holds a build to this.
ARCH64 := -march=x86-64 -mtune=generic

# The machine the C library, its libraries and the programs on them are
# built for: x86_64, the PC (the default), or aarch64 - the ARM64
# kernel, which takes the PC's system calls (kernelarm64/proc.c), so
# that libc/overlay is one layer for both. Weft for the phone is
#   make TARGET=aarch64 BUILD=<its own directory> webkit
# Only that side follows TARGET: the kernels are built as they always
# are, and one build directory holds one machine's libraries.
TARGET ?= x86_64
ifeq ($(TARGET),aarch64)
CC      := aarch64-linux-gnu-gcc
CXX     := aarch64-linux-gnu-g++
AR      := aarch64-linux-gnu-ar
RANLIB  := aarch64-linux-gnu-ranlib
STRIP   := aarch64-linux-gnu-strip
# no fused multiply-add: what the C library computes is what it does
# on the PC, where the tests compare it with glibc
TFLAGS  := -march=armv8-a -mno-outline-atomics -ffp-contract=off
THOST   := aarch64-pc-linux-musl
TLD     := libc/libcarm64.ld
TCPU    := aarch64
OPENSSL_TARGET := linux-aarch64
else ifeq ($(TARGET),x86_64)
AR      := ar
RANLIB  := ranlib
STRIP   := strip
TFLAGS  := -m64 $(ARCH64) -mno-red-zone
THOST   := x86_64-pc-linux-musl
TLD     := libc/libc64.ld
TCPU    := x86_64
OPENSSL_TARGET := linux-x86_64
else
$(error TARGET is x86_64 or aarch64)
endif
# Not handed down to the libraries' own makefiles, in which TARGET means
# something else (ICU's is the library it builds) - neither in the
# flags a sub-make reads nor in its environment
MAKEOVERRIDES := $(filter-out TARGET=%,$(MAKEOVERRIDES))
unexport TARGET

KERNEL64 := $(BUILD)/threados64.bin
CFLAGS64 := -m64 $(ARCH64) -std=gnu11 -ffreestanding -fno-stack-protector -fno-pic \
            -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -Wall -Wextra -Wshadow -O2 -g
LDFLAGS64 := -m64 -ffreestanding -nostdlib -static -no-pie -T kernel64/linker64.ld \
             -Wl,--build-id=none -Wl,--no-warn-rwx-segments

SHARED64 := string serial printk locale bootsplash mark mb2 fbcon vga pic pit pmm heap initrd \
            kbdmap pci ata ahci nvme blk fs tfs3 tfs4 rtl8139 net dhcp tcp video \
            mouse desktop acpi uhci ehci xhci usbhc ttf fonts uitext gfx aurora usbkbd usbmouse usbmass bt usbnet part fat gpt fatcmd install hwinfo hda swap prog shm sem rtc \
            dynlink smpmem smpthreads biglock tlb config user clip \
            e1000 rtl8168 virtio_net netdev smp random hid iwlwifi acpios power \
            devices uvc ipc wpa wlan
OBJS64 := $(BUILD)/64/boot64.o $(BUILD)/64/isr64_stubs.o \
          $(BUILD)/64/switch64.o \
          $(BUILD)/64/kmain64.o $(BUILD)/64/idt64.o $(BUILD)/64/isr64.o \
          $(BUILD)/64/paging64.o $(BUILD)/64/task64.o \
          $(BUILD)/64/gdt64.o $(BUILD)/64/syscall64.o \
          $(BUILD)/64/elf64.o $(BUILD)/64/proc64.o \
          $(BUILD)/64/keyboard64.o $(BUILD)/64/shell64.o \
          $(BUILD)/64/signal64.o $(BUILD)/64/fpu64.o $(BUILD)/64/futex64.o \
          $(BUILD)/64/pipe64.o $(BUILD)/64/compat64.o \
          $(BUILD)/64/initrd64.o \
          $(BUILD)/64/efirt.o \
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

# The long-mode user programs: their own linker script and their own
# syscall library - the kernel is entered with the syscall instruction,
# not through an interrupt gate.
INITRD64 := $(BUILD)/initrd64.img
# The user programs are built with SSE - on x86-64 it is part of the
# calling convention, and the kernel can already save and restore those
# registers when it switches tasks. The kernel itself stays without
# SSE: there is nothing to compute in an interrupt handler, and every
# such instruction would mean saving the interrupted program's state.
USER64_CFLAGS := -m64 $(ARCH64) -std=gnu11 -ffreestanding -fno-stack-protector \
                 -fno-pic -no-pie -static -nostdlib -mno-red-zone \
                 -O2 -Wall -Wextra \
                 -Wl,--build-id=none -Wl,-z,max-page-size=0x1000
USER64_ELFS := $(BUILD)/user64/hello64.elf $(BUILD)/user64/nx64.elf \
               $(BUILD)/user64/wild64.elf $(BUILD)/user64/sep64.elf \
               $(BUILD)/user64/hostile64.elf \
               $(BUILD)/user64/fork64.elf $(BUILD)/user64/spawn64.elf \
               $(BUILD)/user64/sigtest64.elf $(BUILD)/user64/spin64.elf \
               $(BUILD)/user64/ush64.elf $(BUILD)/user64/fileio64.elf \
               $(BUILD)/user64/uecho64.elf $(BUILD)/user64/ucat64.elf \
               $(BUILD)/user64/heap64.elf $(BUILD)/user64/fpu64test.elf \
               $(BUILD)/user64/uls64.elf $(BUILD)/user64/uupper64.elf \
               $(BUILD)/user64/unet64.elf \
               $(BUILD)/user64/guiapp64.elf $(BUILD)/user64/gdeaf.elf \
               $(BUILD)/user64/seq.elf $(BUILD)/user64/spin.elf \
               $(BUILD)/user64/mkwall.elf \
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
               $(BUILD)/user64/ggallery.elf \
               $(BUILD)/user64/gcamera.elf \
               $(BUILD)/user64/grecord.elf \
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
               $(BUILD)/user64/longpath64.elf \
               $(BUILD)/user64/deep64.elf \
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

# The programs in user/: the cryptography, TLS, HTTP, DNS and most of
# the desktop's programs know nothing about the processor. They speak
# to the kernel through user/uapi.h - hence a rule of their own next to
# the one for user64/.
$(BUILD)/user64/%.elf: user/%.c user/uapi.h user64/usys64.h \
                       user64/compat64.h user64/user64.ld $(USER_HEADERS) \
                       Makefile | $(BUILD)/user64
	$(CC) $(USER64_CFLAGS) -T user64/user64.ld $< -o $@


# --- The shared library, built (see "The shared library" above) -----
LIBTHREAD64 := $(BUILD)/libthread64.so
LIB_DECL64  := $(patsubst %,$(BUILD)/decl/%,$(LIB_HEADERS))

$(LIBTHREAD64): $(BUILD)/libsrc/libthread.c $(LIB_SRC) user64/libthread64.ld \
                $(USER_HEADERS) Makefile
	$(CC) -m64 $(ARCH64) -std=gnu11 -ffreestanding -fno-stack-protector -fPIC \
	    -mno-red-zone -O2 -Wall -Wextra -I$(BUILD)/libsrc -Iuser \
	    -c $< -o $(BUILD)/libthread64.o
	$(CC) -m64 -shared -nostdlib -Wl,--build-id=none -Wl,-Bsymbolic \
	    -Wl,-soname,libthread.so -Wl,--no-warn-rwx-segments \
	    -Wl,-z,max-page-size=0x1000 \
	    -T user64/libthread64.ld -o $@ $(BUILD)/libthread64.o

USER64_DYN_CFLAGS := -m64 $(ARCH64) -std=gnu11 -ffreestanding -fno-stack-protector \
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
# (third_party/musl, unchanged upstream 1.2.5), built for TARGET with a
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
MUSL_CFLAGS  := -O2 $(TFLAGS) -fno-stack-protector -fno-pic \
                -fno-asynchronous-unwind-tables

# A header of the overlay is included all over musl, and musl's
# Makefile does not track headers - so a changed one starts the copy
# afresh.
$(MUSL_TREE)/config.mak: Makefile third_party/musl/VERSION \
                         third_party/musl/tools/mkalltypes.sed \
                         $(wildcard third_party/musl/tools/*.sh) \
                         third_party/musl/arch/$(TARGET)/bits/alltypes.h.in \
                         third_party/musl/include/alltypes.h.in \
                         $(filter %.h,$(MUSL_OVERLAY))
	rm -rf $(MUSL_TREE) && mkdir -p $(MUSL_TREE)
	cp -r third_party/musl/. $(MUSL_TREE)/
	sed -i 's/\r$$//' $(MUSL_TREE)/configure \
	    $(MUSL_TREE)/VERSION \
	    $(MUSL_TREE)/tools/mkalltypes.sed \
	    $(MUSL_TREE)/tools/*.sh \
	    $(MUSL_TREE)/arch/$(TARGET)/bits/alltypes.h.in \
	    $(MUSL_TREE)/include/alltypes.h.in
	cd $(MUSL_TREE) && CC="$(CC)" AR=$(AR) RANLIB=$(RANLIB) CFLAGS="$(MUSL_CFLAGS)" ./configure \
	    --target=$(TARGET) --prefix=/ --disable-shared >/dev/null

# The library and its headers are installed only where their contents
# changed (install -C leaves an identical file, and its time, alone),
# and make looks at the time again after a recipe: a copy of musl
# rebuilt the same - after any edit to this Makefile, say - leaves
# libc.a as old as it was, and what is built on it (the libraries in
# libs64, ICU, WebKit, whose builds take hours) is not built again.
#
# And what is compiled against musl depends on its headers, not on
# libc.a (MUSL_HEADERS: their checksums, written only when one changed):
# a fix in the overlay's C files changes libc.a alone, and costs the
# programs their link, not the libraries under them their build.
MUSL_STAMP   := $(MUSL_TREE)/installed.stamp
MUSL_HEADERS := $(MUSL_ROOT)/headers.sum

$(MUSL_STAMP): $(MUSL_TREE)/config.mak $(MUSL_OVERLAY)
	cp -r libc/overlay/. $(MUSL_TREE)/
	$(MAKE) -s -C $(MUSL_TREE) lib/libc.a lib/crt1.o lib/crti.o lib/crtn.o
	$(MAKE) -s -C $(MUSL_TREE) install-libs install-headers \
	    DESTDIR=$(abspath $(MUSL_ROOT)) INSTALL="install -C"
	touch $@

$(LIBC64): $(MUSL_STAMP)
	@test -f $@

$(MUSL_HEADERS): $(MUSL_STAMP)
	cd $(MUSL_ROOT)/include && find . -type f | LC_ALL=C sort | \
	    xargs sha256sum > $(abspath $@).new
	cmp -s $@.new $@ && rm -f $@.new || mv $@.new $@

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

$(FFMPEG_CONFIG): third_party/ffmpeg-9.0.tar.xz Makefile $(MUSL_HEADERS)
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
	    --extra-cflags="-m64 $(ARCH64) -std=gnu11 -O2 -mno-red-zone -fno-stack-protector -fno-pic -fno-asynchronous-unwind-tables -nostdinc -isystem $(abspath $(MUSL_ROOT)/include) -isystem $(shell $(CC) -print-file-name=include) -D_GNU_SOURCE" \
	    --extra-ldflags="-no-pie" --disable-logging \
	    > configure.log 2>&1 || { cat configure.log; exit 1; }

$(FFMPEG_STAMP): $(FFMPEG_CONFIG)
	$(MAKE) -s -C $(FFMPEG_TREE) $(patsubst $(FFMPEG_TREE)/%,%,$(FFMPEG_LIBS))
	touch $@

$(FFMPEG_LIBS): $(FFMPEG_STAMP)

# The Recorder: memory that grows with the recording
$(BUILD)/user64/grecord.elf: user64/grecord.c user/uapi.h user/udraw.h \
                             user/umic.h user64/usys64.h user64/compat64.h \
                             $(LIBC64) libc/libc64.ld \
                             | $(BUILD)/user64 $(BUILD)/libc64
	$(CC) $(LIBC64_CFLAGS) -Iuser -D_GNU_SOURCE -DTHREADOS_LIBC \
	    -Wall -Wextra -c $< -o $(BUILD)/libc64/grecord.o
	$(call LIBC64_LINK,$(BUILD)/libc64/grecord.o) -o $@

# Code, the development environment: files of any length and a tree of
# any size, so built against musl
$(BUILD)/user64/gcode.elf: user64/gcode.c user/uapi.h user/udraw.h \
                           user/ulocale.h user/uterm.h user64/usys64.h \
                           user64/compat64.h $(LIBC64) libc/libc64.ld \
                           | $(BUILD)/user64 $(BUILD)/libc64
	$(CC) $(LIBC64_CFLAGS) -Iuser -D_GNU_SOURCE -DTHREADOS_LIBC \
	    -Wall -Wextra -c $< -o $(BUILD)/libc64/gcode.o
	$(call LIBC64_LINK,$(BUILD)/libc64/gcode.o) -o $@

# The Gallery: a folder of pictures decoded and shrunk, which needs
# memory given back - so it is built against musl
$(BUILD)/user64/ggallery.elf: user64/ggallery.c user/uapi.h user/udraw.h \
                              user/upng.h user/ujpeg.h user/ugif.h \
                              user/uwebp.h user/usvg.h user/uinflate.h \
                              user64/usys64.h user64/compat64.h \
                              $(LIBC64) libc/libc64.ld \
                              | $(BUILD)/user64 $(BUILD)/libc64
	$(CC) $(LIBC64_CFLAGS) -Iuser -D_GNU_SOURCE -DTHREADOS_LIBC \
	    -Wall -Wextra -c $< -o $(BUILD)/libc64/ggallery.o
	$(call LIBC64_LINK,$(BUILD)/libc64/ggallery.o $(MUSL_ROOT)/lib/libm.a) \
	    -o $@

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
LIBC64_CFLAGS := $(TFLAGS) -std=gnu11 -O2 -fno-stack-protector \
                 -fno-pic -fno-asynchronous-unwind-tables \
                 -nostdinc -isystem $(MUSL_ROOT)/include \
                 -isystem $(shell $(CC) -print-file-name=include)
# The same for builds that change into a tree of their own (configure
# scripts, upstream makefiles), where a relative BUILD would point
# nowhere
LIBC64_CFLAGS_ABS := $(subst -isystem $(MUSL_ROOT)/include,-isystem $(abspath $(MUSL_ROOT))/include,$(LIBC64_CFLAGS))
LIBC64_LINK = $(CC) $(TFLAGS) -static -no-pie -nostdlib -s \
              -Wl,--build-id=none -Wl,-z,max-page-size=0x1000 \
              -T $(TLD) $(MUSL_ROOT)/lib/crt1.o \
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

$(BUILD)/user64/pthreadtest.elf: libc/tests/pthreadtest.c $(LIBC64) libc/libc64.ld \
                                 | $(BUILD)/user64 $(BUILD)/libc64
	$(CC) $(LIBC64_CFLAGS) -Wall -Wextra -c $< -o $(BUILD)/libc64/pthreadtest.o
	$(call LIBC64_LINK,$(BUILD)/libc64/pthreadtest.o) -o $@

# Long paths and directory listings through the C library (t_paths)
$(BUILD)/user64/pathtest.elf: libc/tests/pathtest.c $(LIBC64) libc/libc64.ld \
                              | $(BUILD)/user64 $(BUILD)/libc64
	$(CC) $(LIBC64_CFLAGS) -Wall -Wextra -c $< -o $(BUILD)/libc64/pathtest.o
	$(call LIBC64_LINK,$(BUILD)/libc64/pathtest.o) -o $@

# Every file under a directory with its SHA-256 (t_tfs4read)
$(BUILD)/user64/fswalk.elf: libc/tests/fswalk.c user/usha256.h $(LIBC64) \
                            libc/libc64.ld | $(BUILD)/user64 $(BUILD)/libc64
	$(CC) $(LIBC64_CFLAGS) -Wall -Wextra -c $< -o $(BUILD)/libc64/fswalk.o
	$(call LIBC64_LINK,$(BUILD)/libc64/fswalk.o) -o $@

# A day of writing for e2fsck to judge afterwards (t_tfs4write)
$(BUILD)/user64/fswrite.elf: libc/tests/fswrite.c $(LIBC64) libc/libc64.ld \
                             | $(BUILD)/user64 $(BUILD)/libc64
	$(CC) $(LIBC64_CFLAGS) -Wall -Wextra -c $< -o $(BUILD)/libc64/fswrite.o
	$(call LIBC64_LINK,$(BUILD)/libc64/fswrite.o) -o $@

# The mapping area at 512 GiB: gigabytes of it, fork and threads there
$(BUILD)/user64/bigmap.elf: libc/tests/bigmap.c $(LIBC64) libc/libc64.ld \
                            | $(BUILD)/user64 $(BUILD)/libc64
	$(CC) $(LIBC64_CFLAGS) -Wall -Wextra -c $< -o $(BUILD)/libc64/bigmap.o
	$(call LIBC64_LINK,$(BUILD)/libc64/bigmap.o) -o $@

# A program's memory above the low gigabyte
$(BUILD)/user64/bigram.elf: libc/tests/bigram.c $(LIBC64) libc/libc64.ld \
                            | $(BUILD)/user64 $(BUILD)/libc64
	$(CC) $(LIBC64_CFLAGS) -Wall -Wextra -c $< -o $(BUILD)/libc64/bigram.o
	$(call LIBC64_LINK,$(BUILD)/libc64/bigram.o) -o $@

# Processes talking: socket pairs, descriptors sent over them, memfds
$(BUILD)/user64/ipctest.elf: libc/tests/ipctest.c $(LIBC64) libc/libc64.ld \
                             | $(BUILD)/user64 $(BUILD)/libc64
	$(CC) $(LIBC64_CFLAGS) -Wall -Wextra -c $< -o $(BUILD)/libc64/ipctest.o
	$(call LIBC64_LINK,$(BUILD)/libc64/ipctest.o) -o $@

# Signals: handlers with siginfo_t and the interrupted registers, masks,
# sigsuspend, waits that go on, WebKit's way of stopping a thread - the
# same program the ARM64 kernel runs, and the tests compare both with
# its glibc build
$(BUILD)/user64/sigtest.elf: libc/tests/sigtest.c $(LIBC64) libc/libc64.ld \
                             | $(BUILD)/user64 $(BUILD)/libc64
	$(CC) $(LIBC64_CFLAGS) -Wall -Wextra -c $< -o $(BUILD)/libc64/sigtest.o
	$(call LIBC64_LINK,$(BUILD)/libc64/sigtest.o) -o $@

$(BUILD)/user64/nettest.elf: libc/tests/nettest.c $(LIBC64) libc/libc64.ld \
                             | $(BUILD)/user64 $(BUILD)/libc64
	$(CC) $(LIBC64_CFLAGS) -Wall -Wextra -c $< -o $(BUILD)/libc64/nettest.o
	$(call LIBC64_LINK,$(BUILD)/libc64/nettest.o) -o $@

$(BUILD)/user64/randtest.elf: libc/tests/randtest.c $(LIBC64) libc/libc64.ld \
                              | $(BUILD)/user64 $(BUILD)/libc64
	$(CC) $(LIBC64_CFLAGS) -Wall -Wextra -c $< -o $(BUILD)/libc64/randtest.o
	$(call LIBC64_LINK,$(BUILD)/libc64/randtest.o) -o $@

# --- zlib ---------------------------------------------------------------
# The release archive as zlib.net publishes it, unpacked into the build
# directory; zconf.h comes ready-made, so there is no configure step,
# only the library's fifteen files compiled against musl.
ZLIB_TAR  := third_party/zlib-1.4.tar.xz
ZLIB_TREE := $(BUILD)/zlib
ZLIB_SRC  := adler32 compress crc32 deflate gzclose gzlib gzread gzwrite \
             infback inffast inflate inftrees trees uncompr zutil
ZLIB_A    := $(ZLIB_TREE)/libz.a

$(ZLIB_TREE)/zlib.h: $(ZLIB_TAR)
	rm -rf $(ZLIB_TREE) && mkdir -p $(ZLIB_TREE)
	tar -xf $< -C $(ZLIB_TREE) --strip-components=1
	touch $@

$(ZLIB_TREE)/%.o: $(ZLIB_TREE)/zlib.h $(MUSL_HEADERS)
	$(CC) $(LIBC64_CFLAGS) -w -DHAVE_UNISTD_H -DHAVE_STDARG_H \
	    -c $(ZLIB_TREE)/$*.c -o $@

$(ZLIB_A): $(patsubst %,$(ZLIB_TREE)/%.o,$(ZLIB_SRC))
	rm -f $@ && $(AR) rcs $@ $^

$(BUILD)/user64/zlibtest.elf: libc/tests/zlibtest.c $(ZLIB_A) $(LIBC64) \
                              libc/libc64.ld | $(BUILD)/user64 $(BUILD)/libc64
	$(CC) $(LIBC64_CFLAGS) -I$(ZLIB_TREE) -Wall -Wextra -c $< \
	    -o $(BUILD)/libc64/zlibtest.o
	$(call LIBC64_LINK,$(BUILD)/libc64/zlibtest.o $(ZLIB_A)) -o $@

# The Camera: zlib for the PNGs it saves, and memory given back (here,
# under zlib, because a prerequisite takes a variable's value where it
# is read)
$(BUILD)/user64/gcamera.elf: user64/gcamera.c user/uapi.h user/udraw.h \
                             user/ucam.h user/ujpeg.h user64/usys64.h \
                             user64/compat64.h $(ZLIB_A) $(LIBC64) \
                             libc/libc64.ld | $(BUILD)/user64 $(BUILD)/libc64
	$(CC) $(LIBC64_CFLAGS) -Iuser -I$(ZLIB_TREE) -D_GNU_SOURCE \
	    -DTHREADOS_LIBC -Wall -Wextra -c $< -o $(BUILD)/libc64/gcamera.o
	$(call LIBC64_LINK,$(BUILD)/libc64/gcamera.o $(ZLIB_A)) -o $@

# --- Mbed TLS -----------------------------------------------------------
# TLS 1.2 and 1.3, X.509 and the cryptography under them. The release
# archive as Mbed TLS publishes it, its library built by its own
# makefile with the default configuration; random bytes come from
# getrandom(), which is the kernel's (kernel/random.c). Its self-test
# program runs the library's known-answer tests on ThreadOS.
MBEDTLS_TAR   := third_party/mbedtls-3.6.7.tar.bz2
MBEDTLS_TREE  := $(BUILD)/mbedtls
MBEDTLS_STAMP := $(MBEDTLS_TREE)/lib/.built
MBEDTLS_A     := $(MBEDTLS_TREE)/lib/libmbedtls.a \
                 $(MBEDTLS_TREE)/lib/libmbedx509.a \
                 $(MBEDTLS_TREE)/lib/libmbedcrypto.a

$(MBEDTLS_STAMP): $(MBEDTLS_TAR) Makefile $(MUSL_HEADERS)
	rm -rf $(MBEDTLS_TREE) && mkdir -p $(MBEDTLS_TREE)
	tar -xf $< -C $(MBEDTLS_TREE) --strip-components=1
	$(MAKE) -s -C $(MBEDTLS_TREE)/library CC="$(CC)" \
	    CFLAGS="$(LIBC64_CFLAGS_ABS)" static >/dev/null
	mkdir -p $(MBEDTLS_TREE)/lib
	cp $(MBEDTLS_TREE)/library/libmbed*.a $(MBEDTLS_TREE)/lib/
	touch $@

$(BUILD)/user64/mbedtest.elf: $(MBEDTLS_STAMP) | $(BUILD)/user64 $(BUILD)/libc64
	$(CC) $(LIBC64_CFLAGS) -I$(MBEDTLS_TREE)/include -w \
	    -c $(MBEDTLS_TREE)/programs/test/selftest.c \
	    -o $(BUILD)/libc64/mbedtest.o
	$(call LIBC64_LINK,$(BUILD)/libc64/mbedtest.o $(MBEDTLS_A)) -o $@

USER64_ELFS += $(BUILD)/user64/mbedtest.elf

# --- curl ---------------------------------------------------------------
# The release archive as curl.se publishes it, configured the way it is
# for any other system with a C library: its own configure script, told
# to cross-compile, linking its test programs the way every program here
# is linked (musl's start files around them, libc.a and libgcc after).
# HTTP and HTTPS (TLS through Mbed TLS, below); no IPv6 and no Unix
# sockets (the kernel has neither), and the resolver in
# the calling thread, since socketpair() is not there to wake a resolver
# thread with.
CURL_TAR   := third_party/curl-8.23.0.tar.xz
CURL_TREE  := $(BUILD)/curl
CURL_LIB   := $(CURL_TREE)/lib/.libs/libcurl.a
CURL_TOOL  := $(CURL_TREE)/src/curl
CURL_LDFLAGS := -static -no-pie -nostdlib -Wl,--build-id=none \
                -Wl,-z,max-page-size=0x1000 -T $(abspath libc/libc64.ld) \
                $(abspath $(MUSL_ROOT)/lib/crt1.o) $(abspath $(MUSL_ROOT)/lib/crti.o) \
                -L$(abspath $(ZLIB_TREE))
CURL_LIBS  := $(abspath $(ZLIB_A)) $(abspath $(LIBC64)) \
              $(shell $(CC) -print-libgcc-file-name) $(abspath $(MUSL_ROOT)/lib/crtn.o)

$(CURL_TREE)/Makefile: $(CURL_TAR) Makefile $(LIBC64) $(ZLIB_A) $(MBEDTLS_STAMP)
	rm -rf $(CURL_TREE) && mkdir -p $(CURL_TREE)
	tar -xf $< -C $(CURL_TREE) --strip-components=1
	cd $(CURL_TREE) && ./configure --host=$(THOST) \
	    --build=x86_64-pc-linux-gnu CC="$(CC)" \
	    CFLAGS="$(LIBC64_CFLAGS_ABS) -D_GNU_SOURCE" \
	    CPPFLAGS="-I$(abspath $(ZLIB_TREE))" \
	    LDFLAGS="$(CURL_LDFLAGS)" LIBS="$(CURL_LIBS)" \
	    --disable-shared --enable-static --with-mbedtls=$(abspath $(MBEDTLS_TREE)) \
	    --with-zlib \
	    --disable-ipv6 --disable-unix-sockets --disable-threaded-resolver \
	    --disable-socketpair --disable-ldap --disable-ldaps --disable-rtsp \
	    --disable-dict --disable-telnet --disable-tftp --disable-pop3 \
	    --disable-imap --disable-smb --disable-smtp --disable-gopher \
	    --disable-mqtt --disable-manual --disable-docs --disable-ntlm \
	    --disable-kerberos-auth --without-libpsl --without-brotli \
	    --without-zstd --without-nghttp2 --without-libidn2 \
	    --with-ca-bundle=/etc/ssl/certs/ca-certificates.crt --without-ca-path \
	    > configure.log 2>&1 || { tail -40 configure.log; exit 1; }

$(CURL_TOOL): $(CURL_TREE)/Makefile
	$(MAKE) -s -C $(CURL_TREE)/lib
	$(MAKE) -s -C $(CURL_TREE)/src curl

$(BUILD)/user64/curl.elf: $(CURL_TOOL) | $(BUILD)/user64
	$(STRIP) -o $@ $<

# --- FreeType and HarfBuzz ----------------------------------------------
# FreeType draws glyphs (TrueType and CFF outlines, hinted or not) and
# HarfBuzz shapes text: which glyph for which letters, and where, with
# the font's kerning and ligatures. FreeType is its release archive,
# built from the modules apps/fonts/ftmodule.h names, WOFF through our
# zlib; HarfBuzz is its src/ directory (trimmed, see third_party/README),
# built through its one-file harfbuzz.cc with the FreeType glue.
FT_TAR  := third_party/freetype-2.15.0.tar.xz
FT_TREE := $(BUILD)/freetype
FT_SRC  := base/ftsystem base/ftinit base/ftdebug base/ftbase base/ftbbox \
           base/ftbitmap base/ftglyph base/ftmm base/ftsynth \
           truetype/truetype sfnt/sfnt cff/cff psaux/psaux \
           pshinter/pshinter psnames/psnames smooth/smooth autofit/autofit \
           gzip/ftgzip
FT_A    := $(FT_TREE)/libfreetype.a
FT_CFLAGS := -DFT2_BUILD_LIBRARY -DFT_CONFIG_MODULES_H="<ftmodule.h>" \
             -DFT_CONFIG_OPTION_SYSTEM_ZLIB -Iapps/fonts \
             -I$(FT_TREE)/include -I$(ZLIB_TREE)
HB_DIR  := third_party/harfbuzz
HB_O    := $(BUILD)/harfbuzz/harfbuzz.o

$(FT_TREE)/include/ft2build.h: $(FT_TAR)
	rm -rf $(FT_TREE) && mkdir -p $(FT_TREE)
	tar -xf $< -C $(FT_TREE) --strip-components=1
	touch $@

$(FT_TREE)/%.o: $(FT_TREE)/include/ft2build.h apps/fonts/ftmodule.h \
                $(ZLIB_A) $(MUSL_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(LIBC64_CFLAGS) -w $(FT_CFLAGS) -c $(FT_TREE)/src/$*.c -o $@

$(FT_A): $(patsubst %,$(FT_TREE)/%.o,$(FT_SRC))
	rm -f $@ && $(AR) rcs $@ $^

$(HB_O): $(wildcard $(HB_DIR)/src/*.cc $(HB_DIR)/src/*.hh $(HB_DIR)/src/*.h) \
         $(FT_TREE)/include/ft2build.h $(MUSL_HEADERS)
	mkdir -p $(dir $@)
	$(CXX) $(CXX64_FLAGS) -w -fno-rtti -DHAVE_FREETYPE=1 \
	    -I$(FT_TREE)/include -c $(HB_DIR)/src/harfbuzz.cc -o $@

$(BUILD)/user64/fonttest.elf: libc/tests/fonttest.c $(FT_A) $(HB_O) $(ZLIB_A) \
                              $(BUILD)/libc64/cxxrt.o libc/libc64.ld \
                              | $(BUILD)/user64 $(BUILD)/libc64
	$(CC) $(LIBC64_CFLAGS) -I$(FT_TREE)/include -I$(HB_DIR)/src -Wall -Wextra \
	    -c $< -o $(BUILD)/libc64/fonttest.o
	$(call CXX64_LINK,$(BUILD)/libc64/fonttest.o $(HB_O) $(FT_A) $(ZLIB_A)) -o $@

# --- SQLite -----------------------------------------------------------
# The amalgamation exactly as sqlite.org publishes it, unpacked into the
# build directory. Thread-safe, now that musl has threads (and PRAGMA
# threads sorts on several cores); without loadable extensions (there
# is no dlopen) and without memory-mapped I/O (the C library maps no
# files). The math functions and FTS5 are what a real application
# expects to find.
SQLITE_ZIP  := third_party/sqlite-amalgamation-3540100.zip
SQLITE_SRC  := $(BUILD)/sqlite
SQLITE_OPTS := -DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_LOAD_EXTENSION \
               -DSQLITE_MAX_MMAP_SIZE=0 -DSQLITE_ENABLE_MATH_FUNCTIONS \
               -DSQLITE_ENABLE_FTS5

$(SQLITE_SRC)/sqlite3.c: $(SQLITE_ZIP)
	rm -rf $(SQLITE_SRC) && mkdir -p $(SQLITE_SRC)
	python3 scripts/unzipflat.py $< $(SQLITE_SRC)

$(SQLITE_SRC)/shell.c: $(SQLITE_SRC)/sqlite3.c

$(BUILD)/libc64/sqlite3.o: $(SQLITE_SRC)/sqlite3.c $(MUSL_HEADERS) | $(BUILD)/libc64
	$(CC) $(LIBC64_CFLAGS) -w $(SQLITE_OPTS) -c $< -o $@

$(BUILD)/libc64/sqlite-shell.o: $(SQLITE_SRC)/shell.c $(MUSL_HEADERS) | $(BUILD)/libc64
	$(CC) $(LIBC64_CFLAGS) -w $(SQLITE_OPTS) -I$(SQLITE_SRC) -c $< -o $@

$(BUILD)/user64/sqlite3.elf: $(BUILD)/libc64/sqlite3.o $(BUILD)/libc64/sqlite-shell.o \
                             libc/libc64.ld | $(BUILD)/user64
	$(call LIBC64_LINK,$(BUILD)/libc64/sqlite-shell.o $(BUILD)/libc64/sqlite3.o) -o $@

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

$(BUILD)/libc64/qjs-%.o: third_party/quickjs/%.c $(QJS_HDRS) $(MUSL_HEADERS) \
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
               $(BUILD)/user64/pthreadtest.elf $(BUILD)/user64/sqlite3.elf \
               $(BUILD)/user64/pathtest.elf $(BUILD)/user64/fswalk.elf \
               $(BUILD)/user64/fswrite.elf \
               $(BUILD)/user64/bigmap.elf \
               $(BUILD)/user64/ipctest.elf $(BUILD)/user64/sigtest.elf \
               $(BUILD)/user64/bigram.elf \
               $(BUILD)/user64/glibtest.elf \
               $(BUILD)/user64/imgtest.elf $(BUILD)/user64/xmltest.elf \
               $(BUILD)/user64/fctest.elf $(BUILD)/user64/souptest.elf \
               $(BUILD)/user64/nettest.elf $(BUILD)/user64/zlibtest.elf \
               $(BUILD)/user64/randtest.elf \
               $(BUILD)/user64/curl.elf $(BUILD)/user64/fonttest.elf \
               $(BUILD)/user64/update.elf $(BUILD)/user64/wpedeps.elf \
               $(BUILD)/user64/tapp.elf $(BUILD)/user64/gtapp.elf

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
CXX64_FLAGS := $(TFLAGS) -std=gnu++17 -Os -ffunction-sections -fdata-sections \
               -fno-stack-protector \
               -fno-pic -fno-asynchronous-unwind-tables -fno-exceptions \
               -fno-threadsafe-statics -nostdinc -nostdinc++ $(CXX64_INCS) \
               -isystem $(MUSL_ROOT)/include \
               -isystem $(shell $(CC) -print-file-name=include) \
               -include libc/cxx/compat.h -DTHREADOS_LIBC

$(BUILD)/libc64/cxxrt.o: libc/cxx/cxxrt.cpp libc/cxx/compat.h $(MUSL_HEADERS) \
                         | $(BUILD)/libc64
	$(CXX) $(CXX64_FLAGS) -Wall -c $< -o $@

# litehtml (third_party/litehtml, upstream unchanged) lays out HTML and
# CSS; gumbo, inside it, parses the HTML. The files in LH_PATCHED are
# built from a patched copy - apps/browser/patches says what and why.
LH_DIR     := third_party/litehtml
LH_PATCHED := encodings url document css_length style css_properties \
              render_item flex_line render_flex css_selector html_tag media_query
LH_SRC     := $(filter-out $(LH_PATCHED),$(basename $(notdir $(wildcard $(LH_DIR)/src/*.cpp))))
GUMBO_SRC  := $(basename $(notdir $(wildcard $(LH_DIR)/src/gumbo/src/*.c)))
# lhgrid.cpp is ours: CSS grid layout, which litehtml has none of
LH_OBJS    := $(patsubst %,$(BUILD)/litehtml/lh-%.o,$(LH_SRC) $(LH_PATCHED)) \
              $(patsubst %,$(BUILD)/litehtml/gumbo-%.o,$(GUMBO_SRC)) \
              $(BUILD)/litehtml/lhgrid.o
# Headers are patched as the sources are: the include directory is
# copied into the build with apps/browser/patches/<name>.h.patch applied
# to <name>.h - all of it, since a header finds the others beside it
# first - and third_party/litehtml stays as upstream has it
LH_HDR_PATCHES := $(wildcard apps/browser/patches/*.h.patch)
LH_INC_DIR := $(BUILD)/litehtml/inc
LH_HDRS    := $(LH_INC_DIR)/.stamp $(wildcard $(LH_DIR)/src/*.h)
LH_INC     := -I$(LH_INC_DIR) -I$(LH_INC_DIR)/litehtml \
              -I$(LH_DIR)/src -I$(LH_DIR)/src/gumbo/src

$(LH_INC_DIR)/.stamp: $(wildcard $(LH_DIR)/include/*.h $(LH_DIR)/include/litehtml/*.h) \
                      $(LH_HDR_PATCHES) Makefile
	@set -e; mkdir -p $(LH_INC_DIR)/litehtml; \
	cp -f $(LH_DIR)/include/*.h $(LH_INC_DIR)/; \
	cp -f $(LH_DIR)/include/litehtml/*.h $(LH_INC_DIR)/litehtml/; \
	for p in $(LH_HDR_PATCHES); do \
	    h=$(LH_INC_DIR)/litehtml/$$(basename $$p .patch); \
	    sed 's/\r$$//' $$h > $$h.orig; \
	    sed 's/\r$$//' $$p | patch -s -o $$h $$h.orig; \
	done; touch $@

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
$(BUILD)/litehtml/lh-%.o: $(BUILD)/litehtml/%.cpp $(LH_HDRS) $(MUSL_HEADERS) \
                          | $(BUILD)/litehtml
	$(CXX) $(CXX64_FLAGS) -w $(LH_INC) -c $< -o $@

$(BUILD)/litehtml/lh-%.o: $(LH_DIR)/src/%.cpp $(LH_HDRS) $(MUSL_HEADERS) \
                          | $(BUILD)/litehtml
	$(CXX) $(CXX64_FLAGS) -w $(LH_INC) -c $< -o $@

$(BUILD)/litehtml/lhgrid.o: apps/browser/lhgrid.cpp $(LH_HDRS) $(MUSL_HEADERS) \
                           | $(BUILD)/litehtml
	$(CXX) $(CXX64_FLAGS) -Wall $(LH_INC) -c $< -o $@

$(BUILD)/litehtml/gumbo-%.o: $(LH_DIR)/src/gumbo/src/%.c $(MUSL_HEADERS) \
                             | $(BUILD)/litehtml
	$(CC) $(LIBC64_CFLAGS) -w -c $< -o $@

# The C++ link: musl's start files, the program, libstdc++ for what
# the headers do not carry, and the C library after it
CXX64_LINK = $(call LIBC64_LINK,$(1) $(BUILD)/libc64/cxxrt.o \
                                $(LIBSTDCXX)) -Wl,--gc-sections

# --- ICU, for WebKit ------------------------------------------------
# ICU 78.3 as unicode.org releases it (third_party/README.md). Its
# tools are built for this machine first - icupkg trims the data,
# genccode makes an object of it - then its libraries for ThreadOS by
# apps/webkit/icu.mk, from the sources straight, and the data cut down
# to what apps/webkit/icu-data.py keeps (32 MB to 4).
ICU_TGZ    := third_party/icu/icu4c-78.3-sources.tgz
ICU_DIR    := $(BUILD)/icu
ICU_SRC    := $(ICU_DIR)/icu/source
ICU_HOST   := $(ICU_DIR)/host
ICU_ROOT   := $(ICU_DIR)/root
ICU_DAT    := $(ICU_DIR)/data/icudt78l.dat
ICU_STAMP  := $(ICU_ROOT)/built
ICU_HOSTRUN = env LD_LIBRARY_PATH=$(abspath $(ICU_HOST))/lib

$(ICU_SRC)/runConfigureICU: $(ICU_TGZ)
	rm -rf $(ICU_DIR)/icu && mkdir -p $(ICU_DIR)
	tar -xzf $< -C $(ICU_DIR)
	touch $@

$(ICU_HOST)/bin/genccode: $(ICU_SRC)/runConfigureICU
	rm -rf $(ICU_HOST) && mkdir -p $(ICU_HOST)
	cd $(ICU_HOST) && $(abspath $(ICU_SRC))/runConfigureICU Linux \
	    --disable-tests --disable-samples --disable-extras \
	    --disable-icuio --disable-layoutex > configure.log
	$(MAKE) -C $(ICU_HOST) > $(ICU_HOST)/make.log
	touch $@

$(ICU_DAT): $(ICU_HOST)/bin/genccode apps/webkit/icu-data.py
	rm -rf $(dir $@) && mkdir -p $(dir $@)
	cp $(ICU_SRC)/data/in/icudt78l.dat $@
	$(ICU_HOSTRUN) $(ICU_HOST)/bin/icupkg -l $@ | \
	    python3 apps/webkit/icu-data.py > $(dir $@)remove.txt
	$(ICU_HOSTRUN) $(ICU_HOST)/bin/icupkg --ignore-deps \
	    -r $(dir $@)remove.txt $@ 2> /dev/null

# Thread-safe statics: ICU runs on JavaScriptCore's threads too
$(ICU_STAMP): $(ICU_DAT) apps/webkit/icu.mk $(MUSL_HEADERS) libc/cxx/compat.h
	$(MAKE) -f apps/webkit/icu.mk SRC=$(ICU_SRC) OUT=$(ICU_DIR)/obj \
	    DAT=$(ICU_DAT) CXX="$(CXX)" CC="$(CC)" AR="$(AR)" \
	    GENCCODE="$(ICU_HOSTRUN) $(abspath $(ICU_HOST))/bin/genccode" \
	    CXXFLAGS="$(filter-out -fno-threadsafe-statics,$(CXX64_FLAGS)) -w"
	rm -rf $(ICU_ROOT) && mkdir -p $(ICU_ROOT)/include/unicode $(ICU_ROOT)/lib
	cp $(ICU_SRC)/common/unicode/*.h $(ICU_SRC)/i18n/unicode/*.h \
	    $(ICU_ROOT)/include/unicode/
	cp $(ICU_DIR)/obj/*.a $(ICU_ROOT)/lib/
	touch $@

# --- Libraries for WebKit's processes: GLib and what it needs -----------
# What WebKit's processes are built on, each built from its release
# archive in third_party/ by its own build system - configure or meson -
# for ThreadOS, and installed into one place, BUILD/libs64 (include/,
# lib/, lib/pkgconfig/), where the next one finds it. The compiler they
# are all given is libc/threados-cc.in, written out for this build into
# BUILD/toolchain with its paths; meson gets libc/meson-cross.ini.in the
# same way. Nothing is fetched: meson's fallbacks are off
# (--wrap-mode=nodownload), and zlib is ours, installed with a .pc file.
LIBS64      := $(BUILD)/libs64
TOOLCHAIN64 := $(BUILD)/toolchain
TOS_CC      := $(TOOLCHAIN64)/threados-cc
MESON_CROSS := $(TOOLCHAIN64)/cross.ini
LIBS64_PC   := PKG_CONFIG_LIBDIR=$(abspath $(LIBS64))/lib/pkgconfig

# The Makefile says the machine (TARGET) these are written for. A copy
# that came out the same is not written again, and keeps its time: an
# edit of the Makefile does not cost every library its build.
$(TOS_CC): libc/threados-cc.in $(MUSL_HEADERS) Makefile
	mkdir -p $(TOOLCHAIN64)
	sed -e 's|@MUSL@|$(abspath $(MUSL_ROOT))|' -e 's|@REPO@|$(CURDIR)|' \
	    -e 's|@LIBS@|$(abspath $(LIBS64))|' -e 's|@CC@|$(CC)|' \
	    -e 's|@FLAGS@|$(TFLAGS)|' -e 's|@LDSCRIPT@|$(TLD)|' $< > $@.new
	chmod +x $@.new
	cmp -s $@.new $@ && rm -f $@.new || mv $@.new $@

$(MESON_CROSS): libc/meson-cross.ini.in Makefile
	mkdir -p $(TOOLCHAIN64)
	sed -e 's|@TOOLCHAIN@|$(abspath $(TOOLCHAIN64))|' \
	    -e 's|@LIBS@|$(abspath $(LIBS64))|' -e 's|@CPU@|$(TCPU)|' \
	    -e 's|@AR@|$(AR)|' -e 's|@STRIP@|$(STRIP)|' $< > $@.new
	cmp -s $@.new $@ && rm -f $@.new || mv $@.new $@

# A C++ compiler for meson projects that declare the language but whose
# parts built here are C (gst-plugins-bad): its own cross file, given
# after the first, so that the C libraries' cross file - which every
# meson library's build depends on - stays as it is
TOS_CXX         := $(TOOLCHAIN64)/threados-c++
MESON_CROSS_CXX := $(TOOLCHAIN64)/cross-cxx.ini
$(TOS_CXX): libc/threados-c++.in $(TOS_CC) Makefile
	sed -e 's|@CXX@|$(CXX)|' -e 's|@CXXINCS@|$(CXX64_INCS)|' \
	    -e 's|@TOOLCHAIN@|$(abspath $(TOOLCHAIN64))|' \
	    -e 's|@MUSL@|$(abspath $(MUSL_ROOT))|' -e 's|@LIBS@|$(abspath $(LIBS64))|' \
	    -e 's|@REPO@|$(CURDIR)|' -e 's|@FLAGS@|$(TFLAGS)|' $< > $@.new
	chmod +x $@.new
	cmp -s $@.new $@ && rm -f $@.new || mv $@.new $@
$(MESON_CROSS_CXX): $(TOS_CXX)
	printf "[binaries]\ncpp = '%s'\n" $(abspath $(TOS_CXX)) > $@

# GLib asks for -latomic. On x86-64 the compiler does every atomic
# operation itself, so ThreadOS's libatomic is empty - and being first
# in the search, it is the one found, never the host's (a glibc build).
LIBS64_ATOMIC := $(LIBS64)/lib/libatomic.a
$(LIBS64_ATOMIC): $(TOS_CC)
	mkdir -p $(LIBS64)/lib
	echo > $(BUILD)/libatomic-empty.c
	$(TOS_CC) -c $(BUILD)/libatomic-empty.c -o $(BUILD)/libatomic-empty.o
	rm -f $@ && $(AR) rcs $@ $(BUILD)/libatomic-empty.o

# (install -C: zlib is compiled again whenever libc.a changes, into the
# same library, and a copy that came out the same keeps its time - so
# the libraries built on it, from GLib to FreeType and HarfBuzz, and
# WebKit above them, are not built again for a fix in the C library)
LIBS64_ZLIB := $(LIBS64)/lib/libz.a
$(LIBS64_ZLIB): $(ZLIB_A)
	mkdir -p $(LIBS64)/include $(LIBS64)/lib/pkgconfig
	install -C -m 644 $(ZLIB_TREE)/zlib.h $(ZLIB_TREE)/zconf.h $(LIBS64)/include/
	install -C -m 644 $(ZLIB_A) $@
	printf 'prefix=%s\nlibdir=$${prefix}/lib\nincludedir=$${prefix}/include\n\nName: zlib\nDescription: zlib compression library\nVersion: %s\nLibs: -L$${libdir} -lz\nCflags: -I$${includedir}\n' \
	    $(abspath $(LIBS64)) $(patsubst third_party/zlib-%.tar.xz,%,$(ZLIB_TAR)) \
	    > $(LIBS64)/lib/pkgconfig/zlib.pc

# libffi: calls a C function given its signature at run time - GObject's
# signals reach their handlers through it. Its static trampolines read
# the kernel's headers, and are only for closures, which GLib does not
# make.
LIBFFI_TAR := third_party/libffi-3.5.2.tar.gz
LIBFFI_A   := $(LIBS64)/lib/libffi.a
$(LIBFFI_A): $(LIBFFI_TAR) $(TOS_CC)
	rm -rf $(BUILD)/libffi && mkdir -p $(BUILD)/libffi
	tar -xf $< -C $(BUILD)/libffi --strip-components=1
	cd $(BUILD)/libffi && ./configure --host=$(THOST) \
	    --build=x86_64-pc-linux-gnu CC=$(abspath $(TOS_CC)) CFLAGS=-O2 \
	    --prefix=$(abspath $(LIBS64)) --disable-shared --enable-static \
	    --disable-docs --disable-multi-os-directory \
	    --disable-exec-static-tramp \
	    > configure.log 2>&1 || { tail -40 configure.log; exit 1; }
	$(MAKE) -C $(BUILD)/libffi install > $(BUILD)/libffi/make.log 2>&1 \
	    || { tail -40 $(BUILD)/libffi/make.log; exit 1; }

# PCRE2: GLib's regular expressions. No JIT: it writes code into memory
# and runs it, and ThreadOS hands out no memory that is both.
PCRE2_TAR := third_party/pcre2-10.48.tar.bz2
PCRE2_A   := $(LIBS64)/lib/libpcre2-8.a
$(PCRE2_A): $(PCRE2_TAR) $(TOS_CC)
	rm -rf $(BUILD)/pcre2 && mkdir -p $(BUILD)/pcre2
	tar -xf $< -C $(BUILD)/pcre2 --strip-components=1
	cd $(BUILD)/pcre2 && ./configure --host=$(THOST) \
	    --build=x86_64-pc-linux-gnu CC=$(abspath $(TOS_CC)) CFLAGS=-O2 \
	    --prefix=$(abspath $(LIBS64)) --disable-shared --enable-static \
	    --disable-jit --enable-pcre2-8 --disable-pcre2-16 \
	    --disable-pcre2-32 --enable-unicode \
	    > configure.log 2>&1 || { tail -40 configure.log; exit 1; }
	$(MAKE) -C $(BUILD)/pcre2 install > $(BUILD)/pcre2/make.log 2>&1 \
	    || { tail -40 $(BUILD)/pcre2/make.log; exit 1; }

# GLib: the main loop, threads, GObject, GIO. What a desktop Linux
# system has and ThreadOS does not - libmount, SELinux, extended
# attributes, translations - is switched off.
GLIB_TAR  := third_party/glib-2.90.1.tar.xz
GLIB_TREE := $(BUILD)/glib
GLIB_A    := $(LIBS64)/lib/libglib-2.0.a
$(GLIB_A): $(GLIB_TAR) $(MESON_CROSS) $(TOS_CC) $(LIBFFI_A) $(PCRE2_A) \
           $(LIBS64_ZLIB) $(LIBS64_ATOMIC)
	rm -rf $(GLIB_TREE) && mkdir -p $(GLIB_TREE)
	tar -xf $< -C $(GLIB_TREE) --strip-components=1
	cd $(GLIB_TREE) && meson setup build \
	    --cross-file $(abspath $(MESON_CROSS)) --wrap-mode=nodownload \
	    --prefix=$(abspath $(LIBS64)) --buildtype=release \
	    -Ddefault_library=static -Dtests=false -Dintrospection=disabled \
	    -Dnls=disabled -Dlibmount=disabled -Dselinux=disabled \
	    -Dxattr=false -Dlibelf=disabled -Dsysprof=disabled \
	    -Ddocumentation=false -Dman-pages=disabled -Ddtrace=disabled \
	    -Dsystemtap=disabled -Dglib_debug=disabled \
	    -Dbsymbolic_functions=false -Dinstalled_tests=false \
	    > setup.log 2>&1 || { tail -40 setup.log; exit 1; }
	ninja -C $(GLIB_TREE)/build install > $(GLIB_TREE)/ninja.log 2>&1 \
	    || { grep -A20 FAILED $(GLIB_TREE)/ninja.log | head -60; exit 1; }

libs64: $(GLIB_A)

# glibtest: GLib's main loop, threads, GObject, data and processes on
# ThreadOS, held by t_glib to the same program built against the host's
# GLib
$(BUILD)/user64/glibtest.elf: libc/tests/glibtest.c $(GLIB_A) $(TOS_CC) $(LIBC64) \
                              | $(BUILD)/user64
	$(TOS_CC) -O2 -Wall -Wextra -s -o $@ $< \
	    $$($(LIBS64_PC) pkg-config --cflags --static --libs gio-2.0 gio-unix-2.0)

# CMake gets libc/cmake-cross.cmake.in the way meson gets its cross file
CMAKE_CROSS := $(TOOLCHAIN64)/cross.cmake
$(CMAKE_CROSS): libc/cmake-cross.cmake.in Makefile
	mkdir -p $(TOOLCHAIN64)
	sed -e 's|@TOOLCHAIN@|$(abspath $(TOOLCHAIN64))|' \
	    -e 's|@LIBS@|$(abspath $(LIBS64))|' -e 's|@CPU@|$(TCPU)|' $< > $@.new
	cmp -s $@.new $@ && rm -f $@.new || mv $@.new $@

# libpng: PNG pictures
LIBPNG_TAR := third_party/libpng-1.6.59.tar.xz
LIBPNG_A   := $(LIBS64)/lib/libpng16.a
$(LIBPNG_A): $(LIBPNG_TAR) $(TOS_CC) $(LIBS64_ZLIB)
	rm -rf $(BUILD)/libpng && mkdir -p $(BUILD)/libpng
	tar -xf $< -C $(BUILD)/libpng --strip-components=1
	cd $(BUILD)/libpng && ./configure --host=$(THOST) \
	    --build=x86_64-pc-linux-gnu CC=$(abspath $(TOS_CC)) CFLAGS=-O2 \
	    CPPFLAGS=-I$(abspath $(LIBS64))/include \
	    LDFLAGS=-L$(abspath $(LIBS64))/lib \
	    --prefix=$(abspath $(LIBS64)) --disable-shared --enable-static \
	    > configure.log 2>&1 || { tail -40 configure.log; exit 1; }
	$(MAKE) -C $(BUILD)/libpng install > $(BUILD)/libpng/make.log 2>&1 \
	    || { tail -40 $(BUILD)/libpng/make.log; exit 1; }

# libjpeg-turbo: JPEG pictures. Without its SIMD code, which is written
# for NASM; the C it falls back on gives the same pixels.
LIBJPEG_TAR := third_party/libjpeg-turbo-3.2.1.tar.gz
LIBJPEG_A   := $(LIBS64)/lib/libjpeg.a
$(LIBJPEG_A): $(LIBJPEG_TAR) $(TOS_CC) $(CMAKE_CROSS)
	rm -rf $(BUILD)/libjpeg && mkdir -p $(BUILD)/libjpeg
	tar -xf $< -C $(BUILD)/libjpeg --strip-components=1
	cmake -S $(BUILD)/libjpeg -B $(BUILD)/libjpeg/build -G Ninja \
	    -DCMAKE_TOOLCHAIN_FILE=$(abspath $(CMAKE_CROSS)) \
	    -DCMAKE_BUILD_TYPE=Release \
	    -DCMAKE_INSTALL_PREFIX=$(abspath $(LIBS64)) \
	    -DCMAKE_INSTALL_LIBDIR=lib -DENABLE_SHARED=OFF -DENABLE_STATIC=ON \
	    -DWITH_SIMD=OFF -DWITH_TURBOJPEG=ON -DWITH_TOOLS=OFF \
	    -DWITH_TESTS=OFF > $(BUILD)/libjpeg/cmake.log 2>&1 \
	    || { tail -40 $(BUILD)/libjpeg/cmake.log; exit 1; }
	ninja -C $(BUILD)/libjpeg/build install > $(BUILD)/libjpeg/ninja.log 2>&1 \
	    || { tail -40 $(BUILD)/libjpeg/ninja.log; exit 1; }

# libxml2: XML, for the engine's XHTML, SVG and XML documents. Only the
# library is built and installed: its default targets include a module
# for its tests, and a module is something a static ThreadOS program
# cannot load.
LIBXML2_TAR  := third_party/libxml2-2.15.4.tar.xz
LIBXML2_TREE := $(BUILD)/libxml2
LIBXML2_A    := $(LIBS64)/lib/libxml2.a
$(LIBXML2_A): $(LIBXML2_TAR) $(MESON_CROSS) $(TOS_CC) $(LIBS64_ZLIB)
	rm -rf $(LIBXML2_TREE) && mkdir -p $(LIBXML2_TREE)
	tar -xf $< -C $(LIBXML2_TREE) --strip-components=1
	cd $(LIBXML2_TREE) && meson setup build \
	    --cross-file $(abspath $(MESON_CROSS)) --wrap-mode=nodownload \
	    --prefix=$(abspath $(LIBS64)) --libdir=lib --buildtype=release \
	    -Ddefault_library=static -Dpython=disabled -Dreadline=disabled \
	    -Dhistory=disabled -Ddocs=disabled -Dmodules=disabled \
	    -Dhttp=disabled -Dicu=disabled -Dlegacy=disabled -Diconv=enabled \
	    -Dzlib=enabled -Dthreads=enabled \
	    > setup.log 2>&1 || { tail -40 setup.log; exit 1; }
	ninja -C $(LIBXML2_TREE)/build libxml2.a > $(LIBXML2_TREE)/ninja.log 2>&1 \
	    || { grep -A20 FAILED $(LIBXML2_TREE)/ninja.log | head -60; exit 1; }
	meson install -C $(LIBXML2_TREE)/build --no-rebuild --tags devel \
	    > $(LIBXML2_TREE)/install.log 2>&1 \
	    || { tail -20 $(LIBXML2_TREE)/install.log; exit 1; }

# FreeType once more, whole this time, by its own configure: the one
# above (FT_A) has only the modules the system's programs draw with,
# and fontconfig, and later the engine, ask more of it - BDF and Type 1
# fonts, colour glyphs in PNG.
LIBS64_FT_A := $(LIBS64)/lib/libfreetype.a
$(LIBS64_FT_A): $(FT_TAR) $(TOS_CC) $(LIBS64_ZLIB) $(LIBPNG_A)
	rm -rf $(BUILD)/freetype64 && mkdir -p $(BUILD)/freetype64
	tar -xf $< -C $(BUILD)/freetype64 --strip-components=1
	cd $(BUILD)/freetype64 && $(LIBS64_PC) ./configure \
	    --host=$(THOST) --build=x86_64-pc-linux-gnu \
	    CC=$(abspath $(TOS_CC)) CFLAGS=-O2 --prefix=$(abspath $(LIBS64)) \
	    --disable-shared --enable-static --with-zlib=yes --with-png=yes \
	    --with-harfbuzz=no --with-brotli=no --with-bzip2=no \
	    > configure.log 2>&1 || { tail -40 configure.log; exit 1; }
	$(MAKE) -C $(BUILD)/freetype64 > $(BUILD)/freetype64/make.log 2>&1 \
	    || { tail -40 $(BUILD)/freetype64/make.log; exit 1; }
	$(MAKE) -C $(BUILD)/freetype64 install > $(BUILD)/freetype64/install.log \
	    2>&1 || { tail -20 $(BUILD)/freetype64/install.log; exit 1; }

# fontconfig: which font for the family a page names. Its configuration
# is read with libxml2 (no expat needed), its object table made with the
# host's gperf; its programs, tests and translations are not built.
FONTCONFIG_TAR  := third_party/fontconfig-2.18.3.tar.xz
FONTCONFIG_TREE := $(BUILD)/fontconfig
FONTCONFIG_A    := $(LIBS64)/lib/libfontconfig.a
$(FONTCONFIG_A): $(FONTCONFIG_TAR) $(MESON_CROSS) $(TOS_CC) $(LIBS64_FT_A) \
                 $(LIBXML2_A)
	rm -rf $(FONTCONFIG_TREE) && mkdir -p $(FONTCONFIG_TREE)
	tar -xf $< -C $(FONTCONFIG_TREE) --strip-components=1
	cd $(FONTCONFIG_TREE) && meson setup build \
	    --cross-file $(abspath $(MESON_CROSS)) --wrap-mode=nodownload \
	    --prefix=$(abspath $(LIBS64)) --libdir=lib --buildtype=release \
	    -Ddefault_library=static -Ddoc=disabled -Dnls=disabled \
	    -Dtests=disabled -Dtools=disabled -Dcache-build=disabled \
	    -Dxml-backend=libxml2 -Diconv=disabled \
	    > setup.log 2>&1 || { tail -40 setup.log; exit 1; }
	ninja -C $(FONTCONFIG_TREE)/build > $(FONTCONFIG_TREE)/ninja.log 2>&1 \
	    || { grep -A20 FAILED $(FONTCONFIG_TREE)/ninja.log | head -60; exit 1; }
	meson install -C $(FONTCONFIG_TREE)/build --no-rebuild --tags devel \
	    > $(FONTCONFIG_TREE)/install.log 2>&1 \
	    || { tail -20 $(FONTCONFIG_TREE)/install.log; exit 1; }

libs64: $(LIBPNG_A) $(LIBJPEG_A) $(LIBXML2_A) $(FONTCONFIG_A)

# Their tests: pictures held to Pillow (t_images), XML to ElementTree
# (t_xml), fonts to the host's fontconfig (t_fontconfig)
$(BUILD)/user64/imgtest.elf: libc/tests/imgtest.c $(LIBPNG_A) $(LIBJPEG_A) \
                             $(TOS_CC) $(LIBC64) | $(BUILD)/user64
	$(TOS_CC) -O2 -Wall -Wextra -s -o $@ $< \
	    $$($(LIBS64_PC) pkg-config --cflags --static --libs libpng16 libjpeg)

$(BUILD)/user64/xmltest.elf: libc/tests/xmltest.c $(LIBXML2_A) $(TOS_CC) $(LIBC64) \
                             | $(BUILD)/user64
	$(TOS_CC) -O2 -Wall -Wextra -s -o $@ $< \
	    $$($(LIBS64_PC) pkg-config --cflags --static --libs libxml-2.0)

$(BUILD)/user64/fctest.elf: libc/tests/fctest.c $(FONTCONFIG_A) $(TOS_CC) $(LIBC64) \
                            | $(BUILD)/user64
	$(TOS_CC) -O2 -Wall -Wextra -s -o $@ $< \
	    $$($(LIBS64_PC) pkg-config --cflags --static --libs fontconfig freetype2)

# --- The network: OpenSSL, nghttp2, libpsl, libsoup, glib-networking ---
# OpenSSL: TLS for glib-networking, the one backend of it ThreadOS can
# build (GnuTLS would bring nettle, GMP and libtasn1). No async (it
# needs ucontext), no secure heap (mlock and guard pages mean nothing
# here), no loadable engines or providers; its certificates are where
# musl's overlay serves the CA bundle, /etc/ssl/cert.pem.
OPENSSL_TAR := third_party/openssl-3.5.9.tar.gz
OPENSSL_A   := $(LIBS64)/lib/libssl.a
$(OPENSSL_A): $(OPENSSL_TAR) $(TOS_CC)
	rm -rf $(BUILD)/openssl && mkdir -p $(BUILD)/openssl
	tar -xf $< -C $(BUILD)/openssl --strip-components=1
	cd $(BUILD)/openssl && ./Configure $(OPENSSL_TARGET) no-shared no-dso \
	    no-engine no-async no-afalgeng no-secure-memory no-tests no-docs \
	    no-apps --prefix=$(abspath $(LIBS64)) --libdir=lib \
	    --openssldir=/etc/ssl CC=$(abspath $(TOS_CC)) \
	    > configure.log 2>&1 || { tail -40 configure.log; exit 1; }
	$(MAKE) -C $(BUILD)/openssl build_libs > $(BUILD)/openssl/make.log 2>&1 \
	    || { tail -40 $(BUILD)/openssl/make.log; exit 1; }
	$(MAKE) -C $(BUILD)/openssl install_dev > $(BUILD)/openssl/install.log \
	    2>&1 || { tail -20 $(BUILD)/openssl/install.log; exit 1; }

# nghttp2: HTTP/2's framing, for libsoup - the library alone
NGHTTP2_TAR := third_party/nghttp2-1.70.0.tar.xz
NGHTTP2_A   := $(LIBS64)/lib/libnghttp2.a
$(NGHTTP2_A): $(NGHTTP2_TAR) $(TOS_CC)
	rm -rf $(BUILD)/nghttp2 && mkdir -p $(BUILD)/nghttp2
	tar -xf $< -C $(BUILD)/nghttp2 --strip-components=1
	cd $(BUILD)/nghttp2 && ./configure --host=$(THOST) \
	    --build=x86_64-pc-linux-gnu CC=$(abspath $(TOS_CC)) CFLAGS=-O2 \
	    --prefix=$(abspath $(LIBS64)) --disable-shared --enable-static \
	    --enable-lib-only > configure.log 2>&1 \
	    || { tail -40 configure.log; exit 1; }
	$(MAKE) -C $(BUILD)/nghttp2 install > $(BUILD)/nghttp2/make.log 2>&1 \
	    || { tail -40 $(BUILD)/nghttp2/make.log; exit 1; }

# libpsl: the public suffix list - which domains a cookie may be set
# for - with the list from its archive built in, and no IDNA library
# (GLib turns international names to ASCII before libpsl sees them)
LIBPSL_TAR := third_party/libpsl-0.23.1.tar.gz
LIBPSL_A   := $(LIBS64)/lib/libpsl.a
$(LIBPSL_A): $(LIBPSL_TAR) $(MESON_CROSS) $(TOS_CC)
	rm -rf $(BUILD)/libpsl && mkdir -p $(BUILD)/libpsl
	tar -xf $< -C $(BUILD)/libpsl --strip-components=1
	cd $(BUILD)/libpsl && meson setup build \
	    --cross-file $(abspath $(MESON_CROSS)) --wrap-mode=nodownload \
	    --prefix=$(abspath $(LIBS64)) --libdir=lib --buildtype=release \
	    -Ddefault_library=static -Druntime=no -Dbuiltin=true \
	    -Dtests=false -Ddocs=false \
	    > setup.log 2>&1 || { tail -40 setup.log; exit 1; }
	ninja -C $(BUILD)/libpsl/build > $(BUILD)/libpsl/ninja.log 2>&1 \
	    || { grep -A20 FAILED $(BUILD)/libpsl/ninja.log | head -60; exit 1; }
	meson install -C $(BUILD)/libpsl/build --no-rebuild --tags devel \
	    > $(BUILD)/libpsl/install.log 2>&1 \
	    || { tail -20 $(BUILD)/libpsl/install.log; exit 1; }

# SQLite, the one built above, for libsoup's cookie and HSTS stores
LIBS64_SQLITE := $(LIBS64)/lib/libsqlite3.a
# (installed only where it changed, as zlib is above)
$(LIBS64_SQLITE): $(BUILD)/libc64/sqlite3.o $(SQLITE_SRC)/sqlite3.c
	mkdir -p $(LIBS64)/include $(LIBS64)/lib/pkgconfig
	rm -f $(BUILD)/libc64/libsqlite3.a
	$(AR) rcs $(BUILD)/libc64/libsqlite3.a $(BUILD)/libc64/sqlite3.o
	install -C -m 644 $(BUILD)/libc64/libsqlite3.a $@
	install -C -m 644 $(SQLITE_SRC)/sqlite3.h $(SQLITE_SRC)/sqlite3ext.h \
	    $(LIBS64)/include/
	printf 'prefix=%s\nlibdir=$${prefix}/lib\nincludedir=$${prefix}/include\n\nName: SQLite\nDescription: an SQL database engine\nVersion: %s\nLibs: -L$${libdir} -lsqlite3\nCflags: -I$${includedir}\n' \
	    $(abspath $(LIBS64)) \
	    $$(sed -n 's/^#define SQLITE_VERSION *"\(.*\)"/\1/p' $(SQLITE_SRC)/sqlite3.h) \
	    > $(LIBS64)/lib/pkgconfig/sqlite3.pc

# libsoup: HTTP for the engine's network process. Without the parts
# that need what ThreadOS has not (Kerberos, NTLM's helper program,
# Brotli and zstd decoding for now) or that are not for it (tests,
# introspection, documentation).
LIBSOUP_TAR  := third_party/libsoup-3.8.1.tar.xz
LIBSOUP_TREE := $(BUILD)/libsoup
LIBSOUP_A    := $(LIBS64)/lib/libsoup-3.0.a
$(LIBSOUP_A): $(LIBSOUP_TAR) $(MESON_CROSS) $(TOS_CC) $(GLIB_A) \
              $(NGHTTP2_A) $(LIBPSL_A) $(LIBS64_SQLITE)
	rm -rf $(LIBSOUP_TREE) && mkdir -p $(LIBSOUP_TREE)
	tar -xf $< -C $(LIBSOUP_TREE) --strip-components=1
	cd $(LIBSOUP_TREE) && meson setup build \
	    --cross-file $(abspath $(MESON_CROSS)) --wrap-mode=nodownload \
	    --prefix=$(abspath $(LIBS64)) --libdir=lib --buildtype=release \
	    -Ddefault_library=static -Dgssapi=disabled -Dntlm=disabled \
	    -Dbrotli=disabled -Dzstd=disabled -Dtls_check=false \
	    -Dintrospection=disabled -Dvapi=disabled -Ddocs=disabled \
	    -Ddoc_tests=false -Dtests=false -Dautobahn=disabled \
	    -Dinstalled_tests=false -Dsysprof=disabled -Dfuzzing=disabled \
	    -Dpkcs11_tests=disabled \
	    > setup.log 2>&1 || { tail -40 setup.log; exit 1; }
	ninja -C $(LIBSOUP_TREE)/build > $(LIBSOUP_TREE)/ninja.log 2>&1 \
	    || { grep -A20 FAILED $(LIBSOUP_TREE)/ninja.log | head -60; exit 1; }
	meson install -C $(LIBSOUP_TREE)/build --no-rebuild --tags devel \
	    > $(LIBSOUP_TREE)/install.log 2>&1 \
	    || { tail -20 $(LIBSOUP_TREE)/install.log; exit 1; }

# glib-networking: TLS for GIO, through OpenSSL, and proxies from the
# environment. Its modules are built into a program (static_library,
# in BUILD/libs64/lib/gio/modules) and registered by it -
# g_io_openssl_load(NULL) - since nothing loads modules here. It makes
# a loadable module first and its static library out of the module's
# objects, so those are position-independent (b_staticpic), and the
# module itself is the empty file libc/threados-cc leaves.
GIONET_TAR  := third_party/glib-networking-2.90.1.tar.xz
GIONET_TREE := $(BUILD)/glib-networking
GIONET_A    := $(LIBS64)/lib/gio/modules/libgioopenssl.a
$(GIONET_A): $(GIONET_TAR) $(MESON_CROSS) $(TOS_CC) $(GLIB_A) $(OPENSSL_A)
	rm -rf $(GIONET_TREE) && mkdir -p $(GIONET_TREE)
	tar -xf $< -C $(GIONET_TREE) --strip-components=1
	cd $(GIONET_TREE) && meson setup build \
	    --cross-file $(abspath $(MESON_CROSS)) --wrap-mode=nodownload \
	    --prefix=$(abspath $(LIBS64)) --libdir=lib --buildtype=release \
	    -Ddefault_library=static -Db_staticpic=true -Dgnutls=disabled \
	    -Dopenssl=enabled -Denvironment_proxy=enabled -Dlibproxy=disabled \
	    -Dgnome_proxy=disabled -Dtests=false -Dinstalled_tests=false \
	    > setup.log 2>&1 || { tail -40 setup.log; exit 1; }
	ninja -C $(GIONET_TREE)/build > $(GIONET_TREE)/ninja.log 2>&1 \
	    || { grep -A20 FAILED $(GIONET_TREE)/ninja.log | head -60; exit 1; }
	meson install -C $(GIONET_TREE)/build --no-rebuild \
	    > $(GIONET_TREE)/install.log 2>&1 \
	    || { tail -20 $(GIONET_TREE)/install.log; exit 1; }

libs64: $(LIBSOUP_A) $(GIONET_A)

# souptest: HTTP, HTTPS and HTTP/2 against t_soup's servers on the host
$(BUILD)/user64/souptest.elf: libc/tests/souptest.c $(LIBSOUP_A) $(GIONET_A) $(LIBC64) \
                              $(TOS_CC) | $(BUILD)/user64
	$(TOS_CC) -O2 -Wall -Wextra -s -o $@ $< \
	    $$($(LIBS64_PC):$(abspath $(LIBS64))/lib/gio/modules/pkgconfig \
	       pkg-config --cflags --static --libs libsoup-3.0 gioopenssl openssl)

# --- What WPE's port of WebKit needs besides ----------------------------
# OptionsWPE.cmake asks for these whatever is switched off; each is built
# from its release into BUILD/libs64 like the libraries above.

# libwebp: WebP pictures, and the demuxer that takes animations apart.
# Its SSE4.1 and AVX2 code is left out: ThreadOS is built for the first
# x86-64 (ARCH64, scripts/checkisa.py), and SSE2's is there.
WEBP_TAR := third_party/libwebp-1.6.0.tar.gz
WEBP_A   := $(LIBS64)/lib/libwebp.a
$(WEBP_A): $(WEBP_TAR) $(TOS_CC)
	rm -rf $(BUILD)/libwebp && mkdir -p $(BUILD)/libwebp
	tar -xf $< -C $(BUILD)/libwebp --strip-components=1
	cd $(BUILD)/libwebp && ./configure --host=$(THOST) \
	    --build=x86_64-pc-linux-gnu CC=$(abspath $(TOS_CC)) CFLAGS=-O2 \
	    --prefix=$(abspath $(LIBS64)) --disable-shared --enable-static \
	    --disable-sse4.1 --disable-avx2 --disable-gl --disable-sdl \
	    --disable-png --disable-jpeg --disable-tiff --disable-gif \
	    --disable-wic --enable-libwebpmux --enable-libwebpdemux \
	    > configure.log 2>&1 || { tail -40 configure.log; exit 1; }
	$(MAKE) -C $(BUILD)/libwebp/sharpyuv install \
	    > $(BUILD)/libwebp/make.log 2>&1 \
	    || { tail -40 $(BUILD)/libwebp/make.log; exit 1; }
	$(MAKE) -C $(BUILD)/libwebp/src install \
	    >> $(BUILD)/libwebp/make.log 2>&1 \
	    || { tail -40 $(BUILD)/libwebp/make.log; exit 1; }

# libtasn1: DER, for the keys Web Crypto imports
TASN1_TAR := third_party/libtasn1-4.21.0.tar.gz
TASN1_A   := $(LIBS64)/lib/libtasn1.a
$(TASN1_A): $(TASN1_TAR) $(TOS_CC)
	rm -rf $(BUILD)/libtasn1 && mkdir -p $(BUILD)/libtasn1
	tar -xf $< -C $(BUILD)/libtasn1 --strip-components=1
	cd $(BUILD)/libtasn1 && ./configure --host=$(THOST) \
	    --build=x86_64-pc-linux-gnu CC=$(abspath $(TOS_CC)) CFLAGS=-O2 \
	    --prefix=$(abspath $(LIBS64)) --disable-shared --enable-static \
	    --disable-doc --disable-valgrind-tests \
	    > configure.log 2>&1 || { tail -40 configure.log; exit 1; }
	$(MAKE) -C $(BUILD)/libtasn1/lib install \
	    > $(BUILD)/libtasn1/make.log 2>&1 \
	    || { tail -40 $(BUILD)/libtasn1/make.log; exit 1; }

# libgcrypt, and libgpg-error under it: Web Crypto's algorithms and the
# digests WebCore takes. With threads - WebKit calls it from several -
# and without its assembly and the AES-NI, SHA, PCLMUL and AVX paths:
# the first x86-64 again (the C of each is there).
GPGERR_TAR := third_party/libgpg-error-1.61.tar.bz2
GPGERR_A   := $(LIBS64)/lib/libgpg-error.a
$(GPGERR_A): $(GPGERR_TAR) $(TOS_CC)
	rm -rf $(BUILD)/libgpg-error && mkdir -p $(BUILD)/libgpg-error
	tar -xf $< -C $(BUILD)/libgpg-error --strip-components=1
	cd $(BUILD)/libgpg-error && ./configure --host=$(THOST) \
	    --build=x86_64-pc-linux-gnu CC=$(abspath $(TOS_CC)) CFLAGS=-O2 \
	    --prefix=$(abspath $(LIBS64)) --disable-shared --enable-static \
	    --disable-doc --disable-tests --disable-languages --disable-nls \
	    > configure.log 2>&1 || { tail -40 configure.log; exit 1; }
	$(MAKE) -C $(BUILD)/libgpg-error/src install \
	    > $(BUILD)/libgpg-error/make.log 2>&1 \
	    || { tail -40 $(BUILD)/libgpg-error/make.log; exit 1; }

GCRYPT_TAR := third_party/libgcrypt-1.12.4.tar.bz2
GCRYPT_A   := $(LIBS64)/lib/libgcrypt.a
$(GCRYPT_A): $(GCRYPT_TAR) $(TOS_CC) $(GPGERR_A)
	rm -rf $(BUILD)/libgcrypt && mkdir -p $(BUILD)/libgcrypt
	tar -xf $< -C $(BUILD)/libgcrypt --strip-components=1
	cd $(BUILD)/libgcrypt && ./configure --host=$(THOST) \
	    --build=x86_64-pc-linux-gnu CC=$(abspath $(TOS_CC)) CFLAGS=-O2 \
	    --prefix=$(abspath $(LIBS64)) --disable-shared --enable-static \
	    --disable-doc --disable-asm --disable-jent-support \
	    --disable-aesni-support --disable-shaext-support \
	    --disable-pclmul-support --disable-sse41-support \
	    --disable-drng-support --disable-avx-support \
	    --disable-avx2-support --disable-avx512-support \
	    --disable-gfni-support --disable-padlock-support \
	    --with-libgpg-error-prefix=$(abspath $(LIBS64)) \
	    GPGRT_CONFIG=$(abspath $(LIBS64))/bin/gpgrt-config \
	    > configure.log 2>&1 || { tail -40 configure.log; exit 1; }
	$(MAKE) -C $(BUILD)/libgcrypt > $(BUILD)/libgcrypt/make.log 2>&1 \
	    || { tail -40 $(BUILD)/libgcrypt/make.log; exit 1; }
	$(MAKE) -C $(BUILD)/libgcrypt/src install \
	    > $(BUILD)/libgcrypt/install.log 2>&1 \
	    || { tail -20 $(BUILD)/libgcrypt/install.log; exit 1; }

# libepoxy: OpenGL and EGL entry points, looked up at run time. ThreadOS
# has no GPU and WebKit draws with Skia on the processor, but WebKit's
# code is written against these names; with no libEGL to find, epoxy
# finds nothing, and the code that would use it is not reached. It
# builds against the Khronos headers (third_party/khronos), put beside
# the libraries.
EPOXY_TAR := third_party/libepoxy-1.5.10.tar.xz
EPOXY_A   := $(LIBS64)/lib/libepoxy.a
KHRONOS_H := $(wildcard third_party/khronos/*/*.h)
$(EPOXY_A): $(EPOXY_TAR) $(MESON_CROSS) $(TOS_CC) $(KHRONOS_H)
	mkdir -p $(LIBS64)/include
	cp -r third_party/khronos/EGL third_party/khronos/KHR $(LIBS64)/include/
	rm -rf $(BUILD)/libepoxy && mkdir -p $(BUILD)/libepoxy
	tar -xf $< -C $(BUILD)/libepoxy --strip-components=1
	cd $(BUILD)/libepoxy && meson setup build \
	    --cross-file $(abspath $(MESON_CROSS)) --wrap-mode=nodownload \
	    --prefix=$(abspath $(LIBS64)) --libdir=lib --buildtype=release \
	    -Ddefault_library=static -Degl=yes -Dglx=no -Dx11=false \
	    -Dtests=false -Ddocs=false \
	    > setup.log 2>&1 || { tail -40 setup.log; exit 1; }
	ninja -C $(BUILD)/libepoxy/build install \
	    > $(BUILD)/libepoxy/ninja.log 2>&1 \
	    || { grep -A20 FAILED $(BUILD)/libepoxy/ninja.log | head -60; exit 1; }

# libxkbcommon: keymaps, which WPE's platform layer speaks in. Its
# parser is made by the host's bison; no X11, Wayland or registry, and
# no keymap data on ThreadOS - the platform gives its own keymap. The
# library alone is built: its tests link ICU when they find it, which is
# C++ and needs a runtime they are not given.
XKB_TAR := third_party/libxkbcommon-1.13.1.tar.gz
XKB_A   := $(LIBS64)/lib/libxkbcommon.a
$(XKB_A): $(XKB_TAR) $(MESON_CROSS) $(TOS_CC)
	rm -rf $(BUILD)/libxkbcommon && mkdir -p $(BUILD)/libxkbcommon
	tar -xf $< -C $(BUILD)/libxkbcommon --strip-components=1
	cd $(BUILD)/libxkbcommon && meson setup build \
	    --cross-file $(abspath $(MESON_CROSS)) --wrap-mode=nodownload \
	    --prefix=$(abspath $(LIBS64)) --libdir=lib --buildtype=release \
	    -Ddefault_library=static -Denable-x11=false -Denable-wayland=false \
	    -Denable-xkbregistry=false -Denable-docs=false -Denable-tools=false \
	    -Denable-bash-completion=false -Dxkb-config-root=/usr/share/X11/xkb \
	    > setup.log 2>&1 || { tail -40 setup.log; exit 1; }
	ninja -C $(BUILD)/libxkbcommon/build libxkbcommon.a \
	    > $(BUILD)/libxkbcommon/ninja.log 2>&1 \
	    || { grep -A20 FAILED $(BUILD)/libxkbcommon/ninja.log | head -60; exit 1; }
	meson install -C $(BUILD)/libxkbcommon/build --no-rebuild --tags devel \
	    > $(BUILD)/libxkbcommon/install.log 2>&1 \
	    || { tail -20 $(BUILD)/libxkbcommon/install.log; exit 1; }

# --- Media for WebKit: FFmpeg and GStreamer ------------------------------
# A page's video and audio are played by GStreamer, as on every WPE
# system, and decoded by FFmpeg through gst-libav. This FFmpeg is the
# media player's archive built again, for WebKit's processes and for
# either machine: the decoders pages' media comes in - H.264, VP8, VP9,
# AAC, Opus, Vorbis, MP3, FLAC - with the parsers in front of them, and
# libavfilter and libswresample, which gst-libav asks for. Its threads
# are musl's; its hand-written SIMD is nasm's on the PC and the
# assembler's NEON on ARM, as FFmpeg's own build has them.
FFMPEG_MEDIA_TAR := third_party/ffmpeg-9.0.tar.xz
FFMPEG_MEDIA_A   := $(LIBS64)/lib/libavcodec.a
$(FFMPEG_MEDIA_A): $(FFMPEG_MEDIA_TAR) $(TOS_CC)
	rm -rf $(BUILD)/ffmpeg-media && mkdir -p $(BUILD)/ffmpeg-media
	tar -xf $< -C $(BUILD)/ffmpeg-media --strip-components=1
	cd $(BUILD)/ffmpeg-media && ./configure \
	    --prefix=$(abspath $(LIBS64)) --enable-cross-compile \
	    --arch=$(TCPU) --target-os=linux --cc=$(abspath $(TOS_CC)) \
	    --ar=$(AR) --ranlib=$(RANLIB) --pkg-config=pkg-config \
	    --pkg-config-flags=--static --disable-programs --disable-doc \
	    --disable-network --disable-everything --disable-autodetect \
	    --disable-debug --disable-gpl --disable-version3 --enable-static \
	    --disable-shared --enable-pthreads --enable-avfilter \
	    --enable-swresample \
	    --enable-decoder=h264,vp8,vp9,aac,mp3,opus,vorbis,flac \
	    --enable-parser=h264,vp8,vp9,aac,opus,vorbis,mpegaudio,flac \
	    --enable-filter=aresample,format,scale,null,anull \
	    > configure.log 2>&1 || { tail -40 configure.log; exit 1; }
	$(MAKE) -C $(BUILD)/ffmpeg-media install \
	    > $(BUILD)/ffmpeg-media/make.log 2>&1 \
	    || { tail -40 $(BUILD)/ffmpeg-media/make.log; exit 1; }

# libopus: Opus's reference library. A page's Opus is decoded by FFmpeg;
# GStreamer's opusparse (gst-plugins-bad, below) reads each packet's
# framing with libopus, and WebKit's Media Source Extensions take Opus
# only with that parser - which is how YouTube sends its sound, in WebM.
OPUS_TAR := third_party/opus/opus-1.5.2.tar.gz
OPUS_A   := $(LIBS64)/lib/libopus.a
$(OPUS_A): $(OPUS_TAR) $(MESON_CROSS) $(TOS_CC)
	rm -rf $(BUILD)/opus && mkdir -p $(BUILD)/opus
	tar -xzf $(OPUS_TAR) -C $(BUILD)/opus --strip-components=1
	cd $(BUILD)/opus && meson setup build \
	    --cross-file $(abspath $(MESON_CROSS)) --wrap-mode=nodownload \
	    --prefix=$(abspath $(LIBS64)) --libdir=lib --buildtype=release \
	    -Ddefault_library=static -Dextra-programs=disabled \
	    -Dtests=disabled -Ddocs=disabled -Dasm=disabled \
	    > setup.log 2>&1 || { tail -30 setup.log; exit 1; }
	ninja -C $(BUILD)/opus/build install > $(BUILD)/opus/ninja.log 2>&1 \
	    || { grep -A20 FAILED $(BUILD)/opus/ninja.log | head -60; exit 1; }

# GStreamer: the core, and of its plugins only what WebKit plays media
# with - everything else is off (auto_features=disabled), each module
# built static by meson into BUILD/libs64. The plugins are static too:
# no registry, nothing loaded at run time; gst_init calls
# gst_init_static_plugins (the core built with gstreamer-static-full),
# which apps/webkit/media/gststatic.c gives, registering each by name.
GST_VER   := 1.28.9
GST_DIR   := third_party/gstreamer
GST_MESON  = meson setup build --cross-file $(abspath $(MESON_CROSS)) \
                 --wrap-mode=nodownload --prefix=$(abspath $(LIBS64)) \
                 --libdir=lib --buildtype=release -Ddefault_library=static \
                 -Dauto_features=disabled -Dtests=disabled -Ddoc=disabled
# (and what all but gst-libav have, and what all but it and good have)
GST_PLAIN := -Dexamples=disabled -Dnls=disabled
GST_NO_GIR := -Dintrospection=disabled
# (a module's build: unpacked afresh, set up with the options after it,
# built and installed; $(1) the archive's name, $(2) the options)
define GST_BUILD
	rm -rf $(BUILD)/$(1) && mkdir -p $(BUILD)/$(1)
	tar -xf $(GST_DIR)/$(1)-$(GST_VER).tar.xz -C $(BUILD)/$(1) \
	    --strip-components=1
	cd $(BUILD)/$(1) && $(GST_MESON) $(2) > setup.log 2>&1 \
	    || { tail -40 setup.log; exit 1; }
	ninja -C $(BUILD)/$(1)/build install > $(BUILD)/$(1)/ninja.log 2>&1 \
	    || { grep -A20 FAILED $(BUILD)/$(1)/ninja.log | head -60; exit 1; }
endef

# The core and its own elements (queue, tee, typefind, filesrc...); no
# debug log - WebKit's categories compile to nothing - and no tools
GST_A := $(LIBS64)/lib/libgstreamer-1.0.a
$(GST_A): $(GST_DIR)/gstreamer-$(GST_VER).tar.xz $(MESON_CROSS) $(TOS_CC) \
          $(GLIB_A) apps/webkit/media/include/asm/hwcap.h
	$(call GST_BUILD,gstreamer,$(GST_PLAIN) $(GST_NO_GIR) \
	    -Dc_args=-I$(abspath apps/webkit/media/include) \
	    -Dgstreamer-static-full=true \
	    -Dgst_debug=false -Dregistry=false -Dptp-helper=disabled \
	    -Dtools=disabled -Dbenchmarks=disabled -Dcheck=disabled \
	    -Dlibunwind=disabled -Dlibdw=disabled -Dcoretracers=disabled \
	    -Dbash-completion=disabled)

# Base: its libraries (audio, video, pbutils, tag, app, fft...) and the
# plugins playback is made of - playbin and the decodebins, typefind,
# conversion and resampling, volume - but no GL: there is none
GST_BASE_A := $(LIBS64)/lib/libgstvideo-1.0.a
$(GST_BASE_A): $(GST_DIR)/gst-plugins-base-$(GST_VER).tar.xz $(GST_A) \
               $(LIBS64_ZLIB)
	$(call GST_BUILD,gst-plugins-base,$(GST_PLAIN) $(GST_NO_GIR) \
	    -Dapp=enabled \
	    -Daudioconvert=enabled -Daudioresample=enabled \
	    -Dplayback=enabled -Dtypefind=enabled -Dvideoconvertscale=enabled \
	    -Dvolume=enabled -Dpbtypes=enabled -Dgl=disabled -Dorc=disabled \
	    -Dtools=disabled)

# Good: the containers pages' media comes in - MP4 (isomp4) and WebM
# (matroska) - the audio parsers, autoaudiosink, and audiofx, whose
# scaletempo keeps the pitch when a video plays faster
GST_GOOD_A := $(LIBS64)/lib/gstreamer-1.0/libgstisomp4.a
$(GST_GOOD_A): $(GST_DIR)/gst-plugins-good-$(GST_VER).tar.xz $(GST_BASE_A)
	$(call GST_BUILD,gst-plugins-good,$(GST_PLAIN) -Disomp4=enabled \
	    -Dmatroska=enabled -Daudioparsers=enabled -Dautodetect=enabled \
	    -Daudiofx=enabled -Dorc=disabled -Dasm=disabled)

# Bad: the video parsers (H.264, VP9, AV1...) in front of the decoders,
# and the codec parsers library they are built on
GST_BAD_A := $(LIBS64)/lib/gstreamer-1.0/libgstvideoparsersbad.a
$(GST_BAD_A): $(GST_DIR)/gst-plugins-bad-$(GST_VER).tar.xz $(GST_BASE_A) \
              $(MESON_CROSS_CXX) $(OPUS_A)
	$(call GST_BUILD,gst-plugins-bad,$(GST_PLAIN) $(GST_NO_GIR) \
	    --cross-file $(abspath $(MESON_CROSS_CXX)) \
	    -Dvideoparsers=enabled -Dopus=enabled \
	    -Dgpl=disabled -Dorc=disabled -Dtools=disabled)

# gst-libav: FFmpeg's decoders as GStreamer elements (avdec_h264...)
GST_LIBAV_A := $(LIBS64)/lib/gstreamer-1.0/libgstlibav.a
$(GST_LIBAV_A): $(GST_DIR)/gst-libav-$(GST_VER).tar.xz $(GST_BASE_A) \
                $(FFMPEG_MEDIA_A)
	$(call GST_BUILD,gst-libav,)

# ThreadOS's own: the plugins' registration (gst_init_static_plugins)
# and the sound sink, as one library, and a pkg-config file naming it,
# every plugin and what they are built on - in the order a static link
# wants them - for WebKit's processes and gsttest. (The core calls the
# registration, and comes after it in a link: the registration is asked
# for by name, -u, before its library is reached.)
GST_PLUGINS := coreelements app audioconvert audioresample playback \
               typefindfunctions videoconvertscale volume pbtypes isomp4 \
               matroska audioparsers autodetect audiofx videoparsersbad \
               opusparse libav
GST_THREADOS_A := $(LIBS64)/lib/libgstthreados.a
GST_THREADOS_PC := gstreamer-threados-1.0
$(GST_THREADOS_A): apps/webkit/media/gststatic.c \
                   apps/webkit/media/gstthreadossink.c user64/usys64.h \
                   $(GST_LIBAV_A) $(GST_GOOD_A) $(GST_BAD_A)
	mkdir -p $(BUILD)/gstthreados
	for f in gststatic gstthreadossink; do \
	    $(TOS_CC) -O2 -Wall -Wextra -DGST_PLUGIN_BUILD_STATIC -Iuser64 \
	        $$($(LIBS64_PC) pkg-config --cflags gstreamer-audio-1.0) \
	        -c apps/webkit/media/$$f.c -o $(BUILD)/gstthreados/$$f.o \
	        || exit 1; done
	rm -f $@ && $(AR) rcs $@ $(BUILD)/gstthreados/gststatic.o \
	    $(BUILD)/gstthreados/gstthreadossink.o
	printf 'prefix=%s\nlibdir=$${prefix}/lib\nplugindir=$${libdir}/gstreamer-1.0\n\nName: %s\nDescription: GStreamer and its plugins, built in, for ThreadOS\nVersion: %s\nRequires: gstreamer-1.0 gstreamer-base-1.0 gstreamer-app-1.0 gstreamer-audio-1.0 gstreamer-video-1.0 gstreamer-pbutils-1.0 gstreamer-tag-1.0 gstreamer-riff-1.0 gstreamer-rtp-1.0 gstreamer-fft-1.0 gstreamer-codecparsers-1.0 opus libavfilter libavformat libavcodec libswresample libswscale libavutil zlib\nLibs: -Wl,-u,gst_init_static_plugins -L$${libdir} -lgstthreados -L$${plugindir} %s\n' \
	    $(abspath $(LIBS64)) $(GST_THREADOS_PC) $(GST_VER) \
	    "$(foreach p,$(GST_PLUGINS),-lgst$(p))" \
	    > $(LIBS64)/lib/pkgconfig/$(GST_THREADOS_PC).pc

gstreamer: $(GST_THREADOS_A)

# gsttest: media decoded and played through GStreamer on ThreadOS, held
# by t_gstreamer to what FFmpeg on the host makes of the same files
$(BUILD)/user64/gsttest.elf: libc/tests/gsttest.c $(GST_THREADOS_A) $(TOS_CC) \
                             $(LIBC64) | $(BUILD)/user64
	$(TOS_CC) -O2 -Wall -Wextra -s -o $@ $< \
	    $$($(LIBS64_PC) pkg-config --cflags --static --libs $(GST_THREADOS_PC))

# ICU itself (built for WebKit above) as pkg-config knows it: icu-uc and
# icu-i18n, pointing at BUILD/icu/root, as ICU's own build would write
ICU_VER := $(patsubst third_party/icu/icu4c-%-sources.tgz,%,$(ICU_TGZ))
ICU_PC  := $(LIBS64)/lib/pkgconfig/icu-uc.pc
$(ICU_PC): $(ICU_STAMP)
	mkdir -p $(dir $@)
	printf 'prefix=%s\nlibdir=$${prefix}/lib\nincludedir=$${prefix}/include\n\nName: icu-uc\nDescription: International Components for Unicode: Common and Data libraries\nVersion: %s\nLibs: -L$${libdir} -licuuc -licudata\nCflags: -I$${includedir}\n' \
	    $(abspath $(ICU_ROOT)) $(ICU_VER) > $@
	printf 'prefix=%s\nlibdir=$${prefix}/lib\nincludedir=$${prefix}/include\n\nName: icu-i18n\nDescription: International Components for Unicode: Stream and I18N support\nVersion: %s\nRequires: icu-uc\nLibs: -L$${libdir} -licui18n\nCflags: -I$${includedir}\n' \
	    $(abspath $(ICU_ROOT)) $(ICU_VER) > $(dir $@)icu-i18n.pc

# HarfBuzz as a library (third_party/harfbuzz, the copy the browser's
# shaping is compiled from): built with the libraries' FreeType, and its
# ICU part on its own, as harfbuzz and harfbuzz-icu, with the
# hb-features.h meson would write and pkg-config files for both
HB64_A     := $(LIBS64)/lib/libharfbuzz.a
HB64_ICU_A := $(LIBS64)/lib/libharfbuzz-icu.a
HB64_VER   := $(shell sed -n 's/^\#define HB_VERSION_STRING "\(.*\)"/\1/p' $(HB_DIR)/src/hb-version.h)
$(HB64_A): $(wildcard $(HB_DIR)/src/*.cc $(HB_DIR)/src/*.hh $(HB_DIR)/src/*.h) \
           $(HB_DIR)/src/hb-features.h.in $(LIBS64_FT_A) $(ICU_PC) $(MUSL_HEADERS)
	mkdir -p $(BUILD)/hb64 $(LIBS64)/include/harfbuzz $(LIBS64)/lib/pkgconfig
	$(CXX) $(CXX64_FLAGS) -w -fno-rtti -DHAVE_FREETYPE=1 \
	    -I$(LIBS64)/include/freetype2 -c $(HB_DIR)/src/harfbuzz.cc \
	    -o $(BUILD)/hb64/harfbuzz.o
	$(CXX) $(CXX64_FLAGS) -w -fno-rtti -DHAVE_ICU=1 -I$(ICU_ROOT)/include \
	    -c $(HB_DIR)/src/hb-icu.cc -o $(BUILD)/hb64/hb-icu.o
	rm -f $(HB64_A) $(HB64_ICU_A)
	$(AR) rcs $(HB64_A) $(BUILD)/hb64/harfbuzz.o
	$(AR) rcs $(HB64_ICU_A) $(BUILD)/hb64/hb-icu.o
	cp $(HB_DIR)/src/hb*.h $(LIBS64)/include/harfbuzz/
	sed -e 's/^#mesondefine \(HB_HAS_FREETYPE\|HB_HAS_ICU\)$$/#define \1 1/' \
	    -e 's/^#mesondefine \(.*\)$$/\/* #undef \1 *\//' \
	    $(HB_DIR)/src/hb-features.h.in > $(LIBS64)/include/harfbuzz/hb-features.h
	printf 'prefix=%s\nlibdir=$${prefix}/lib\nincludedir=$${prefix}/include\n\nName: harfbuzz\nDescription: HarfBuzz text shaping library\nVersion: %s\nRequires.private: freetype2\nLibs: -L$${libdir} -lharfbuzz\nCflags: -I$${includedir}/harfbuzz\n' \
	    $(abspath $(LIBS64)) $(HB64_VER) > $(LIBS64)/lib/pkgconfig/harfbuzz.pc
	printf 'prefix=%s\nlibdir=$${prefix}/lib\nincludedir=$${prefix}/include\n\nName: harfbuzz-icu\nDescription: HarfBuzz text shaping library ICU integration\nVersion: %s\nRequires: harfbuzz icu-uc\nLibs: -L$${libdir} -lharfbuzz-icu\nCflags: -I$${includedir}/harfbuzz\n' \
	    $(abspath $(LIBS64)) $(HB64_VER) > $(LIBS64)/lib/pkgconfig/harfbuzz-icu.pc

libs64: $(WEBP_A) $(TASN1_A) $(GCRYPT_A) $(EPOXY_A) $(XKB_A) $(HB64_A)

# wpedeps: each of them on ThreadOS, held by t_wpedeps to other
# implementations of the same - hashlib and OpenSSL, Pillow's libwebp,
# the definitions of a keymap and of a font, Python's unicodedata.
# HarfBuzz and ICU are C++: the C++ runtime is linked in after them.
WPEDEPS_PC := libgcrypt libtasn1 libwebp libwebpdemux xkbcommon harfbuzz-icu epoxy
$(BUILD)/user64/wpedeps.elf: libc/tests/wpedeps.c $(WEBP_A) $(TASN1_A) \
                             $(GCRYPT_A) $(EPOXY_A) $(XKB_A) $(HB64_A) \
                             $(BUILD)/libc64/cxxrt.o $(TOS_CC) $(LIBC64) | $(BUILD)/user64
	$(TOS_CC) -O2 -Wall -Wextra -c -o $(BUILD)/libc64/wpedeps.o $< \
	    $$($(LIBS64_PC) pkg-config --cflags $(WPEDEPS_PC))
	$(TOS_CC) -s -Wl,--gc-sections -o $@ $(BUILD)/libc64/wpedeps.o \
	    $(BUILD)/libc64/cxxrt.o \
	    $$($(LIBS64_PC) pkg-config --static --libs $(WPEDEPS_PC)) $(LIBSTDCXX)

# bigprog: a program of 9 MiB, most of it known bytes (scripts/mkblob.py)
# in each kind of segment, which says their SHA-256 - larger than the
# 2 MiB a program from the disk used to be held to (kernel/prog.c). Not
# in the initrd: t_bigprog fetches it onto the disk and runs it there.
$(BUILD)/gen/bigprog-ro.bin: scripts/mkblob.py
	@mkdir -p $(BUILD)/gen
	python3 scripts/mkblob.py ro $$((5 << 20)) $@
$(BUILD)/gen/bigprog-rw.bin: scripts/mkblob.py
	@mkdir -p $(BUILD)/gen
	python3 scripts/mkblob.py rw $$((1 << 20)) $@
$(BUILD)/user64/bigprog.elf: libc/tests/bigprog.c $(BUILD)/gen/bigprog-ro.bin \
                             $(BUILD)/gen/bigprog-rw.bin $(OPENSSL_A) \
                             $(TOS_CC) $(LIBC64) | $(BUILD)/user64
	$(TOS_CC) -O2 -Wall -Wextra -Wa,-I$(BUILD)/gen -s -o $@ $< \
	    $$($(LIBS64_PC) pkg-config --cflags --static --libs libcrypto)

# Programs built for the disk rather than the initrd
DISK64_ELFS := $(BUILD)/user64/bigprog.elf

# --- WebKit: JavaScriptCore -------------------------------------------
# WPE WebKit's release (third_party/README.md), built by its own CMake
# through apps/webkit/threados.cmake: the JSCOnly port, the C loop
# interpreter (no JIT: no executable memory to hand out), the system's
# malloc, everything static. -Os because the initrd is memory. The
# shell, jsc, runs scripts; the browser engine is the next stage.
WEBKIT_TAR := third_party/webkit/wpewebkit-2.54.2.tar.xz
WEBKIT_DIR := $(BUILD)/webkit
WEBKIT_SRC := $(WEBKIT_DIR)/wpewebkit-2.54.2
JSC_DIR    := $(WEBKIT_DIR)/jsc

$(WEBKIT_SRC)/CMakeLists.txt: $(WEBKIT_TAR)
	rm -rf $(WEBKIT_SRC) && mkdir -p $(WEBKIT_DIR)
	tar -xf $< -C $(WEBKIT_DIR)
	touch $@

# CMake is run again in the same directory, not a fresh one (as for the
# WPE port below): a change to the toolchain file relinks jsc rather
# than compiling JavaScriptCore again
$(JSC_DIR)/build.ninja: $(WEBKIT_SRC)/CMakeLists.txt apps/webkit/threados.cmake \
                        $(ICU_STAMP)
	mkdir -p $(JSC_DIR)
	cd $(JSC_DIR) && cmake -G Ninja $(abspath $(WEBKIT_SRC)) \
	    -DCMAKE_TOOLCHAIN_FILE=$(abspath apps/webkit/threados.cmake) \
	    -DTHREADOS_MUSL=$(abspath $(MUSL_ROOT)) -DTHREADOS_REPO=$(CURDIR) \
	    -DTHREADOS_CXXRT=$(abspath $(BUILD)/libc64/cxxrt.o) \
	    -DTHREADOS_CC=$(CC) -DTHREADOS_CXX=$(CXX) \
	    "-DTHREADOS_FLAGS=$(TFLAGS)" -DTHREADOS_LD=$(TLD) \
	    -DTHREADOS_CPU=$(TCPU) \
	    -DICU_ROOT=$(abspath $(ICU_ROOT)) -DPORT=JSCOnly \
	    -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS_RELEASE="-Os -DNDEBUG" \
	    -DCMAKE_CXX_FLAGS_RELEASE="-Os -DNDEBUG" \
	    -DENABLE_STATIC_JSC=ON -DENABLE_JIT=OFF -DENABLE_C_LOOP=ON \
	    -DENABLE_DFG_JIT=OFF -DENABLE_FTL_JIT=OFF -DENABLE_WEBASSEMBLY=OFF \
	    -DENABLE_SAMPLING_PROFILER=OFF -DUSE_SYSTEM_MALLOC=ON \
	    -DENABLE_API_TESTS=OFF -DDEVELOPER_MODE=OFF > cmake.log

# ninja knows which of WebKit's files changed; the C library and the
# C++ runtime it cannot see, so the program is linked again every time
# this runs
$(BUILD)/user64/jsc.elf: $(JSC_DIR)/build.ninja $(LIBC64) \
                         $(BUILD)/libc64/cxxrt.o libc/cxx/compat.h \
                         | $(BUILD)/user64
	rm -f $(JSC_DIR)/bin/jsc
	ninja -C $(JSC_DIR) jsc > $(JSC_DIR)/ninja.log || \
	    { tail -40 $(JSC_DIR)/ninja.log; exit 1; }
	$(STRIP) -o $@ $(JSC_DIR)/bin/jsc

USER64_ELFS += $(BUILD)/user64/jsc.elf

# --- WebKit: the WPE port ---------------------------------------------
# WebCore and WebKit from the same release, through the WPE port: the
# browser engine itself, in three programs (apps/webkit/programs.cmake)
# - weft, the window; wpewebproc, which lays out, runs the scripts
# of and paints a page; wpenetproc, which fetches. It paints with Skia
# on the processor into shared memory, which apps/webkit/wpethreados.c
# copies into a window: there is no EGL, no GPU, no Wayland or DRM, no
# accessibility bus, no spelling, and no JIT. Pages' video and audio,
# Media Source Extensions (what YouTube streams through) and Web Audio
# are played by GStreamer (above), its frames painted by Skia like the
# rest of the page, its sound through threadosaudiosink.
#
# What ThreadOS needs changed in WebKit is in apps/webkit/patches, each
# patch saying why. They are applied to the files they touch, taken
# from the archive afresh, so a changed patch costs the rebuild of those
# files and not of the tree (the archive keeps its files' times).
#
# It takes hours on four cores, so it is not part of the default build:
# `make webkit`. The programs are tens of megabytes - too large for the
# initrd, which is memory - and t_webkit fetches them onto the disk.
WPE_DIR      := $(WEBKIT_DIR)/wpe
WPE_PATCHES  := $(sort $(wildcard apps/webkit/patches/*.patch))
WPE_PATCHED  := $(WEBKIT_DIR)/patched.stamp
WPE_PROGRAMS := weft wpewebproc wpenetproc
WPE_ELFS     := $(WPE_PROGRAMS:%=$(BUILD)/user64/%.elf)
WPE_PC       := glib-2.0 gio-2.0 gio-unix-2.0 gobject-2.0 gmodule-2.0 \
                gthread-2.0 libsoup-3.0 libxml-2.0 sqlite3 libpng16 libjpeg \
                harfbuzz harfbuzz-icu epoxy libgcrypt libtasn1 xkbcommon \
                libwebp libwebpdemux freetype2 fontconfig zlib icu-uc \
                icu-i18n gioopenssl openssl $(GST_THREADOS_PC)
WPE_PKGPATH  := $(abspath $(LIBS64))/lib/pkgconfig:$(abspath $(LIBS64))/lib/gio/modules/pkgconfig
WPE_LIBS     := $(GLIB_A) $(LIBSOUP_A) $(GIONET_A) $(LIBXML2_A) \
                $(LIBS64_SQLITE) $(LIBPNG_A) $(LIBJPEG_A) $(FONTCONFIG_A) \
                $(WEBP_A) $(TASN1_A) $(GCRYPT_A) $(EPOXY_A) $(XKB_A) \
                $(HB64_A) $(ICU_PC) $(GST_THREADOS_A)
WPE_OPTIONS  := -DPORT=WPE -DENABLE_JIT=OFF -DENABLE_C_LOOP=ON \
                -DENABLE_DFG_JIT=OFF -DENABLE_FTL_JIT=OFF \
                -DENABLE_WEBASSEMBLY=OFF -DENABLE_SAMPLING_PROFILER=OFF \
                -DUSE_SYSTEM_MALLOC=ON -DENABLE_API_TESTS=OFF \
                -DDEVELOPER_MODE=OFF -DENABLE_DOCUMENTATION=OFF \
                -DENABLE_INTROSPECTION=OFF -DENABLE_JOURNALD_LOG=OFF \
                -DENABLE_WPE_PLATFORM=ON -DENABLE_WPE_PLATFORM_DRM=OFF \
                -DENABLE_WPE_PLATFORM_HEADLESS=ON \
                -DENABLE_WPE_PLATFORM_WAYLAND=OFF -DENABLE_WPE_LEGACY_API=OFF \
                -DENABLE_WPE_QT_API=OFF -DUSE_ATK=OFF -DUSE_FLITE=OFF \
                -DUSE_GBM=OFF -DUSE_LIBBACKTRACE=OFF -DUSE_LIBDRM=OFF \
                -DUSE_LIBHYPHEN=OFF -DUSE_SYSPROF_CAPTURE=OFF -DUSE_AVIF=OFF \
                -DUSE_LCMS=OFF -DUSE_JPEGXL=OFF -DUSE_WOFF2=OFF \
                -DENABLE_SPELLCHECK=OFF -DENABLE_XSLT=OFF \
                -DENABLE_SPEECH_SYNTHESIS=OFF -DENABLE_PDFJS=OFF \
                -DENABLE_WEBDRIVER=OFF -DENABLE_ENCRYPTED_MEDIA=OFF \
                -DENABLE_VIDEO=ON -DENABLE_WEB_AUDIO=ON \
                -DENABLE_MEDIA_STREAM=OFF -DENABLE_MEDIA_RECORDER=OFF \
                -DENABLE_WEB_CODECS=OFF -DENABLE_MEDIA_SESSION=OFF \
                -DENABLE_MEDIA_SOURCE=ON -DENABLE_WEB_RTC=OFF \
                -DENABLE_WEBGL=OFF -DENABLE_GAMEPAD=OFF \
                -DENABLE_BUBBLEWRAP_SANDBOX=OFF -DENABLE_GPU_PROCESS=OFF \
                -DENABLE_WEBXR=OFF -DENABLE_MEMORY_SAMPLER=OFF \
                -DENABLE_RESOURCE_USAGE=OFF -DUSE_SKIA_OPENTYPE_SVG=OFF \
                -DUSE_GSTREAMER=ON -DUSE_GSTREAMER_GL=OFF \
                -DUSE_GSTREAMER_MPEGTS=OFF -DUSE_GSTREAMER_WEBRTC=OFF

$(WPE_PATCHED): $(WEBKIT_SRC)/CMakeLists.txt $(WPE_PATCHES)
	cd $(WEBKIT_DIR) && tar -xf $(abspath $(WEBKIT_TAR)) \
	    $$(sed -n 's|^+++ b/|wpewebkit-2.54.2/|p' $(abspath $(WPE_PATCHES)))
	for p in $(WPE_PATCHES); do \
	    patch -s -d $(WEBKIT_SRC) -p1 < $$p || exit 1; done
	touch $@

# CMake is run again in the same directory, never in a fresh one: the
# objects stay, and ninja rebuilds what the new configuration changed.
# The programs' link line is worked out here from the libraries'
# pkg-config files, so a change in GStreamer's plugins - its pkg-config
# file is written with libgstthreados - configures again.
$(WPE_DIR)/build.ninja: $(WPE_PATCHED) apps/webkit/threados.cmake \
                        apps/webkit/static.cmake apps/webkit/programs.cmake \
                        $(ICU_STAMP) $(BUILD)/libc64/cxxrt.o \
                        $(GST_THREADOS_A) | $(WPE_LIBS)
	mkdir -p $(WPE_DIR)
	cd $(WPE_DIR) && PKG_CONFIG_LIBDIR=$(WPE_PKGPATH) cmake -G Ninja \
	    $(abspath $(WEBKIT_SRC)) -Wno-dev \
	    -DCMAKE_TOOLCHAIN_FILE=$(abspath apps/webkit/threados.cmake) \
	    -DCMAKE_PROJECT_INCLUDE=$(abspath apps/webkit/static.cmake) \
	    -DTHREADOS_PROGRAMS=$(abspath apps/webkit/programs.cmake) \
	    -DTHREADOS_MUSL=$(abspath $(MUSL_ROOT)) -DTHREADOS_REPO=$(CURDIR) \
	    -DTHREADOS_CXXRT=$(abspath $(BUILD)/libc64/cxxrt.o) \
	    -DTHREADOS_CC=$(CC) -DTHREADOS_CXX=$(CXX) \
	    "-DTHREADOS_FLAGS=$(TFLAGS)" -DTHREADOS_LD=$(TLD) \
	    -DTHREADOS_CPU=$(TCPU) \
	    -DTHREADOS_LIBS=$(abspath $(LIBS64)) \
	    "-DTHREADOS_LINK=$$(PKG_CONFIG_LIBDIR=$(WPE_PKGPATH) pkg-config --static --libs $(WPE_PC))" \
	    -DICU_ROOT=$(abspath $(ICU_ROOT)) \
	    -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS_RELEASE="-Os -DNDEBUG" \
	    -DCMAKE_CXX_FLAGS_RELEASE="-Os -DNDEBUG" $(WPE_OPTIONS) > cmake.log \
	    || { tail -30 cmake.log; exit 1; }
	touch $@

# As with jsc, ninja knows which of WebKit's files changed, and the
# programs are linked again every time this runs. ninja runs a compiler
# for every core and two more; some of WebCore's files take 3 GB each,
# so WEBKIT_JOBS=n holds it to n where memory is the limit.
$(WPE_ELFS) &: $(WPE_DIR)/build.ninja $(LIBC64) $(BUILD)/libc64/cxxrt.o \
               libc/cxx/compat.h apps/webkit/weft.c \
               apps/webkit/wpethreados.c apps/webkit/wpethreados.h \
               apps/webkit/wpewebproc.cpp apps/webkit/wpenetproc.cpp \
               user64/usys64.h | $(BUILD)/user64
	rm -f $(WPE_PROGRAMS:%=$(WPE_DIR)/bin/%)
	ninja $(if $(WEBKIT_JOBS),-j$(WEBKIT_JOBS)) -C $(WPE_DIR) \
	    $(WPE_PROGRAMS:%=bin/%) > $(WPE_DIR)/ninja.log || \
	    { grep -A20 FAILED $(WPE_DIR)/ninja.log | head -60; exit 1; }
	for p in $(WPE_PROGRAMS); do \
	    $(STRIP) -o $(BUILD)/user64/$$p.elf $(WPE_DIR)/bin/$$p || exit 1; done

# Weft as an application package (scripts/mktapp.py): the three programs
# and a manifest, a module beside the initrd which the loader brings
# when it finds it (boot/efi/efiboot.c, boot/grub64.cfg) and whose
# programs the kernel runs like the initrd's (kernel/initrd.c). The disc
# and the stick carry it once it has been built. Signed when TAPP_KEY
# names the key that signs releases (make webkit TAPP_KEY=key.pem);
# 'tapp verify weft' says which it is.
WEFT_TAPP := $(BUILD)/weft.tapp
TAPPS = $(wildcard $(BUILD)/*.tapp)

$(WEFT_TAPP): $(WPE_ELFS) scripts/mktapp.py scripts/mkupdate.py
	python3 scripts/mktapp.py $@ --id weft --name Weft --art 5 \
	    --version $(VERSION) --arch $(TARGET) --exec weft \
	    $(if $(TAPP_KEY),--key $(TAPP_KEY)) \
	    $(foreach p,$(WPE_PROGRAMS),$(p)=$(BUILD)/user64/$(p).elf)

webkit: $(WEFT_TAPP)

# Everything WebKit is built on, without WebKit: the libraries a new
# machine's build is checked with before the hours WebKit takes
webkit-libs: $(WPE_LIBS) $(ICU_STAMP) $(BUILD)/libc64/cxxrt.o

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

# Brotli's fixed data, from the RFC's (third_party/brotli-rfc7932): WOFF2
$(BUILD)/gen/ubrotlidata.h: scripts/mkbrotlidata.py $(wildcard third_party/brotli-rfc7932/*)
	@mkdir -p $(BUILD)/gen
	python3 scripts/mkbrotlidata.py third_party/brotli-rfc7932 $@

$(BUILD)/user64/web.elf: apps/browser/web.cpp apps/browser/webjs.h $(LH_OBJS) \
                         $(BUILD)/litehtml/webjs.o $(QJS_OBJS) \
                         $(FT_A) $(HB_O) $(ZLIB_A) user/uwoff.h \
                         user/uwoff2.h user/ubrotli.h $(BUILD)/gen/ubrotlidata.h \
                         $(BUILD)/libc64/cxxrt.o libc/libc64.ld \
                         user/udraw.h user64/usys64.h user64/compat64.h \
                         | $(BUILD)/user64
	$(CXX) $(CXX64_FLAGS) -Wall $(LH_INC) -Iuser -Iuser64 -I$(BUILD)/gen \
	    -I$(FT_TREE)/include -I$(HB_DIR)/src -c $< -o $(BUILD)/litehtml/web.o
	$(call CXX64_LINK,$(BUILD)/litehtml/web.o $(BUILD)/litehtml/webjs.o \
	    $(LH_OBJS) $(QJS_OBJS) $(HB_O) $(FT_A) $(ZLIB_A)) -o $@

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

$(BUILD)/litehtml/host/lhgrid.o: apps/browser/lhgrid.cpp $(LH_HDRS) \
                                | $(BUILD)/litehtml/host
	$(CXX) -std=gnu++17 -O2 -Wall $(LH_INC) -c $< -o $@

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

# Every program is linked again when the C library changed: their own
# objects, and the libraries in libs64, depend on its headers only
# (MUSL_HEADERS), so a fix in libc.a reaches them here
$(filter $(BUILD)/user64/%,$(USER64_ELFS) $(DISK64_ELFS)): $(LIBC64)

# The licences of everything the images hand out, taken from the
# libraries' own archives (scripts/mknotices.py says why). Every
# program linked with them is on the disc, so the texts travel in the
# initrd - an installed system and an update carry them too - and lie
# at the root of the disc, readable on any computer.
LICENSES_TXT := $(BUILD)/LICENSES.txt
$(LICENSES_TXT): scripts/mknotices.py third_party/README.md LICENSE \
                 fonts/LICENSE-DejaVu.txt $(wildcard third_party/* third_party/*/*)
	@mkdir -p $(BUILD)
	python3 scripts/mknotices.py $@

$(INITRD64): $(USER64_ELFS) $(LIBTHREAD64) scripts/mkinitrd.py $(FONTS) \
             $(VERSION_FILE) $(VERSION_CODE_FILE) $(LICENSES_TXT) \
             third_party/iwlwifi/iwlwifi-3160-17.ucode \
             third_party/cacert/cacert.pem
	python3 scripts/mkinitrd.py $@ \
	    libthread.so=$(LIBTHREAD64) LICENSES.txt=$(LICENSES_TXT) \
	    sans.ttf=fonts/sans.ttf bold.ttf=fonts/bold.ttf mono.ttf=fonts/mono.ttf \
	    hello64=$(BUILD)/user64/hello64.elf \
	    nx64=$(BUILD)/user64/nx64.elf \
	    wild64=$(BUILD)/user64/wild64.elf \
	    hostile64=$(BUILD)/user64/hostile64.elf \
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
	    guiapp=$(BUILD)/user64/guiapp64.elf gdeaf=$(BUILD)/user64/gdeaf.elf \
	    seq=$(BUILD)/user64/seq.elf spin=$(BUILD)/user64/spin.elf \
	    mkwall=$(BUILD)/user64/mkwall.elf \
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
	    ggallery=$(BUILD)/user64/ggallery.elf \
	    gcamera=$(BUILD)/user64/gcamera.elf \
	    grecord=$(BUILD)/user64/grecord.elf \
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
	    deep64=$(BUILD)/user64/deep64.elf \
	    dynlib=$(BUILD)/user64/dynlib.elf \
	    cores=$(BUILD)/user64/cores.elf \
	    mutex=$(BUILD)/user64/mutex.elf prodcons=$(BUILD)/user64/prodcons.elf \
	    syscalls=$(BUILD)/user64/syscalls.elf \
	    libctest=$(BUILD)/user64/libctest.elf \
	    pthreadtest=$(BUILD)/user64/pthreadtest.elf \
	    pathtest=$(BUILD)/user64/pathtest.elf \
	    fswalk=$(BUILD)/user64/fswalk.elf \
	    fswrite=$(BUILD)/user64/fswrite.elf \
	    longpath64=$(BUILD)/user64/longpath64.elf \
	    bigmap=$(BUILD)/user64/bigmap.elf \
	    ipctest=$(BUILD)/user64/ipctest.elf \
	    sigtest=$(BUILD)/user64/sigtest.elf \
	    bigram=$(BUILD)/user64/bigram.elf \
	    glibtest=$(BUILD)/user64/glibtest.elf \
	    imgtest=$(BUILD)/user64/imgtest.elf \
	    xmltest=$(BUILD)/user64/xmltest.elf \
	    fctest=$(BUILD)/user64/fctest.elf \
	    souptest=$(BUILD)/user64/souptest.elf \
	    wpedeps=$(BUILD)/user64/wpedeps.elf \
	    sqlite3=$(BUILD)/user64/sqlite3.elf \
	    nettest=$(BUILD)/user64/nettest.elf \
	    randtest=$(BUILD)/user64/randtest.elf \
	    zlibtest=$(BUILD)/user64/zlibtest.elf \
	    curl=$(BUILD)/user64/curl.elf \
	    fonttest=$(BUILD)/user64/fonttest.elf \
	    mbedtest=$(BUILD)/user64/mbedtest.elf \
	    js=$(BUILD)/user64/js.elf \
	    jsc=$(BUILD)/user64/jsc.elf \
	    update=$(BUILD)/user64/update.elf tapp=$(BUILD)/user64/tapp.elf gtapp=$(BUILD)/user64/gtapp.elf \
	    iwl3160.ucode=third_party/iwlwifi/iwlwifi-3160-17.ucode \
	    cacert.pem=third_party/cacert/cacert.pem \
	    version=$(VERSION_FILE) version-code=$(VERSION_CODE_FILE)
	@echo "Done: $@ ($$(stat -c%s $@) B)"

initrd64: $(INITRD64)

KERNEL64_HEADERS := $(wildcard kernel64/*.h)

$(BUILD)/64/%.o: kernel64/%.c $(KERNEL_HEADERS) $(KERNEL64_HEADERS) $(USERCTL_HEADER) | $(BUILD)/64
	$(CC) $(CFLAGS64) -c $< -o $@

# (the shared code reaches this kernel's own headers too: ipc.c its
# ipchost64.h)
$(BUILD)/64/%.o: kernel/%.c $(KERNEL_HEADERS) $(KERNEL64_HEADERS) $(USERCTL_HEADER) | $(BUILD)/64
	$(CC) $(CFLAGS64) -c $< -o $@

# The system's mark (kernel/mark.c): art/threados-mark.svg as coverage,
# cut to the mark and 256 pixels on its longer side, which the kernel
# scales to whatever size it is shown at
LOGO_HDR := $(BUILD)/gen/logo.h
$(LOGO_HDR): art/threados-mark.svg scripts/mklogo.py
	@mkdir -p $(BUILD)/gen
	python3 scripts/mklogo.py $< $@ 256
$(BUILD)/64/mark.o: $(LOGO_HDR)
$(BUILD)/64/mark.o: CFLAGS64 += -I$(BUILD)/gen

# --- ACPICA -------------------------------------------------------------
# The interpreter of the firmware's AML (Linux's and the BSDs'): the
# release archive, unpacked into the build with the one line
# kernel/acpica/patches/acenv.h.patch adds - the name of our host
# header, kernel/acpica/acthreados.h - and compiled with the kernel's
# flags. Its own string functions are used under names of their own
# (memcpy -> acpi_memcpy, ...), so that they do not meet the kernel's.
# kernel/acpios.c is what it asks of the system, kernel/power.c what we
# ask of it.
ACPICA_TAR   := third_party/acpica-unix-20260408.tar.gz
ACPICA_DIR   := $(BUILD)/acpica
ACPICA_STAMP := $(ACPICA_DIR)/.unpacked
ACPICA_LIB   := $(ACPICA_DIR)/libacpica.a
ACPICA_PARTS := dispatcher events executer hardware namespace parser \
                resources tables utilities
# rsdump.c is the debugger's: it prints resources with tables built only
# into ACPICA's own tools
ACPICA_CLIB  := fprintf memcmp memcpy memmove memset printf snprintf sprintf \
                strcat strchr strcmp strcpy strlen strncat strncmp strncpy \
                strpbrk strstr strtok strtoul tolower toupper vfprintf \
                vprintf vsnprintf
# Our side (acpios.c, power.c) sees ACPICA's headers as a system's - their
# debug stubs leave parameters unused - and keeps the kernel's names
ACPICA_OWN   := $(CFLAGS64) -D__THREADOS__ \
                -isystem $(abspath $(ACPICA_DIR))/source/include \
                -I$(abspath kernel/acpica)
ACPICA_FLAGS := $(ACPICA_OWN) $(foreach f,$(ACPICA_CLIB),-D$(f)=acpi_$(f))

$(ACPICA_STAMP): $(ACPICA_TAR) kernel/acpica/patches/acenv.h.patch Makefile
	rm -rf $(ACPICA_DIR) && mkdir -p $(ACPICA_DIR)
	tar -xzf $< -C $(ACPICA_DIR) --strip-components=1
	sed 's/\r$$//' kernel/acpica/patches/acenv.h.patch | \
	    patch -s -d $(ACPICA_DIR) -p1
	touch $@

$(ACPICA_LIB): $(ACPICA_STAMP) kernel/acpica/acthreados.h
	mkdir -p $(ACPICA_DIR)/obj
	ls $(foreach p,$(ACPICA_PARTS),$(ACPICA_DIR)/source/components/$(p)/*.c) | \
	    grep -v -e '/rsdump\.c$$' | \
	    xargs -P 8 -I{} sh -c '$(CC) $(ACPICA_FLAGS) -w -c {} \
	        -o $(ACPICA_DIR)/obj/$$(basename {} .c).o'
	$(AR) rcs $@ $(ACPICA_DIR)/obj/*.o

$(BUILD)/64/acpios.o $(BUILD)/64/power.o: \
$(BUILD)/64/%.o: kernel/%.c $(ACPICA_STAMP) kernel/acpica/acthreados.h \
                 $(KERNEL_HEADERS) | $(BUILD)/64
	$(CC) $(ACPICA_OWN) -c $< -o $@

$(BUILD)/64/wpa.o: user/uwpa.h user/uaes.h user/usha1.h

$(KERNEL64): $(OBJS64) $(ACPICA_LIB) kernel64/linker64.ld
	$(CC) $(LDFLAGS64) $(OBJS64) $(ACPICA_LIB) -o $@

kernel64: $(KERNEL64) $(INITRD64) $(DISK64_ELFS)
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

$(ISO64): $(KERNEL64) $(INITRD64) boot/grub64.cfg $(ISO64_LOADER) \
          $(TAPPS) $(LICENSES_TXT)
	@command -v grub-mkrescue >/dev/null || { \
	    echo "ERROR: no grub-mkrescue."; \
	    echo "  Debian/Ubuntu: apt install grub-pc-bin grub-efi-amd64-bin \
grub-common xorriso mtools"; \
	    exit 1; }
	mkdir -p $(BUILD)/isodir64/boot/grub
	cp $(KERNEL64) $(BUILD)/isodir64/boot/threados64.bin
	cp $(INITRD64) $(BUILD)/isodir64/boot/initrd64.img
	cp boot/grub64.cfg $(BUILD)/isodir64/boot/grub/grub.cfg
	cp $(LICENSES_TXT) $(BUILD)/isodir64/LICENSES.txt
	$(if $(ISO64_LOADER),cp $(ISO64_LOADER) $(BUILD)/isodir64/boot/threadboot.efi)
	rm -rf $(BUILD)/isodir64/boot/apps
	$(if $(TAPPS),mkdir -p $(BUILD)/isodir64/boot/apps && cp $(TAPPS) $(BUILD)/isodir64/boot/apps/)
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

# 'make run' is the system started off the disc the way a real machine
# starts it: GRUB, the
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

# The automatic test: a boot in QEMU with no window, and the VGA screen
# read back. The test makes a fresh disk image itself and checks that
# what was written survives between boots. The groups run in parallel,
# because each has a QEMU machine of its own and spends most of its
# time waiting anyway: for the boot, for an answer, for the screen to
# be redrawn.
#
#   make test            every group
#   make test T=k64core  selected groups (T=k64core,uefi)
#   make test J=1        one after another, for debugging
# The disc too, where GRUB can make it: t_isoinstall installs from the
# one 'make iso64' builds, not from a copy of its own
test: $(KERNEL64) $(INITRD64) browser \
      $(if $(shell command -v grub-mkrescue 2>/dev/null),$(ISO64))
	python3 -u scripts/run_tests.py $(if $(T),-t$(T)) $(if $(J),-j$(J)) \
	    $(KERNEL64) $(INITRD64)

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
EFI_CC  := clang -target x86_64-unknown-windows $(ARCH64) -ffreestanding \
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

# --- The ARM64 kernel ------------------------------------------------
#
# A third kernel, for QEMU's virt machine: the first stage of the port a
# phone or a tablet will run. For now it is its own small directories
# (bootarm64/, kernelarm64/) plus the shared files that already build
# for it; the rest of kernel/ follows as it stops assuming x86. clang
# builds it because one clang targets every architecture, while the gcc
# on a PC targets only its own - the reason it builds threadboot too.
KERNELARM := $(BUILD)/threados-arm64.elf
CFLAGSARM := --target=aarch64-none-elf -std=gnu11 -ffreestanding \
             -fno-stack-protector -fno-pic -mgeneral-regs-only \
             -mstrict-align -Wall -Wextra -Wshadow -O2 -g
# What of kernel/ the phone's screens are drawn with - the PC's own
# graphics, fonts and mark - and the initrd that brings the fonts
SHAREDARM := gfx ttf uitext fonts mark aurora initrd locale bootsplash ipc \
             net tcp dhcp random
OBJSARM   := $(BUILD)/arm64/boot.o $(BUILD)/arm64/kmain.o \
             $(BUILD)/arm64/uart.o $(BUILD)/arm64/fdt.o \
             $(BUILD)/arm64/string.o $(BUILD)/arm64/vectors.o \
             $(BUILD)/arm64/exceptions.o $(BUILD)/arm64/mmu.o \
             $(BUILD)/arm64/gic.o $(BUILD)/arm64/timer.o \
             $(BUILD)/arm64/pmm.o $(BUILD)/arm64/heap.o \
             $(BUILD)/arm64/thread.o $(BUILD)/arm64/switch.o \
             $(BUILD)/arm64/smp.o $(BUILD)/arm64/fwcfg.o \
             $(BUILD)/arm64/video.o $(BUILD)/arm64/rtc.o \
             $(BUILD)/arm64/virtio.o $(BUILD)/arm64/touch.o \
             $(BUILD)/arm64/mobile.o $(BUILD)/arm64/proc.o \
             $(BUILD)/arm64/netglue.o $(BUILD)/arm64/vnet.o \
             $(BUILD)/arm64/winmob.o $(BUILD)/arm64/vrng.o \
             $(BUILD)/arm64/mobapps.o $(BUILD)/arm64/mobsound.o $(BUILD)/arm64/vsnd.o \
             $(patsubst %,$(BUILD)/arm64/%.o,$(SHAREDARM))

$(BUILD)/arm64:
	mkdir -p $@

$(BUILD)/arm64/%.o: bootarm64/%.S | $(BUILD)/arm64
	clang $(CFLAGSARM) -c $< -o $@

$(BUILD)/arm64/%.o: kernelarm64/%.c $(wildcard kernelarm64/*.h) | $(BUILD)/arm64
	clang $(CFLAGSARM) -c $< -o $@

# (the shared code reaches this kernel's own headers too - ipc.c its
# ipchost.h, which reads struct thread)
$(BUILD)/arm64/%.o: kernel/%.c $(KERNEL_HEADERS) $(wildcard kernelarm64/*.h) | $(BUILD)/arm64
	clang $(CFLAGSARM) -c $< -o $@

$(BUILD)/arm64/mark.o: $(LOGO_HDR)
$(BUILD)/arm64/mark.o: CFLAGSARM += -I$(BUILD)/gen

$(KERNELARM): $(OBJSARM) kernelarm64/linker.ld
	ld.lld -nostdlib -static -T kernelarm64/linker.ld $(OBJSARM) -o $@

# Programs for the ARM64 kernel (userarm64/): freestanding, at EL0, on
# the system calls of kernelarm64/proc.c. Linked at 256 GiB, where the
# kernel maps nothing (userarm64/user.ld). The FP/SIMD registers are
# theirs, so not -mgeneral-regs-only; and no fused multiply-add, so a
# sum comes out bit for bit as the tests' Python computes it.
USERARM := hello fpsum fault badcall spin
USERARM_ELFS := $(patsubst %,$(BUILD)/arm64/user/%.elf,$(USERARM))
CFLAGSARMUSER := --target=aarch64-none-elf -std=gnu11 -ffreestanding \
                 -fno-stack-protector -fno-pic -fno-builtin \
                 -ffp-contract=off -Wall -Wextra -O2

$(BUILD)/arm64/user:
	mkdir -p $@

$(BUILD)/arm64/user/%.elf: userarm64/%.c userarm64/usys.h userarm64/user.ld \
                           | $(BUILD)/arm64/user
	clang $(CFLAGSARMUSER) -c $< -o $(@:.elf=.o)
	ld.lld -nostdlib -static -T userarm64/user.ld $(@:.elf=.o) -o $@

# musl for the ARM64 kernel: the same release (third_party/musl, whose
# aarch64 directories are kept for this) and the same overlay as the
# PC's, built for aarch64. The overlay's layer (src/threados) is one C
# file for both machines - musl names its Linux calls (SYS_openat) and
# the layer answers by name - and only the handful of files musl writes
# in assembly have an aarch64 copy beside the x86_64 one. The compiler
# is the cross gcc (g++-aarch64-linux-gnu), for the libgcc it brings:
# on AArch64 a long double is 128 bits, done in software, and musl's
# printf and strtod work in it. No fused multiply-add: what musl's
# arithmetic gives has to be what it gives on the PC, where the tests
# compare it with glibc.
ARMCC         := aarch64-linux-gnu-gcc
MUSLARM_TREE  := $(BUILD)/arm64/musl/tree
MUSLARM_ROOT  := $(BUILD)/arm64/musl/root
MUSLARM_STAMP := $(MUSLARM_TREE)/installed.stamp
LIBCARM       := $(MUSLARM_ROOT)/lib/libc.a
MUSLARM_CFLAGS := -O2 -march=armv8-a -mno-outline-atomics \
                  -fno-stack-protector -fno-pic \
                  -fno-asynchronous-unwind-tables -ffp-contract=off

$(MUSLARM_TREE)/config.mak: Makefile third_party/musl/VERSION \
                            third_party/musl/tools/mkalltypes.sed \
                            $(wildcard third_party/musl/tools/*.sh) \
                            third_party/musl/arch/aarch64/bits/alltypes.h.in \
                            third_party/musl/include/alltypes.h.in \
                            $(filter %.h,$(MUSL_OVERLAY))
	rm -rf $(MUSLARM_TREE) && mkdir -p $(MUSLARM_TREE)
	cp -r third_party/musl/. $(MUSLARM_TREE)/
	sed -i 's/\r$$//' $(MUSLARM_TREE)/configure \
	    $(MUSLARM_TREE)/VERSION \
	    $(MUSLARM_TREE)/tools/mkalltypes.sed \
	    $(MUSLARM_TREE)/tools/*.sh \
	    $(MUSLARM_TREE)/arch/aarch64/bits/alltypes.h.in \
	    $(MUSLARM_TREE)/include/alltypes.h.in
	cd $(MUSLARM_TREE) && CC=$(ARMCC) AR=aarch64-linux-gnu-ar \
	    RANLIB=aarch64-linux-gnu-ranlib CFLAGS="$(MUSLARM_CFLAGS)" \
	    ./configure --target=aarch64 --prefix=/ --disable-shared >/dev/null

$(MUSLARM_STAMP): $(MUSLARM_TREE)/config.mak $(MUSL_OVERLAY)
	cp -r libc/overlay/. $(MUSLARM_TREE)/
	$(MAKE) -s -C $(MUSLARM_TREE) lib/libc.a lib/crt1.o lib/crti.o lib/crtn.o
	$(MAKE) -s -C $(MUSLARM_TREE) install-libs install-headers \
	    DESTDIR=$(abspath $(MUSLARM_ROOT)) INSTALL="install -C"
	touch $@

$(LIBCARM): $(MUSLARM_STAMP)
	@test -f $@

libcarm64: $(LIBCARM)

# Programs against it: libc/libcarm64.ld is libc/libc64.ld at the ARM64
# kernel's program address. The test programs of libc/tests are the
# PC's, unchanged - the point of them is that they print the same.
MUSLARM_PROGS := libctest pthreadtest ipctest nettest wintest randtest \
                 sigtest notetest sndtest
MUSLARM_ELFS  := $(patsubst %,$(BUILD)/arm64/libc/%.elf,$(MUSLARM_PROGS))
LIBCARM_CFLAGS := -std=gnu11 -O2 -march=armv8-a -mno-outline-atomics \
                  -fno-stack-protector -fno-pic \
                  -fno-asynchronous-unwind-tables -ffp-contract=off \
                  -nostdinc -isystem $(MUSLARM_ROOT)/include
LIBCARM_LINK = $(ARMCC) -static -no-pie -nostdlib -s -Wl,--build-id=none \
               -Wl,-z,max-page-size=0x1000 -T libc/libcarm64.ld \
               $(MUSLARM_ROOT)/lib/crt1.o $(MUSLARM_ROOT)/lib/crti.o $(1) \
               $(LIBCARM) $$($(ARMCC) -print-libgcc-file-name) \
               $(MUSLARM_ROOT)/lib/crtn.o

$(BUILD)/arm64/libc:
	mkdir -p $@

# ThreadOS's own calls, the PC's library of them (its AArch64 branch)
USYSARM_ELFS := $(patsubst %,$(BUILD)/arm64/libc/%.elf,wintest notetest sndtest)
$(USYSARM_ELFS): LIBCARM_CFLAGS += -Iuser64
$(USYSARM_ELFS): user64/usys64.h

$(BUILD)/arm64/libc/%.elf: libc/tests/%.c $(MUSLARM_STAMP) libc/libcarm64.ld \
                           | $(BUILD)/arm64/libc
	$(ARMCC) $(LIBCARM_CFLAGS) -isystem $$($(ARMCC) -print-file-name=include) \
	    -c $< -o $(@:.elf=.o)
	$(call LIBCARM_LINK,$(@:.elf=.o)) -o $@

# The phone's initrd: the fonts and the programs. QEMU hands it over
# through fw_cfg (-fw_cfg name=opt/threados/initrd,file=...).
INITRDARM := $(BUILD)/initrd-arm64.img
$(INITRDARM): scripts/mkinitrd.py fonts/sans.ttf fonts/bold.ttf fonts/mono.ttf \
              third_party/cacert/cacert.pem $(USERARM_ELFS) $(MUSLARM_ELFS)
	python3 scripts/mkinitrd.py $@ sans.ttf=fonts/sans.ttf \
	    bold.ttf=fonts/bold.ttf mono.ttf=fonts/mono.ttf \
	    cacert.pem=third_party/cacert/cacert.pem \
	    $(foreach p,$(USERARM),$(p)=$(BUILD)/arm64/user/$(p).elf) \
	    $(foreach p,$(MUSLARM_PROGS),$(p)=$(BUILD)/arm64/libc/$(p).elf)

kernelarm64: $(KERNELARM) $(INITRDARM)
	@echo "Done: $(KERNELARM) (AArch64, QEMU virt), $(INITRDARM)"

# The ARM64 kernel's programs that stand on the libraries - and Weft -
# are the PC's rules built for aarch64 (TARGET, above), into a build
# directory of their own beside this one, where the ARM64 tests look
# for them (scripts/tests/t_arm64libs.py)
ARM64_USER := $(BUILD)/aarch64

arm64-libs:
	$(MAKE) TARGET=aarch64 BUILD=$(ARM64_USER) webkit-libs \
	    $(ARM64_USER)/user64/glibtest.elf $(ARM64_USER)/user64/wpedeps.elf \
	    $(ARM64_USER)/user64/souptest.elf

weft-arm64:
	$(MAKE) TARGET=aarch64 BUILD=$(ARM64_USER) webkit

# The serial port alone; Ctrl-A X leaves QEMU
run-arm64: $(KERNELARM)
	qemu-system-aarch64 -machine virt -cpu cortex-a72 -m 512 -smp 2 \
	    -nographic -kernel $(KERNELARM)

# ThreadOS mobile: the same kernel as a phone - a screen (ramfb), a
# touchscreen (the mouse is the finger), a speaker and a microphone
# (virtio-sound, through PHONE_AUDIO: pa, alsa or sdl on a desktop) and
# the initrd; the serial console stays on the terminal
PHONE_AUDIO ?= none
PHONE_DEVICES = -device ramfb -device virtio-tablet-device \
                -device virtio-rng-device \
                -audiodev $(PHONE_AUDIO),id=snd0 \
                -device virtio-sound-device,audiodev=snd0 \
                -global virtio-mmio.force-legacy=false \
                -fw_cfg name=opt/threados/initrd,file=$(INITRDARM)
# The Moto G34 5G's panel (720 x 1600), and of its four gigabytes the
# three the kernel takes below 4 GiB
run-phone: $(KERNELARM) $(INITRDARM)
	qemu-system-aarch64 -machine virt -cpu cortex-a72 -m 3072 -smp 4 \
	    $(PHONE_DEVICES) -serial stdio -kernel $(KERNELARM) \
	    -append "panel=720x1600"

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

# With the application packages that have been built (make webkit)
$(STICK): $(EFI_OUT) $(KERNEL64) $(INITRD64) scripts/mkstick.py \
          $(TAPPS)
	@command -v mformat >/dev/null || { \
	    echo "ERROR: no mtools (apt install mtools)"; exit 1; }
	python3 scripts/mkstick.py $@ $(EFI_OUT) $(KERNEL64) $(INITRD64) \
	    $(foreach t,$(TAPPS),--tapp $(t))

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
