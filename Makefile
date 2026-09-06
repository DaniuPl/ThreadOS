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
             $(BUILD)/user/mkwall.elf \
             $(BUILD)/user/gmon.elf $(BUILD)/user/threads.elf $(BUILD)/user/prio.elf $(BUILD)/user/gterm.elf \
             $(BUILD)/user/gedit.elf $(BUILD)/user/gtasks.elf \
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
             $(BUILD)/user/syscalls.elf

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
        $(BUILD)/e1000.o \
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
        $(BUILD)/xhci.o \
        $(BUILD)/usbhc.o \
        $(BUILD)/usbkbd.o \
        $(BUILD)/usbmouse.o \
        $(BUILD)/usbmass.o \
        $(BUILD)/bt.o \
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

.PHONY: all iso iso64 initrd64 run run-iso run-iso64-uefi test fuzz clean \
        kernel64 efi stick run-stick

all: $(KERNEL) $(INITRD)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/boot.o: boot/boot.S | $(BUILD)
	$(AS) $(ASFLAGS) -c $< -o $@

# Trampolina rdzeni wtornych idzie przez gcc, a nie przez as: sa w niej
# makra preprocesora liczace adresy w miejscu docelowym
$(BUILD)/ap.o: boot/ap.S | $(BUILD)
	$(CC) $(ASFLAGS) -c $< -o $@

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

# --- Biblioteka wspoldzielona ----------------------------------------
#
# Kryptografia, TLS, HTTP i DNS zyja w naglowkach user/*.h jako funkcje
# 'static inline'. Kazdy program niosl je dotad w sobie - httpget i gweb
# to po osiemdziesiat kilobajtow tego samego kodu.
#
# Zrodlo zostaje jedno. scripts/split_headers.py robi z kazdego naglowka dwie
# wersje: dla biblioteki (te same funkcje, ale widoczne z zewnatrz) i dla
# programu (same zapowiedzi, bez cial). Zadna z nich nie jest pisana
# recznie, wiec nie ma jak sie rozjechac ze zrodlem.
#
# Kopiowanie zrodel do build/ nie jest ozdoba: '#include "utls.h"' szuka
# najpierw w katalogu pliku, ktory je wciaga. Gdyby libproton.c lezal
# w user/, znalazlby oryginal zamiast wersji wygenerowanej.
LIB_HEADERS := ubignum.h usha256.h usha512.h uchacha.h uaes.h urand.h ux25519.h \
               ursa.h uecdsa.h ux509.h ucert.h utls.h uhttp.h uhtml.h \
               udns.h caroots.h uproton.h
LIB_SRC  := $(patsubst %,$(BUILD)/libsrc/%,$(LIB_HEADERS))
LIB_DECL := $(patsubst %,$(BUILD)/decl/%,$(LIB_HEADERS))
LIBPROTON := $(BUILD)/libproton.so

$(BUILD)/libsrc $(BUILD)/decl $(BUILD)/dyn:
	mkdir -p $@

$(BUILD)/libsrc/%.h: user/%.h scripts/split_headers.py | $(BUILD)/libsrc
	python3 scripts/split_headers.py --lib $< > $@

$(BUILD)/decl/%.h: user/%.h scripts/split_headers.py | $(BUILD)/decl
	python3 scripts/split_headers.py --decl $< > $@

$(BUILD)/libsrc/libproton.c: user/libproton.c | $(BUILD)/libsrc
	cp $< $@

# -Bsymbolic: wywolania wewnatrz biblioteki wiaza sie od razu, przy
# konsolidacji. Bez tego szlyby przez jej wlasna tablice skokow, a te
# wypelnia sie dopiero przy ladowaniu - czym u nas nie ma komu sie zajac.
$(LIBPROTON): $(BUILD)/libsrc/libproton.c $(LIB_SRC) user/libproton.ld \
              $(USER_HEADERS) Makefile
	$(CC) -m32 -std=gnu11 -ffreestanding -fno-stack-protector -fPIC -O2 \
	    -Wall -Wextra -I$(BUILD)/libsrc -Iuser -c $< -o $(BUILD)/libproton.o
	$(CC) -m32 -shared -nostdlib -Wl,--build-id=none -Wl,-Bsymbolic \
	    -Wl,-soname,libproton.so -Wl,--no-warn-rwx-segments \
	    -T user/libproton.ld -o $@ $(BUILD)/libproton.o

$(BUILD)/dyn/%.c: user/%.c | $(BUILD)/dyn
	cp $< $@

# Programy, ktore biora kryptografie z biblioteki zamiast niesc ja same
USER_DYN_CFLAGS := -m32 -std=gnu11 -ffreestanding -fno-stack-protector \
                   -fno-pic -no-pie -nostdlib -O2 -Wall -Wextra \
                   -Wl,--build-id=none -Wl,--no-warn-rwx-segments

# Regula wzorcowa ograniczona do tych pieciu nazw: reszta programow
# buduje sie jak dotad, ze wszystkim w srodku
USER_DYN := cryptotest certtest httpget gweb nslookup dynlib
USER_DYN_ELFS := $(patsubst %,$(BUILD)/user/%.elf,$(USER_DYN))

$(USER_DYN_ELFS): $(BUILD)/user/%.elf: $(BUILD)/dyn/%.c $(LIB_DECL) \
                     $(LIBPROTON) user/userdyn.ld Makefile | $(BUILD)/user
	$(CC) $(USER_DYN_CFLAGS) -I$(BUILD)/decl -Iuser -T user/userdyn.ld \
	    $< $(LIBPROTON) -o $@

$(INITRD): $(USER_ELFS) $(LIBPROTON) scripts/mkinitrd.py
	python3 scripts/mkinitrd.py $@ \
	    libproton.so=$(LIBPROTON) \
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
	    gedit=$(BUILD)/user/gedit.elf gtasks=$(BUILD)/user/gtasks.elf \
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
	    syscalls=$(BUILD)/user/syscalls.elf

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
            kbdmap pci ata ahci nvme blk fs rtl8139 net dhcp tcp video \
            mouse desktop acpi uhci xhci usbhc usbkbd usbmouse usbmass bt part fat gpt fatcmd install hwinfo hda swap prog shm rtc \
            dynlink smpmem smpthreads biglock tlb config user clip \
            e1000 netdev smp
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
               $(BUILD)/user64/shmwrite64.elf \
               $(BUILD)/user64/shmread64.elf \
               $(BUILD)/user64/gclock64.elf \
               $(BUILD)/user64/gfiles.elf \
               $(BUILD)/user64/gmon64.elf \
               $(BUILD)/user64/gterm.elf \
               $(BUILD)/user64/gedit.elf \
               $(BUILD)/user64/gtasks.elf \
               $(BUILD)/user64/ginstal.elf \
               $(BUILD)/user64/gmikser.elf \
               $(BUILD)/user64/gimg.elf \
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
               $(BUILD)/user64/syscalls.elf

$(BUILD)/user64:
	mkdir -p $(BUILD)/user64

# Makefile jest tu zaleznoscia nie bez powodu: zmiana flag kompilacji
# musi przebudowac programy, a bez tego wpisu make uznaje, ze skoro
# zrodla sie nie zmienily, to nie ma nic do roboty.
$(BUILD)/user64/%.elf: user64/%.c user64/usys64.h user64/user64.ld \
                       Makefile | $(BUILD)/user64
	$(CC) $(USER64_CFLAGS) -T user64/user64.ld $< -o $@

# Programy wspolne dla obu jader: kryptografia, TLS, HTTP i DNS zyja
# w user/ i nie wiedza nic o trybie procesora. Do jadra mowia przez
# user/uapi.h, ktore wybiera wlasciwa strone - stad ta druga regula,
# budujaca te same zrodla drugi raz, tyle ze na 64 bity.
$(BUILD)/user64/%.elf: user/%.c user/uapi.h user64/usys64.h \
                       user64/compat64.h user64/user64.ld $(USER_HEADERS) \
                       Makefile | $(BUILD)/user64
	$(CC) $(USER64_CFLAGS) -T user64/user64.ld $< -o $@

# --- Biblioteka wspoldzielona w trybie dlugim -------------------------
#
# Ten sam zabieg, co w jadrze 32-bitowym i z tych samych naglowkow -
# rozny jest tylko adres, pod ktorym biblioteka lezy, i szerokosc
# rejestrow. Zrodla nikt drugi raz nie pisze.
LIBPROTON64 := $(BUILD)/libproton64.so
LIB_DECL64  := $(patsubst %,$(BUILD)/decl/%,$(LIB_HEADERS))

$(LIBPROTON64): $(BUILD)/libsrc/libproton.c $(LIB_SRC) user64/libproton64.ld \
                $(USER_HEADERS) Makefile
	$(CC) -m64 -std=gnu11 -ffreestanding -fno-stack-protector -fPIC \
	    -mno-red-zone -O2 -Wall -Wextra -I$(BUILD)/libsrc -Iuser \
	    -c $< -o $(BUILD)/libproton64.o
	$(CC) -m64 -shared -nostdlib -Wl,--build-id=none -Wl,-Bsymbolic \
	    -Wl,-soname,libproton.so -Wl,--no-warn-rwx-segments \
	    -Wl,-z,max-page-size=0x1000 \
	    -T user64/libproton64.ld -o $@ $(BUILD)/libproton64.o

USER64_DYN_CFLAGS := -m64 -std=gnu11 -ffreestanding -fno-stack-protector \
                     -fno-pic -no-pie -nostdlib -mno-red-zone \
                     -O2 -Wall -Wextra \
                     -Wl,--build-id=none -Wl,-z,max-page-size=0x1000

USER64_DYN_ELFS := $(patsubst %,$(BUILD)/user64/%.elf,$(USER_DYN))

$(USER64_DYN_ELFS): $(BUILD)/user64/%.elf: $(BUILD)/dyn/%.c $(LIB_DECL64) \
                     $(LIBPROTON64) user64/userdyn64.ld Makefile \
                     | $(BUILD)/user64
	$(CC) $(USER64_DYN_CFLAGS) -I$(BUILD)/decl -Iuser \
	    -T user64/userdyn64.ld $< $(LIBPROTON64) -o $@

$(INITRD64): $(USER64_ELFS) $(LIBPROTON64) scripts/mkinitrd.py
	python3 scripts/mkinitrd.py $@ \
	    libproton.so=$(LIBPROTON64) \
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
	    shmwrite64=$(BUILD)/user64/shmwrite64.elf \
	    shmread64=$(BUILD)/user64/shmread64.elf \
	    gclock=$(BUILD)/user64/gclock64.elf \
	    gfiles=$(BUILD)/user64/gfiles.elf \
	    gmon=$(BUILD)/user64/gmon64.elf \
	    gterm=$(BUILD)/user64/gterm.elf \
	    gedit=$(BUILD)/user64/gedit.elf \
	    gtasks=$(BUILD)/user64/gtasks.elf \
	    ginstal=$(BUILD)/user64/ginstal.elf \
	    gmikser=$(BUILD)/user64/gmikser.elf \
	    gimg=$(BUILD)/user64/gimg.elf \
	    cryptotest=$(BUILD)/user64/cryptotest.elf \
	    certtest=$(BUILD)/user64/certtest.elf \
	    aestest=$(BUILD)/user64/aestest.elf \
	    wpatest=$(BUILD)/user64/wpatest.elf \
	    nslookup=$(BUILD)/user64/nslookup.elf \
	    httpget=$(BUILD)/user64/httpget.elf \
	    gweb=$(BUILD)/user64/gweb.elf \
	    mkmap64=$(BUILD)/user64/mkmap64.elf \
	    tlbtest64=$(BUILD)/user64/tlbtest64.elf \
	    mmapfile64=$(BUILD)/user64/mmapfile64.elf \
	    mmapzap64=$(BUILD)/user64/mmapzap64.elf \
	    dynlib=$(BUILD)/user64/dynlib.elf \
	    cores=$(BUILD)/user64/cores.elf \
	    syscalls=$(BUILD)/user64/syscalls.elf
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
	python3 -u scripts/run_tests.py $(if $(T),-t$(T)) $(if $(J),-j$(J)) \
	    $(KERNEL) $(INITRD) $(KERNEL64) $(INITRD64)

# Parsery dostaja dane prosto z sieci - ten cel przepuszcza je przez
# sanitizery na hoscie, karmiac uszkodzonymi wejsciami (certyfikaty,
# HTML, HTTP oraz ramki trafiajace do net.c i tcp.c w jadrze).
fuzz:
	python3 scripts/fuzz_parsers.py 500

clean:
	rm -rf $(BUILD)

# --- Wlasny program rozruchowy UEFI ----------------------------------
#
# Aplikacja EFI to plik w formacie PE32+ z konwencją wywolan Microsoftu,
# czyli dwie rzeczy, ktorych GCC na Linuksie nie robi. Robi je clang z
# lld-linkiem - i to jedyne, co jest tu cudze: format pliku. Kod jest
# w calosci nasz, bez gnu-efi i bez EDK2.
EFI_OUT := $(BUILD)/BOOTX64.EFI
EFI_CC  := clang -target x86_64-unknown-windows -ffreestanding \
           -fshort-wchar -mno-red-zone -Wall -Wextra -O2

$(EFI_OUT): boot/efi/efiboot.c boot/efi/efi.h | $(BUILD)
	@command -v clang >/dev/null && command -v lld-link >/dev/null || { \
	    echo "BLAD: brak clang albo lld-link (apt install clang lld)"; \
	    exit 1; }
	$(EFI_CC) -c boot/efi/efiboot.c -o $(BUILD)/efiboot.o
	lld-link -subsystem:efi_application -entry:efi_main -nodefaultlib \
	    -out:$@ $(BUILD)/efiboot.o
	@echo "Gotowe: $@"

efi: $(EFI_OUT)

# Firmware UEFI dla QEMU. Te same pliki, ktorych uzywa run-iso64-uefi.
OVMF_CODE := /usr/share/OVMF/OVMF_CODE_4M.fd
OVMF_VARS := /usr/share/OVMF/OVMF_VARS_4M.fd

# --- Pendrive, z ktorego startuje ta maszyna -------------------------
#
# To, z czego startuje maszyna z UEFI, nie jest plyta ani jadrem: to
# dysk z tablica GPT, partycja EFI w srodku, FAT-em w niej i loaderem
# pod nazwa, ktora firmware zna. Ten cel robi dokladnie taki obraz -
# surowy, do wgrania na pendrive przez dd. Bez GRUB-a i bez El Torito.
STICK := $(BUILD)/protonos-stick.img

$(STICK): $(EFI_OUT) $(KERNEL64) $(INITRD64) scripts/mkstick.py
	@command -v mformat >/dev/null || { \
	    echo "BLAD: brak mtools (apt install mtools)"; exit 1; }
	python3 scripts/mkstick.py $@ $(EFI_OUT) $(KERNEL64) $(INITRD64)

stick: $(STICK)

# Ten sam obraz uruchomiony pod OVMF - zanim pojdzie na pendrive,
# warto zobaczyc, ze startuje.
run-stick: $(STICK)
	@test -f $(OVMF_CODE) || { echo "BLAD: brak OVMF"; exit 1; }
	cp $(OVMF_VARS) $(BUILD)/ovmf_vars.fd
	qemu-system-x86_64 -m 512 \
	    -drive if=pflash,format=raw,unit=0,readonly=on,file=$(OVMF_CODE) \
	    -drive if=pflash,format=raw,unit=1,file=$(BUILD)/ovmf_vars.fd \
	    -drive file=$(STICK),format=raw,media=disk
