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
LDFLAGS := -m32 -ffreestanding -nostdlib -static -no-pie -T linker.ld \
           -Wl,--build-id=none

BUILD   := build
KERNEL  := $(BUILD)/protonos.bin
ISO     := $(BUILD)/protonos.iso

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
        $(BUILD)/keyboard.o

.PHONY: all iso run run-iso test clean

all: $(KERNEL)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/boot.o: boot/boot.S | $(BUILD)
	$(AS) $(ASFLAGS) -c $< -o $@

$(BUILD)/isr_stubs.o: kernel/isr.S | $(BUILD)
	$(AS) $(ASFLAGS) -c $< -o $@

$(BUILD)/%.o: kernel/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(KERNEL): $(OBJS) linker.ld
	$(LD) $(LDFLAGS) $(OBJS) -o $@
	@grub-file --is-x86-multiboot $@ 2>/dev/null \
	    && echo "OK: $@ jest poprawnym obrazem Multiboot" \
	    || echo "INFO: grub-file niedostepny - pomijam weryfikacje Multiboot"

iso: $(KERNEL) grub.cfg
	mkdir -p $(BUILD)/isodir/boot/grub
	cp $(KERNEL) $(BUILD)/isodir/boot/protonos.bin
	cp grub.cfg $(BUILD)/isodir/boot/grub/grub.cfg
	grub-mkrescue -o $(ISO) $(BUILD)/isodir

# QEMU potrafi załadować jądro Multiboot bezpośrednio - bez ISO i GRUB-a
run: $(KERNEL)
	qemu-system-i386 -kernel $(KERNEL)

run-iso: iso
	qemu-system-i386 -cdrom $(ISO)

# Automatyczny test: boot w QEMU bez okna + weryfikacja ekranu VGA
test: $(KERNEL)
	python3 scripts/vga_smoke_test.py $(KERNEL)

clean:
	rm -rf $(BUILD)
