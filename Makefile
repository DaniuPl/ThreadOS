# ProtonOS - budowanie jądra
#
# Cele:
#   make        - buduje protonos.bin (jądro Multiboot)
#   make iso    - buduje bootowalne ISO (wymaga grub-mkrescue + xorriso)
#   make run    - uruchamia jądro w QEMU (wymaga qemu-system-i386)
#   make fuzz   - sprawdza parsery na uszkodzonych danych (sanitizery)
#   make clean  - usuwa pliki wynikowe

CC      := gcc
AS      := gcc
LD      := gcc

CFLAGS  := -m32 -std=gnu11 -ffreestanding -fno-stack-protector -fno-pic \
           -Wall -Wextra -O2 -g
ASFLAGS := -m32
# Uwaga: docelowo warto linkować z -lgcc (wymaga gcc-multilib z 32-bit libgcc),
# np. gdy pojawi się 64-bitowe dzielenie. Na razie jądro się bez niej obywa.
# --no-warn-rwx-segments: sekcja .user swiadomie laczy kod i dane (RWX)
LDFLAGS := -m32 -ffreestanding -nostdlib -static -no-pie -T linker.ld \
           -Wl,--build-id=none -Wl,--no-warn-rwx-segments

BUILD   := build
KERNEL  := $(BUILD)/protonos.bin
ISO     := $(BUILD)/protonos.iso
INITRD  := $(BUILD)/initrd.img
DISK    := $(BUILD)/disk.img

# Programy uzytkownika (ELF-y ladowane z initrd)
USER_CFLAGS := -m32 -std=gnu11 -ffreestanding -fno-stack-protector \
               -fno-pic -no-pie -static -nostdlib -O2 -Wall -Wextra \
               -Wl,--build-id=none -Wl,--no-warn-rwx-segments
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
             $(BUILD)/user/guiapp.elf $(BUILD)/user/gclock.elf \
             $(BUILD)/user/mmaptest.elf $(BUILD)/user/gfiles.elf \
             $(BUILD)/user/shmtest.elf $(BUILD)/user/shmread.elf \
             $(BUILD)/user/gmon.elf $(BUILD)/user/threads.elf $(BUILD)/user/prio.elf $(BUILD)/user/gterm.elf \
             $(BUILD)/user/udpecho.elf $(BUILD)/user/udptest.elf \
             $(BUILD)/user/nslookup.elf \
             $(BUILD)/user/httpget.elf $(BUILD)/user/tcpserv.elf \
             $(BUILD)/user/gweb.elf \
             $(BUILD)/user/cryptotest.elf \
             $(BUILD)/user/certtest.elf \
             $(BUILD)/user/forktest.elf

OBJS := $(BUILD)/boot.o \
        $(BUILD)/isr_stubs.o \
        $(BUILD)/kmain.o \
        $(BUILD)/vga.o \
        $(BUILD)/serial.o \
        $(BUILD)/printk.o \
        $(BUILD)/string.o \
        $(BUILD)/gdt.o \
        $(BUILD)/idt.o \
        $(BUILD)/isr.o \
        $(BUILD)/pic.o \
        $(BUILD)/pit.o \
        $(BUILD)/keyboard.o \
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
        $(BUILD)/ata.o \
        $(BUILD)/fs.o \
        $(BUILD)/kfile.o \
        $(BUILD)/pipe.o \
        $(BUILD)/rtc.o \
        $(BUILD)/mouse.o \
        $(BUILD)/video.o \
        $(BUILD)/desktop.o \
        $(BUILD)/pci.o \
        $(BUILD)/shm.o \
        $(BUILD)/rtl8139.o \
        $(BUILD)/net.o \
        $(BUILD)/dhcp.o \
        $(BUILD)/tcp.o

.PHONY: all iso run run-iso test fuzz clean

all: $(KERNEL) $(INITRD)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/boot.o: boot/boot.S | $(BUILD)
	$(AS) $(ASFLAGS) -c $< -o $@

$(BUILD)/isr_stubs.o: kernel/isr.S | $(BUILD)
	$(AS) $(ASFLAGS) -c $< -o $@

$(BUILD)/switch.o: kernel/switch.S | $(BUILD)
	$(AS) $(ASFLAGS) -c $< -o $@

$(BUILD)/%.o: kernel/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(KERNEL): $(OBJS) linker.ld
	$(LD) $(LDFLAGS) $(OBJS) -o $@
	@grub-file --is-x86-multiboot $@ 2>/dev/null \
	    && echo "OK: $@ jest poprawnym obrazem Multiboot" \
	    || echo "INFO: grub-file niedostepny - pomijam weryfikacje Multiboot"

$(BUILD)/user: | $(BUILD)
	mkdir -p $(BUILD)/user

# Naglowki userspace sa wciagane przez wiele programow (biblioteczka
# syscalli, siec, kryptografia), wiec zmiana ktoregokolwiek ma
# przebudowac wszystko - stad wildcard zamiast recznej listy.
USER_HEADERS := $(wildcard user/*.h)

$(BUILD)/user/%.elf: user/%.c $(USER_HEADERS) user/user.ld | $(BUILD)/user
	$(CC) $(USER_CFLAGS) -T user/user.ld $< -o $@

$(INITRD): $(USER_ELFS) scripts/mkinitrd.py
	python3 scripts/mkinitrd.py $@ \
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
	    guiapp=$(BUILD)/user/guiapp.elf gclock=$(BUILD)/user/gclock.elf \
	    mmaptest=$(BUILD)/user/mmaptest.elf gfiles=$(BUILD)/user/gfiles.elf \
	    shmtest=$(BUILD)/user/shmtest.elf shmread=$(BUILD)/user/shmread.elf \
	    gmon=$(BUILD)/user/gmon.elf threads=$(BUILD)/user/threads.elf \
	    prio=$(BUILD)/user/prio.elf gterm=$(BUILD)/user/gterm.elf \
	    udpecho=$(BUILD)/user/udpecho.elf udptest=$(BUILD)/user/udptest.elf \
	    nslookup=$(BUILD)/user/nslookup.elf \
	    httpget=$(BUILD)/user/httpget.elf tcpserv=$(BUILD)/user/tcpserv.elf \
	    gweb=$(BUILD)/user/gweb.elf \
	    cryptotest=$(BUILD)/user/cryptotest.elf \
	    certtest=$(BUILD)/user/certtest.elf \
	    forktest=$(BUILD)/user/forktest.elf

# Pusty obraz dysku 16 MiB pod system plikow PFS (jadro go sformatuje).
# PFS3 przydziela bloki z bitmapy, wiec miejsce idzie tam, gdzie trzeba.
$(DISK): | $(BUILD)
	dd if=/dev/zero of=$@ bs=1M count=16 status=none

iso: $(KERNEL) $(INITRD) grub.cfg
	mkdir -p $(BUILD)/isodir/boot/grub
	cp $(KERNEL) $(BUILD)/isodir/boot/protonos.bin
	cp $(INITRD) $(BUILD)/isodir/boot/initrd.img
	cp grub.cfg $(BUILD)/isodir/boot/grub/grub.cfg
	grub-mkrescue -o $(ISO) $(BUILD)/isodir

# QEMU potrafi załadować jądro Multiboot bezpośrednio - bez ISO i GRUB-a.
# Dysk (-hda) trafia na pierwsza magistrale ATA jako master.
# Siec: karta RTL8139 w trybie uzytkownika QEMU (adres z DHCP: 10.0.2.15,
# brama 10.0.2.2). hostfwd wystawia serwer TCP z ProtonOS-a na hosta:
# po 'run tcpserv' mozna z terminala zrobic 'nc localhost 5555'.
NETOPTS := -netdev user,id=n0,hostfwd=tcp::5555-:5555 \
           -device rtl8139,netdev=n0

run: $(KERNEL) $(INITRD) $(DISK)
	qemu-system-i386 -kernel $(KERNEL) -initrd $(INITRD) -hda $(DISK) $(NETOPTS)

run-iso: iso
	qemu-system-i386 -cdrom $(ISO)

# Automatyczny test: boot w QEMU bez okna + weryfikacja ekranu VGA.
# Test sam tworzy swiezy obraz dysku i sprawdza persystencje miedzy boot-ami.
test: $(KERNEL) $(INITRD)
	python3 -u scripts/vga_smoke_test.py $(KERNEL) $(INITRD)

# Parsery dostaja dane prosto z sieci - ten cel przepuszcza je przez
# sanitizery na hoscie, karmiac uszkodzonymi wejsciami (certyfikaty,
# HTML, HTTP oraz ramki trafiajace do net.c i tcp.c w jadrze).
fuzz:
	python3 scripts/fuzz_parsers.py 500

clean:
	rm -rf $(BUILD)
