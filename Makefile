# ProtonOS - budowanie jądra
#
# Cele:
#   make        - buduje protonos.bin (jądro Multiboot)
#   make iso    - buduje bootowalne ISO (wymaga grub-mkrescue + xorriso)
#   make run    - uruchamia jądro w QEMU (wymaga qemu-system-i386)
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
             $(BUILD)/user/sh.elf $(BUILD)/user/echo.elf \
             $(BUILD)/user/memtest.elf $(BUILD)/user/fileio.elf \
             $(BUILD)/user/cat.elf $(BUILD)/user/wc.elf \
             $(BUILD)/user/seq.elf $(BUILD)/user/grep.elf \
             $(BUILD)/user/head.elf $(BUILD)/user/ls.elf \
             $(BUILD)/user/rm.elf $(BUILD)/user/date.elf \
             $(BUILD)/user/cp.elf $(BUILD)/user/mv.elf \
             $(BUILD)/user/mkdir.elf

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
        $(BUILD)/rtc.o

.PHONY: all iso run run-iso test clean

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

$(BUILD)/user/%.elf: user/%.c user/usys.h user/user.ld | $(BUILD)/user
	$(CC) $(USER_CFLAGS) -T user/user.ld $< -o $@

$(INITRD): $(USER_ELFS) scripts/mkinitrd.py
	python3 scripts/mkinitrd.py $@ \
	    hello=$(BUILD)/user/hello.elf evil=$(BUILD)/user/evil.elf \
	    sh=$(BUILD)/user/sh.elf echo=$(BUILD)/user/echo.elf \
	    memtest=$(BUILD)/user/memtest.elf fileio=$(BUILD)/user/fileio.elf \
	    cat=$(BUILD)/user/cat.elf wc=$(BUILD)/user/wc.elf \
	    seq=$(BUILD)/user/seq.elf grep=$(BUILD)/user/grep.elf \
	    head=$(BUILD)/user/head.elf ls=$(BUILD)/user/ls.elf \
	    rm=$(BUILD)/user/rm.elf date=$(BUILD)/user/date.elf \
	    cp=$(BUILD)/user/cp.elf mv=$(BUILD)/user/mv.elf \
	    mkdir=$(BUILD)/user/mkdir.elf

# Pusty obraz dysku 1 MiB pod system plikow PFS (jadro go sformatuje)
$(DISK): | $(BUILD)
	dd if=/dev/zero of=$@ bs=1M count=1 status=none

iso: $(KERNEL) $(INITRD) grub.cfg
	mkdir -p $(BUILD)/isodir/boot/grub
	cp $(KERNEL) $(BUILD)/isodir/boot/protonos.bin
	cp $(INITRD) $(BUILD)/isodir/boot/initrd.img
	cp grub.cfg $(BUILD)/isodir/boot/grub/grub.cfg
	grub-mkrescue -o $(ISO) $(BUILD)/isodir

# QEMU potrafi załadować jądro Multiboot bezpośrednio - bez ISO i GRUB-a.
# Dysk (-hda) trafia na pierwsza magistrale ATA jako master.
run: $(KERNEL) $(INITRD) $(DISK)
	qemu-system-i386 -kernel $(KERNEL) -initrd $(INITRD) -hda $(DISK)

run-iso: iso
	qemu-system-i386 -cdrom $(ISO)

# Automatyczny test: boot w QEMU bez okna + weryfikacja ekranu VGA.
# Test sam tworzy swiezy obraz dysku i sprawdza persystencje miedzy boot-ami.
test: $(KERNEL) $(INITRD)
	python3 scripts/vga_smoke_test.py $(KERNEL) $(INITRD)

clean:
	rm -rf $(BUILD)
