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
             $(BUILD)/user/forktest.elf $(BUILD)/user/ptree.elf $(BUILD)/user/sigtest.elf $(BUILD)/user/lazymem.elf $(BUILD)/user/swaptest.elf

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
        $(BUILD)/rtc.o \
        $(BUILD)/mouse.o \
        $(BUILD)/video.o \
        $(BUILD)/desktop.o \
        $(BUILD)/pci.o \
        $(BUILD)/shm.o \
        $(BUILD)/rtl8139.o \
        $(BUILD)/net.o \
        $(BUILD)/dhcp.o \
        $(BUILD)/tcp.o \
        $(BUILD)/signal.o \
        $(BUILD)/swap.o \
        $(BUILD)/ahci.o \
        $(BUILD)/blk.o \
        $(BUILD)/uhci.o \
        $(BUILD)/usbkbd.o \
        $(BUILD)/acpi.o \
        $(BUILD)/mb2.o \
        $(BUILD)/fbcon.o

.PHONY: all iso iso64 initrd64 run run-iso run-iso64-uefi test fuzz clean \
        kernel64

all: $(KERNEL) $(INITRD)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/boot.o: boot/boot.S | $(BUILD)
	$(AS) $(ASFLAGS) -c $< -o $@

$(BUILD)/isr_stubs.o: kernel/isr.S | $(BUILD)
	$(AS) $(ASFLAGS) -c $< -o $@

$(BUILD)/switch.o: kernel/switch.S | $(BUILD)
	$(AS) $(ASFLAGS) -c $< -o $@

# Zaleznosc od naglowkow jest tu wypisana wprost: bez niej zmiana
# w kernel/include/ nie przebudowywala nic, a make konczyl sie zielono,
# choc obiekty pamietaly stary prototyp.
KERNEL_HEADERS := $(wildcard kernel/include/*.h)

$(BUILD)/%.o: kernel/%.c $(KERNEL_HEADERS) | $(BUILD)
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
	    forktest=$(BUILD)/user/forktest.elf ptree=$(BUILD)/user/ptree.elf \
	    sigtest=$(BUILD)/user/sigtest.elf \
	    lazymem=$(BUILD)/user/lazymem.elf \
	    swaptest=$(BUILD)/user/swaptest.elf

# --- Jadro 64-bitowe (tryb dlugi) ------------------------------------
#
# Drugi obraz, budowany z tych samych zrodel: rozne sa tylko wejscie
# (boot64/boot64.S wchodzi w tryb dlugi) i szkielet kmain64. Reszta to
# te same pliki kompilowane po raz drugi - roznice miedzy trybami
# zamykaja sie w naglowku io.h i w kilku rzutowaniach.
KERNEL64 := $(BUILD)/protonos64.bin
CFLAGS64 := -m64 -std=gnu11 -ffreestanding -fno-stack-protector -fno-pic \
            -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -Wall -Wextra -O2 -g
LDFLAGS64 := -m64 -ffreestanding -nostdlib -static -no-pie -T linker64.ld \
             -Wl,--build-id=none -Wl,--no-warn-rwx-segments

SHARED64 := string serial printk mb2 fbcon vga pic pit pmm heap initrd \
            kbdmap pci ata ahci blk fs rtl8139 net dhcp tcp video \
            mouse desktop acpi uhci usbkbd swap prog shm rtc
OBJS64 := $(BUILD)/64/boot64.o $(BUILD)/64/isr64_stubs.o \
          $(BUILD)/64/switch64.o \
          $(BUILD)/64/kmain64.o $(BUILD)/64/idt64.o $(BUILD)/64/isr64.o \
          $(BUILD)/64/paging64.o $(BUILD)/64/task64.o \
          $(BUILD)/64/gdt64.o $(BUILD)/64/syscall64.o \
          $(BUILD)/64/elf64.o $(BUILD)/64/proc64.o \
          $(BUILD)/64/keyboard64.o $(BUILD)/64/shell64.o \
          $(BUILD)/64/signal64.o $(BUILD)/64/fpu64.o \
          $(BUILD)/64/pipe64.o $(BUILD)/64/compat64.o \
          $(BUILD)/64/syscall64_stub.o \
          $(addprefix $(BUILD)/64/,$(addsuffix .o,$(SHARED64)))

$(BUILD)/64:
	mkdir -p $(BUILD)/64

$(BUILD)/64/boot64.o: boot64/boot64.S | $(BUILD)/64
	$(CC) -m64 -c $< -o $@

$(BUILD)/64/isr64_stubs.o: boot64/isr64.S | $(BUILD)/64
	$(CC) -m64 -c $< -o $@

$(BUILD)/64/switch64.o: boot64/switch64.S | $(BUILD)/64
	$(CC) -m64 -c $< -o $@

$(BUILD)/64/syscall64_stub.o: boot64/syscall64.S | $(BUILD)/64
	$(CC) -m64 -c $< -o $@

# Programy uzytkownika trybu dlugiego. Buduja sie tak samo jak
# 32-bitowe, tylko innym linkerem i z wlasna biblioteczka syscalli -
# w trybie dlugim do jadra wchodzi sie instrukcja syscall, nie
# bramka przerwania.
INITRD64 := $(BUILD)/initrd64.img
# Programy uzytkownika buduja sie z SSE - na x86-64 jest ono czescia
# konwencji wywolan, a jadro potrafi juz zapisac i odtworzyc stan tych
# rejestrow przy przelaczaniu zadan. Samo jadro zostaje bez SSE:
# w obsludze przerwania nie ma czego liczyc, a kazda taka instrukcja
# wymagalaby zapisania stanu przerwanego programu.
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
               $(BUILD)/user64/shmpisz64.elf \
               $(BUILD)/user64/shmczytaj64.elf \
               $(BUILD)/user64/gclock64.elf \
               $(BUILD)/user64/gfiles64.elf \
               $(BUILD)/user64/gmon64.elf \
               $(BUILD)/user64/gterm64.elf

$(BUILD)/user64:
	mkdir -p $(BUILD)/user64

# Makefile jest tu zaleznoscia nie bez powodu: zmiana flag kompilacji
# musi przebudowac programy, a bez tego wpisu make uznaje, ze skoro
# zrodla sie nie zmienily, to nie ma nic do roboty.
$(BUILD)/user64/%.elf: user64/%.c user64/usys64.h user64/user64.ld \
                       Makefile | $(BUILD)/user64
	$(CC) $(USER64_CFLAGS) -T user64/user64.ld $< -o $@

$(INITRD64): $(USER64_ELFS) scripts/mkinitrd.py
	python3 scripts/mkinitrd.py $@ \
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
	    uupper64=$(BUILD)/user64/uupper64.elf \
	    unet64=$(BUILD)/user64/unet64.elf \
	    guiapp=$(BUILD)/user64/guiapp64.elf \
	    swaptest64=$(BUILD)/user64/swaptest64.elf \
	    shmpisz64=$(BUILD)/user64/shmpisz64.elf \
	    shmczytaj64=$(BUILD)/user64/shmczytaj64.elf \
	    gclock=$(BUILD)/user64/gclock64.elf \
	    gfiles=$(BUILD)/user64/gfiles64.elf \
	    gmon=$(BUILD)/user64/gmon64.elf \
	    gterm=$(BUILD)/user64/gterm64.elf
	@echo "Gotowe: $@ ($$(stat -c%s $@) B)"

initrd64: $(INITRD64)

KERNEL64_HEADERS := $(wildcard kernel64/*.h)

$(BUILD)/64/%.o: kernel64/%.c $(KERNEL_HEADERS) $(KERNEL64_HEADERS) | $(BUILD)/64
	$(CC) $(CFLAGS64) -c $< -o $@

$(BUILD)/64/%.o: kernel/%.c $(KERNEL_HEADERS) | $(BUILD)/64
	$(CC) $(CFLAGS64) -c $< -o $@

$(KERNEL64): $(OBJS64) linker64.ld
	$(CC) $(LDFLAGS64) $(OBJS64) -o $@

kernel64: $(KERNEL64) $(INITRD64)
	@echo "Gotowe: $(KERNEL64) (tryb dlugi, ladowany przez GRUB-a)"

# Obraz ISO z jadrem 64-bitowym, startujacy i pod starym BIOS-em,
# i pod UEFI. Ta sama plyta obsluguje obie drogi: grub-mkrescue wklada
# do niej dwa zestawy plikow GRUB-a i partycje EFI, a firmware wybiera
# ten, ktory rozumie. To wlasnie brakowalo do rozruchu na nowszym
# sprzecie - stary obraz umial tylko droge BIOS-owa.
ISO64 := $(BUILD)/protonos64.iso

$(ISO64): $(KERNEL64) $(INITRD64) grub64.cfg
	@command -v grub-mkrescue >/dev/null || { \
	    echo "BLAD: brak grub-mkrescue."; \
	    echo "  Debian/Ubuntu: apt install grub-pc-bin grub-efi-amd64-bin \
grub-common xorriso mtools"; \
	    exit 1; }
	mkdir -p $(BUILD)/isodir64/boot/grub
	cp $(KERNEL64) $(BUILD)/isodir64/boot/protonos64.bin
	cp $(INITRD64) $(BUILD)/isodir64/boot/initrd64.img
	cp grub64.cfg $(BUILD)/isodir64/boot/grub/grub.cfg
	grub-mkrescue -o $@ $(BUILD)/isodir64
	@echo "Gotowe: $@ (BIOS + UEFI)"

iso64: $(ISO64)

# Rozruch przez UEFI wymaga firmware - OVMF udaje je w QEMU tak samo,
# jak robi to prawdziwa plyta glowna
run-iso64-uefi: $(ISO64)
	cp /usr/share/OVMF/OVMF_VARS_4M.fd $(BUILD)/ovmf_vars.fd
	qemu-system-x86_64 -cdrom $(ISO64) -boot d \
	    -drive if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
	    -drive if=pflash,format=raw,unit=1,file=$(BUILD)/ovmf_vars.fd

# Pusty obraz dysku 16 MiB pod system plikow PFS (jadro go sformatuje).
# PFS3 przydziela bloki z bitmapy, wiec miejsce idzie tam, gdzie trzeba.
$(DISK): | $(BUILD)
	dd if=/dev/zero of=$@ bs=1M count=16 status=none

# Obraz ISO do wypalenia albo uruchomienia z 'qemu -cdrom'. GRUB wgrywa
# jadro przez Multiboot i podaje mu initrd jako modul; menu pozwala wybrac
# pulpit albo konsole tekstowa (grub.cfg).
iso: $(KERNEL) $(INITRD) grub.cfg
	@command -v grub-mkrescue >/dev/null || { \
	    echo "BLAD: brak grub-mkrescue."; \
	    echo "  Debian/Ubuntu: apt install grub-pc-bin grub-common xorriso"; \
	    echo "  Fedora:        dnf install grub2-tools xorriso"; \
	    exit 1; }
	mkdir -p $(BUILD)/isodir/boot/grub
	cp $(KERNEL) $(BUILD)/isodir/boot/protonos.bin
	cp $(INITRD) $(BUILD)/isodir/boot/initrd.img
	cp grub.cfg $(BUILD)/isodir/boot/grub/grub.cfg
	grub-mkrescue -o $(ISO) $(BUILD)/isodir
	@echo "Gotowe: $(ISO) - 'make run-iso' albo wypal na pendrive"

# QEMU potrafi załadować jądro Multiboot bezpośrednio - bez ISO i GRUB-a.
# Dysk (-hda) trafia na pierwsza magistrale ATA jako master.
# Siec: karta RTL8139 w trybie uzytkownika QEMU (adres z DHCP: 10.0.2.15,
# brama 10.0.2.2). hostfwd wystawia serwer TCP z ProtonOS-a na hosta:
# po 'run tcpserv' mozna z terminala zrobic 'nc localhost 5555'.
NETOPTS := -netdev user,id=n0,hostfwd=tcp::5555-:5555 \
           -device rtl8139,netdev=n0

run: $(KERNEL) $(INITRD) $(DISK)
	qemu-system-i386 -kernel $(KERNEL) -initrd $(INITRD) -hda $(DISK) $(NETOPTS)

# Uruchomienie z ISO tak, jak na prawdziwej maszynie: dysk (PFS + plik
# wymiany) i karta sieciowa, bo bez nich polowa systemu nie ma co robic.
run-iso: iso $(DISK)
	qemu-system-i386 -cdrom $(ISO) -boot d -hda $(DISK) $(NETOPTS)

# Automatyczny test: boot w QEMU bez okna + weryfikacja ekranu VGA.
# Test sam tworzy swiezy obraz dysku i sprawdza persystencje miedzy boot-ami.
# Grupy testu chodza rownolegle, bo kazda ma wlasna maszyne QEMU
# i wiekszosc czasu i tak czeka: na rozruch, na odpowiedz, na
# przerysowanie ekranu.
#
#   make test            wszystkie grupy
#   make test T=k64      tylko jadro 64-bitowe (albo k64,uefi)
#   make test J=1        jedna po drugiej, do debugowania
test: $(KERNEL) $(INITRD) $(KERNEL64) $(INITRD64)
	python3 -u scripts/run_testy.py $(if $(T),-t$(T)) $(if $(J),-j$(J)) \
	    $(KERNEL) $(INITRD) $(KERNEL64) $(INITRD64)

# Parsery dostaja dane prosto z sieci - ten cel przepuszcza je przez
# sanitizery na hoscie, karmiac uszkodzonymi wejsciami (certyfikaty,
# HTML, HTTP oraz ramki trafiajace do net.c i tcp.c w jadrze).
fuzz:
	python3 scripts/fuzz_parsers.py 500

clean:
	rm -rf $(BUILD)
