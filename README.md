# ProtonOS

Własny system operacyjny na PC (x86, 32-bit), pisany od zera w C i asemblerze.
Jądro jest zgodne ze specyfikacją **Multiboot**, więc bootuje je GRUB albo
bezpośrednio QEMU.

## Struktura projektu

```
ProtonOS/
├── boot/
│   └── boot.S          # nagłówek Multiboot, ustawienie stosu, skok do kmain()
├── kernel/
│   ├── kmain.c         # główny punkt wejścia jądra w C
│   ├── gdt.c           # własna GDT (płaski model, segmenty ring 0 i ring 3)
│   ├── idt.c           # IDT - 256 bramek przerwań
│   ├── isr.S           # stuby przerwań w asemblerze (wyjątki + IRQ)
│   ├── isr.c           # wspólny handler: wyjątki CPU i dispatch IRQ
│   ├── pic.c           # remapowanie PIC 8259, maskowanie, EOI
│   ├── pit.c           # timer PIT (100 Hz), licznik uptime
│   ├── keyboard.c      # klawiatura PS/2 - scancode -> ASCII
│   ├── task.c          # wątki jądra + scheduler (round-robin, wywłaszczanie)
│   ├── switch.S        # przełączanie kontekstu (rejestry callee-saved + ESP)
│   ├── syscall.c       # syscalle int 0x80: write/exit/sleep/getticks/
│   │                   #   read/spawn/wait/getpid
│   ├── usermode.c      # wejście w ring 3 przez iret
│   ├── exec.c          # loader ELF32: przestrzeń adresowa, segmenty, argv, sbrk
│   ├── initrd.c        # initrd (moduł Multiboot, format PIRD)
│   ├── shell.c         # prosty shell: help, ps, mem, uptime, clear, reboot...
│   ├── pmm.c           # alokator ramek fizycznych (bitmapa, mapa Multiboot)
│   ├── paging.c        # stronicowanie - mapowanie identycznościowe 64 MiB
│   ├── heap.c          # sterta jądra: kmalloc/kfree (first-fit + scalanie)
│   ├── serial.c        # COM1 - logi widoczne w QEMU przez -serial stdio
│   ├── printk.c        # formatowane wypisywanie (%s %c %d %u %x %p)
│   ├── vga.c           # sterownik trybu tekstowego VGA
│   ├── string.c        # memset/memcpy/strlen (brak libc w jądrze)
│   └── include/        # nagłówki jądra
├── user/               # programy użytkownika (osobne ELF-y!)
│   ├── usys.h          # biblioteczka syscalli (int 0x80)
│   ├── hello.c         # grzeczny proces: liczy i wychodzi przez sys_exit
│   ├── evil.c          # niegrzeczny: czyta pamięć jądra i ginie
│   ├── sh.c            # ush - shell użytkownika (read/spawn/wait w ring 3)
│   ├── echo.c          # wypisuje swoje argumenty (argc/argv)
│   ├── memtest.c       # alokuje pamięć przez sbrk/malloc i ją weryfikuje
│   └── user.ld         # programy linkowane pod 0x08048000
├── scripts/
│   ├── mkinitrd.py     # pakuje ELF-y do obrazu initrd (format PIRD)
│   └── vga_smoke_test.py  # automatyczny test e2e w QEMU (make test)
├── linker.ld           # skrypt linkera - jądro ładowane pod adres 1 MiB
├── grub.cfg            # konfiguracja GRUB-a dla obrazu ISO
└── Makefile
```

## Wymagania

- `gcc` z obsługą `-m32` (na Ubuntu/Debianie: `sudo apt install gcc-multilib`)
- `make`
- do uruchomienia: `qemu-system-i386` (pakiet `qemu-system-x86`)
- do zbudowania ISO: `grub-mkrescue` + `xorriso` (pakiety `grub-pc-bin`, `xorriso`)

## Budowanie i uruchamianie

```sh
make          # buduje build/protonos.bin
make run      # uruchamia jądro w QEMU (najprostsza droga)
make test     # automatyczny test: boot headless + weryfikacja ekranu VGA
make iso      # buduje bootowalne ISO z GRUB-em
make run-iso  # uruchamia ISO w QEMU
make clean
```

Po uruchomieniu jądro wypisuje status inicjalizacji każdego podsystemu,
w prawym górnym rogu tyka licznik uptime, a wszystko co wpiszesz na
klawiaturze jest echo'wane na ekran. Logi jądra idą równolegle na port
szeregowy (`make run` + opcja `-serial stdio` w QEMU pokaże je w terminalu).

`make test` bootuje jądro w QEMU bez okna i przez QMP sprawdza zawartość
ekranu VGA, tykanie timera oraz echo klawiatury (wstrzykuje klawisze
poleceniem `sendkey`) - można rozwijać system bez wpatrywania się w okno.

## Jak to działa

1. **GRUB/QEMU** znajduje w pierwszych 8 KiB obrazu nagłówek Multiboot
   (`boot/boot.S`), ładuje jądro pod adres 1 MiB i skacze do `_start`
   już w 32-bitowym trybie chronionym.
2. `_start` ustawia stos (16 KiB w sekcji `.bss`) i woła `kmain()`,
   przekazując magic Multiboot oraz wskaźnik na strukturę z informacjami
   od bootloadera (m.in. mapę pamięci).
3. `kmain()` inicjalizuje sterownik VGA i wypisuje tekst, pisząc
   bezpośrednio do bufora pod adresem `0xB8000`.

## Plan rozwoju

- [x] GDT (Global Descriptor Table) - własne deskryptory segmentów
- [x] IDT + obsługa wyjątków CPU
- [x] PIC (remapowanie) i przerwania sprzętowe (IRQ)
- [x] Sterownik klawiatury (PS/2)
- [x] Timer (PIT) i licznik czasu
- [x] Port szeregowy (COM1) + printk
- [x] Odczyt mapy pamięci z Multiboot
- [x] Zarządzanie pamięcią fizyczną (alokator ramek - bitmapa)
- [x] Stronicowanie (identycznościowe 64 MiB; page fault z diagnostyką CR2)
- [x] Alokator pamięci jądra (kmalloc/kfree)
- [x] Prosty shell w jądrze (help, ps, mem, uptime, clear, crash, reboot)
- [x] Wątki jądra + scheduler (round-robin, wywłaszczanie z timera,
      sleep/yield/exit, sprzątanie zombie)
- [x] Tryb użytkownika (ring 3): TSS, syscalle int 0x80, ochrona pamięci
      (proces sięgający po pamięć jądra ginie, system żyje dalej)
- [x] Osobne przestrzenie adresowe (katalog stron per proces, CR3 w
      schedulerze, pełne sprzątanie ramek po zakończeniu procesu)
- [x] Ładowanie programów ELF z initrd (`ls`, `run hello`, `run evil`)
- [x] Syscalle read (klawiatura, blokujące), spawn, wait, getpid
- [x] Shell jako program użytkownika (`run sh` - ring 3, przejmuje
      konsolę, uruchamia programy przez spawn+wait, `exit` wraca do jądra)
- [x] Argumenty programów (argc/argv) - `run echo proton os dziala`
- [x] Syscall sbrk + malloc w userspace (dynamiczna pamięć) - `run memtest`
- [ ] System plików z prawdziwego dysku (ATA + np. FAT)
- [ ] Potoki / przekierowania, zapis do plików (sys_open/read/write)
- [ ] mmap, współdzielona pamięć między procesami
