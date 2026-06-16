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
│   ├── ata.c           # sterownik dysku ATA/IDE (PIO, LBA28, polling)
│   ├── fs.c            # PFS - system plików z katalogami (ścieżki, ., ..)
│   ├── kfile.c         # deskryptory plików: konsola/pliki/potoki (open/read/write/seek)
│   ├── pipe.c          # potoki międzyprocesowe (bufor kołowy, blokowanie)
│   ├── rtc.c           # zegar czasu rzeczywistego (CMOS, BCD/binarny)
│   ├── mouse.c         # mysz PS/2 (IRQ12, pakiety 3-bajtowe, pozycja)
│   ├── video.c         # tryb graficzny: VGA 13h (8bpp) i VESA liniowy (32bpp),
│   │                   #   podwójne buforowanie, tekst 8x8
│   ├── pci.c           # dostęp do PCI (znalezienie framebuffera karty)
│   ├── shm.c           # pamięć współdzielona między procesami (po kluczu)
│   ├── desktop.c       # kompozytor: okna jądra i aplikacji, drag, [x], kursor
│   ├── shell.c         # shell: help, ps, mem, ls, run, fls, cat, date...
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
│   ├── sh.c            # ush - shell użytkownika (spawn/wait, > oraz |)
│   ├── echo.c          # wypisuje swoje argumenty (argc/argv)
│   ├── memtest.c       # alokuje pamięć przez sbrk/malloc i ją weryfikuje
│   ├── fileio.c        # zapis i odczyt pliku przez deskryptory (open/read/write)
│   ├── cat.c           # wypisuje plik na konsolę (read pliku -> write na fd 1)
│   ├── wc.c            # liczy bajty/wiersze ze stdin (konsument potoku)
│   ├── seq.c           # wypisuje 1..N (producent potoku)
│   ├── grep.c          # przepuszcza wiersze zawierające wzorzec
│   ├── head.c          # przepuszcza pierwsze N wierszy
│   ├── ls.c            # listuje pliki na dysku (sys_readdir)
│   ├── rm.c            # usuwa plik (sys_unlink)
│   ├── date.c          # data i godzina z RTC (sys_time)
│   ├── cp.c            # kopiuje plik (read src -> write dst)
│   ├── mv.c            # przenosi plik (kopia + unlink źródła)
│   ├── mkdir.c         # tworzy katalog (sys_mkdir)
│   ├── spin.c          # długo żyjący proces do demonstracji kill
│   ├── guiapp.c        # interaktywna aplikacja ring 3 (okno + klik/klawisz)
│   ├── gclock.c        # zegar w oknie graficznym (RTC, uruchamiany z menu)
│   ├── gfiles.c        # menedżer plików GUI - lista + klik->podgląd treści
│   ├── gmon.c          # monitor zasobów (RAM/uptime na żywo, sys_sysinfo)
│   ├── shmtest.c/shmread.c # demo pamięci współdzielonej
│   ├── threads.c       # wątki w procesie (wspólna przestrzeń adresowa)
│   ├── mmaptest.c      # test syscalla mmap (świeża pamięć procesu)
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
Test uruchamia QEMU **dwukrotnie na tym samym obrazie dysku**, więc
weryfikuje też, że pliki zapisane na PFS przeżywają restart.

`make run` dołącza obraz dysku `build/disk.img` (jako `-hda`); jądro
formatuje pusty dysk przy pierwszym starcie. Pliki zapisane przez `write`
lub syscall `fwrite` zostają na nim między uruchomieniami.

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
- [x] Sterownik dysku ATA (PIO) + system plików PFS - pliki przeżywają restart
- [x] Deskryptory plików w stylu Uniksa: open/close/read/write/seek,
      fd 0/1/2 = klawiatura/konsola, wyższe fd = pliki (jeden `write` na
      wszystko); program `cat` w ring 3
- [x] Przekierowanie wyjścia w shellu (`echo tekst > plik`) - jądro
      podmienia stdout procesu na uchwyt pliku przed wejściem w ring 3
- [x] Potoki międzyprocesowe (`echo cos | wc`) - dwa procesy równolegle,
      bufor w jądrze, konsument blokuje się na pustym potoku i dostaje
      EOF, gdy producent się kończy
- [x] Narzędzia uniksowe komponujące się przez potoki: `seq`, `grep`,
      `head`, `wc`, `cat` (np. `seq 9 | grep 7 > plik`)
- [x] Wielostopniowe potoki (`seq 20 | grep 1 | wc`, do 5 etapów)
- [x] Zarządzanie plikami z ring 3: `ls` (sys_readdir), `rm` (sys_unlink)
- [x] Status wyjścia procesu zwracany przez `wait` (kod widoczny w `ush`;
      `grep` zwraca 1 bez dopasowań, proces ubity wyjątkiem → -1)
- [x] Zegar czasu rzeczywistego (RTC/CMOS) + program `date`
- [x] Dopisywanie `>>` oraz programy `cp` i `mv`
- [x] Katalogi w PFS: ścieżki (absolutne/względne, `.`/`..`), katalog
      roboczy per proces (dziedziczony), `mkdir`/`cd`/`pwd`/`ls`
- [x] Procesy w tle (`cmd &`) i `kill <pid>` (dostarczane przy powrocie
      do ring 3 - z syscalla albo przerwania timera)
- [x] Sterownik myszy PS/2 (IRQ12) - pozycja i przyciski (komenda `mouse`)
- [x] Tryb graficzny VGA 13h (320x200x256): piksele, prostokąty, paleta
      (komenda `gfx`, testowany przez QMP screendump)
- [x] Kursor myszy na pulpicie graficznym (kompozytor jako wątek jądra)
- [x] Renderowanie tekstu czcionką bitmapową 8x8
- [x] Okna z paskami tytułu, przeciąganie myszą (kompozytor) - `desktop`
- [x] Klikalne kontrolki: przycisk zamknięcia [x], przycisk akcji z licznikiem
- [x] Aplikacje graficzne w ring 3: proces tworzy własne okno i rysuje
      przez syscalle (`win_create`/`win_rect`/`win_text`); kompozytor składa
      jego bufor i sprząta okno po śmierci procesu (`run guiapp`)
- [x] Dostarczanie zdarzeń do aplikacji: kliknięcie w okno trafia do
      procesu przez kolejkę zdarzeń (`win_poll`), aplikacja reaguje
- [x] Zdarzenia klawiatury do okna z fokusem (klik fokusuje; klik w
      pulpit zdejmuje fokus i klawisze wracają do shella jądra)
- [x] Pasek zadań z przyciskiem `Menu` i menu uruchamiania aplikacji
      (klik uruchamia program przez exec_spawn) - `guiapp`, `gclock`
- [x] Lista otwartych okien na pasku zadań: podświetlenie okna z fokusem,
      klik wynosi okno na wierzch (porządek Z niezależny od slotu)
- [x] Wyższa rozdzielczość VESA (640x480x32 przez DISPI + PCI), pulpit
      z podwójnym buforowaniem (bez migotania); fallback do 320x200
- [x] Syscall `mmap` - anonimowa pamięć procesu (świeża, zerowana),
      zwalniana z przestrzenią adresową; `run mmaptest`
- [x] Menedżer plików GUI (`gfiles`) - lista plików z PFS + kliknięcie
      nazwy otwiera plik i pokazuje treść (FS -> klik -> read -> okno)
- [x] Pamięć współdzielona między procesami (`sys_shm` po kluczu) -
      dwa procesy widzą tę samą pamięć (`shmtest` + `shmread`)
- [x] Ikony na pulpicie - klikalne kafelki uruchamiające aplikacje
- [x] Wskaźnik zasobów (`gmon`) - RAM/uptime na żywo + pasek użycia
      pamięci (syscall `sys_sysinfo`)
- [x] Wątki w procesie (`sys_thread`) - wiele zadań współdzieli przestrzeń
      adresową; przestrzeń zwalniana po ostatnim wątku (`run threads`)
- [ ] Lepszy scheduler z priorytetami
