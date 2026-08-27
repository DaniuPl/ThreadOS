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
│   ├── keyboard.c      # klawiatura PS/2 - scancode -> ASCII, strzałki/PgUp
│   ├── task.c          # wątki jądra + scheduler (round-robin, wywłaszczanie)
│   ├── switch.S        # przełączanie kontekstu (rejestry callee-saved + ESP)
│   ├── syscall.c       # syscalle int 0x80 (m.in. gniazda UDP) + walidacja
│   │                   #   wskaźników z ring 3
│   │                   #   (user_ok/user_str: zły adres = -1, nie page fault)
│   ├── usermode.c      # wejście w ring 3 przez iret
│   ├── exec.c          # loader ELF32: przestrzeń adresowa, segmenty, argv, sbrk
│   ├── initrd.c        # initrd (moduł Multiboot, format PIRD)
│   ├── ata.c           # sterownik dysku ATA/IDE (PIO, LBA28, IDENTIFY)
│   ├── fs.c            # PFS3 - bloki 1 KiB z bitmapy, i-węzły (do 64 MiB)
│   ├── kfile.c         # deskryptory plików: konsola/pliki/potoki (open/read/write/seek)
│   ├── pipe.c          # potoki międzyprocesowe (bufor kołowy, blokowanie)
│   ├── rtc.c           # zegar czasu rzeczywistego (CMOS, BCD/binarny)
│   ├── mouse.c         # mysz PS/2 (IRQ12, pakiety 3-bajtowe, pozycja)
│   ├── video.c         # tryb graficzny: VGA 13h (8bpp) i VESA liniowy (32bpp),
│   │                   #   podwójne buforowanie, tekst 8x8
│   ├── pci.c           # dostęp do PCI (znalezienie framebuffera karty)
│   ├── shm.c           # pamięć współdzielona między procesami (po kluczu)
│   ├── rtl8139.c       # sterownik karty sieciowej Realtek RTL8139 (DMA)
│   ├── net.c           # stos sieciowy: Ethernet + ARP + IPv4 + ICMP + UDP
│   ├── dhcp.c          # klient DHCP (DISCOVER/OFFER/REQUEST/ACK)
│   ├── tcp.c           # TCP: klient i serwer, ACK-i, retransmisje, FIN
│   ├── signal.c        # sygnały: doręczanie, ramka na stosie, sigreturn
│   ├── desktop.c       # kompozytor: okna (drag, resize, [_]/[x]), ikony, kursor
│   ├── shell.c         # shell: help, ps, mem, ls, run, fls, cat, date,
│   │                   #   ifconfig, arp, ping...
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
│   ├── evil2.c         # podaje jądru wskaźniki na jego pamięć - dostaje -1
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
│   ├── udpecho.c       # serwer UDP w ring 3 (odsyła, co dostał)
│   ├── udptest.c       # klient UDP - datagram do udpecho i z powrotem
│   ├── udns.h          # klient DNS dla programów (wspólny dla nslookup/gweb)
│   ├── uhttp.h         # klient HTTP/1.1: chunked, przekierowania, URL-e
│   ├── uhtml.h         # HTML -> tekst: znaczniki, encje, odnośniki, wiersze
│   ├── usha256.h       # SHA-256, HMAC, HKDF (fundament pod TLS 1.3)
│   ├── uchacha.h       # ChaCha20, Poly1305, AEAD (RFC 8439)
│   ├── ux25519.h       # X25519 - wymiana kluczy na Curve25519
│   ├── urand.h         # źródło losowości (RTC + timer + SHA-256)
│   ├── utls.h          # klient TLS 1.3: handshake, klucze, rekordy
│   ├── ubignum.h       # wielkie liczby (Montgomery) pod RSA
│   ├── ursa.h          # podpisy RSA: PKCS#1 v1.5 i PSS
│   ├── ux509.h         # parser certyfikatów X.509 (DER)
│   ├── uecdsa.h        # ECDSA na krzywych P-256 i P-384 (weryfikacja)
│   ├── usha512.h       # SHA-512 i SHA-384 (podpisy na P-384)
│   ├── caroots.h       # wbudowane korzenie CA (generowane skryptem)
│   ├── ucert.h         # weryfikacja łańcucha, dat i nazwy hosta
│   ├── nslookup.c      # klient DNS (zapytanie A przez UDP z ring 3)
│   ├── httpget.c       # klient HTTP/HTTPS (na konsolę albo do pliku)
│   ├── tcpserv.c       # serwer TCP w ring 3 (nasłuch na porcie 5555)
│   ├── gweb.c          # przeglądarka w oknie: adres -> TCP -> tekst + PFS
│   ├── cryptotest.c    # wektory testowe kryptografii z RFC
│   ├── certtest.c      # test parsera X.509 i podpisów RSA
│   ├── threads.c       # wątki w procesie (wspólna przestrzeń adresowa)
│   ├── prio.c          # demo schedulera z priorytetami
│   ├── forktest.c      # fork() i kopiowanie przy zapisie
│   ├── ptree.c         # drzewo procesów: waitany i sieroty
│   ├── sigtest.c       # sygnały: handler, SIG_IGN, Ctrl+C
│   ├── gterm.c         # terminal w oknie GUI (komenda -> plik -> okno)
│   ├── mmaptest.c      # test syscalla mmap (świeża pamięć procesu)
│   └── user.ld         # programy linkowane pod 0x08048000
├── tests/
│   └── fuzz/           # sprawdzanie parserów na uszkodzonych danych
│       ├── harness.c   # parsery ring 3 (X.509, HTML, HTTP, podpisy)
│       ├── kernel_harness.c  # ścieżka pakietów: net.c + tcp.c
│       └── kstub/      # zaślepki sprzętu i schedulera dla kodu jądra
├── scripts/
│   ├── mkinitrd.py     # pakuje ELF-y do obrazu initrd (format PIRD)
│   ├── mkcaroots.py    # buduje wbudowany magazyn zaufania (caroots.h)
│   ├── fuzz_parsers.py # psuje dane i karmi nimi parsery (make fuzz)
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
make fuzz     # parsery na uszkodzonych danych (sanitizery, na hoście)
make iso      # buduje bootowalne ISO z GRUB-em
make run-iso  # uruchamia ISO w QEMU
make clean
```

Po uruchomieniu jądro wypisuje status inicjalizacji każdego podsystemu,
w prawym górnym rogu tyka licznik uptime, a wszystko co wpiszesz na
klawiaturze jest echo'wane na ekran. Logi jądra idą równolegle na port
szeregowy (`make run` + opcja `-serial stdio` w QEMU pokaże je w terminalu).

Testy uruchamiają część instancji z `-append nodesktop` (sprawdzają
konsolę), a część bez niej - żeby przejść ścieżkę, którą widzi użytkownik.

`make test` bootuje jądro w QEMU bez okna i przez QMP sprawdza zawartość
ekranu VGA, tykanie timera oraz echo klawiatury (wstrzykuje klawisze
poleceniem `sendkey`) - można rozwijać system bez wpatrywania się w okno.
Test uruchamia QEMU **dwukrotnie na tym samym obrazie dysku**, więc
weryfikuje też, że pliki zapisane na PFS przeżywają restart.

`make run` podpina też kartę sieciową RTL8139 w trybie użytkownika QEMU
(`-netdev user`). Adres IP nie jest wpisany na sztywno - zaraz po starcie
klient **DHCP** prosi o niego sieć i ustawia maskę, bramę oraz serwer DNS
(w QEMU wychodzi 10.0.2.15/24, brama 10.0.2.2, DNS 10.0.2.3). Gdy w sieci
nie ma serwera DHCP, system po kilku próbach bierze adres awaryjny.
Komendy: `ifconfig`, `arp`, `ping 10.0.2.2`, `dhcp` (ponowna konfiguracja).
ProtonOS odpowiada również na cudze zapytania ARP i na pingi do siebie.

Programy w ring 3 mają własne gniazda UDP: `run udpecho` nasłuchuje na
porcie 7777, `run udptest` wysyła do niego datagram (domyślnie przez pętlę
zwrotną, więc działa i bez sieci), a `run nslookup example.com` pyta
o adres serwer DNS przyznany przez DHCP.

Adresy można podawać po nazwie - `nslookup`, `httpget` i przeglądarka
pytają serwer DNS przyznany przez DHCP (albo wskazany ręcznie: `dns 1.1.1.1`).
Klient HTTP mówi po HTTP/1.1: składa treść dzieloną na kawałki
(`Transfer-Encoding: chunked`) i idzie za przekierowaniami 301/302/307/308,
także względnymi. Adresy `https://` idą przez **TLS 1.3** (X25519 +
ChaCha20-Poly1305) - działa to zarówno w `httpget`, jak i w przeglądarce.
Certyfikat serwera jest **weryfikowany** (podpisy RSA - PKCS#1 v1.5 i PSS -
oraz ECDSA na krzywych P-256 i P-384), gdy na dysku leży magazyn zaufania `roots.der` (sklejone certyfikaty w DER): sprawdzany jest podpis
z `CertificateVerify`, łańcuch podpisów do zaufanego wystawcy, daty
ważności i nazwa hosta. Bez tego pliku połączenie jest tylko szyfrowane -
i program mówi to wprost, a przeglądarka pokazuje `TLS?` zamiast `TLS+`.

TCP działa w obie strony. Jako klient: `run httpget 10.0.2.2 8000 /`
pobiera stronę i wypisuje ją na konsolę, a `run httpget 10.0.2.2 8000 /
strona.htm` zapisuje samą treść na dysk (PFS). Jako serwer: `run tcpserv`
nasłuchuje na porcie 5555, który `make run` wystawia na hosta - można się
połączyć zwykłym `nc localhost 5555`. Stan połączeń pokazuje `netstat`.

Na pulpicie jest też **przeglądarka** (`gweb`, ikona `Web` albo wpis w menu):
wpisujesz adres w postaci `host[:port][/ścieżka]` (albo `https://...`),
a program rozwiązuje nazwę, pobiera stronę, obdziera ją ze znaczników HTML,
rysuje tekst w oknie i zapisuje treść na dysk jako `strona.htm`. Stronę
przewija się strzałkami i PgUp/PgDn (Home wraca na górę), a **odnośniki są
klikalne** - wyróżnione kolorem, klik idzie za adresem z `<a href>`.

`make run` dołącza obraz dysku `build/disk.img` (16 MiB, jako `-hda`); jądro
rozpoznaje jego pojemność przez ATA IDENTIFY i formatuje pusty dysk przy
pierwszym starcie. Pliki zostają na nim między uruchomieniami, a `df`
pokazuje zajętość. PFS3 przydziela bloki po 1 KiB z bitmapy: 64 i-węzły,
nazwy do 63 znaków i do **64 MiB na plik** (12 bloków bezpośrednich, blok
pośredni i blok podwójnie pośredni) - czyli tyle, ile zmieści się na
obrazie dysku.

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
- [x] Scheduler z priorytetami (`sys_setprio`): wyższy wywłaszcza niższy,
      równe round-robin (`run prio`)
- [x] Terminal w oknie GUI (`gterm`) - wpisz komendę, jej wyjście
      pojawia się w oknie (klawiatura -> spawn z przekierowaniem -> odczyt)
- [x] Zmiana rozmiaru / minimalizacja okien: przycisk `[_]` zwija okno
      (zostaje na pasku zadań - klik przywraca), uchwyt w prawym dolnym
      rogu okien jądra zmienia rozmiar przeciąganiem
- [x] Zapis pozycji ikon na dysku: ikony pulpitu można przeciągać myszą,
      a ich rozmieszczenie zapisuje się do pliku PFS (`icons.pos`) i wraca
      po restarcie (kompozytor czyta je przy starcie)
- [x] Karta sieciowa RTL8139 (PCI + DMA: pierścień RX, 4 deskryptory TX,
      przerwanie karty) i stos sieciowy: Ethernet, ARP z tablicą adresów,
      IPv4 z sumą kontrolną, ICMP - `ifconfig`, `arp`, `ping 10.0.2.2`
      (system odpowiada też na cudze ARP i pingi do siebie)
- [x] Walidacja wskaźników z ring 3: każdy adres podany w syscallu musi
      leżeć w przestrzeni użytkownika i być zamapowany w katalogu stron
      procesu (`user_ok`/`user_str`), łańcuchy i struktury kopiowane do
      buforów jądra; zły wskaźnik to błąd -1, a nie zapis w pamięci jądra
      cudzymi rękami (`run evil2`)
- [x] UDP: gniazda w jądrze (porty, kolejki datagramów, suma kontrolna
      z pseudonagłówkiem) i klient DHCP - adres IP, maska, brama i serwer
      DNS przychodzą z sieci zamiast być wpisane na sztywno (`dhcp`);
      `kill` przyjmuje też nazwę procesu, a prompt shella odrysowuje się
      po asynchronicznych komunikatach jądra
- [x] Gniazda UDP w ring 3 (`udp_open`/`udp_send`/`udp_recv`/`udp_close`,
      `netinfo`): gniazdo należy do procesu i zwalnia się po jego śmierci,
      dane idą przez sprawdzone wskaźniki, a pętla zwrotna pozwala dwóm
      procesom gadać po UDP bez sieci (`udpecho` + `udptest`); klient DNS
      w userspace (`nslookup example.com`) i przerywanie oczekiwania
      w jądrze przez `kill`
- [x] TCP: aktywne i pasywne otwarcie (klient i serwer), numery
      sekwencyjne z potwierdzeniami, retransmisje z wątku `tcpd`,
      zamknięcie przez FIN, RST na segment do zamkniętego portu;
      syscalle `tcp_connect`/`listen`/`accept`/`send`/`recv`/`close`,
      klient HTTP (`httpget` - treść ze świata ląduje na PFS) i serwer
      w ring 3 (`tcpserv`), podgląd przez `netstat`
- [x] Przeglądarka w oknie pulpitu (`gweb`): pasek adresu z klawiatury,
      pobranie strony po TCP, wycięcie znaczników HTML, tekst zawijany
      w oknie i zapis treści na PFS - sieć, GUI, klawiatura i dysk
      w jednej aplikacji ring 3
- [x] Start prosto na pulpit graficzny; linia poleceń jądra (Multiboot
      `-append`) z opcją `nodesktop` zostawia konsolę tekstową
- [x] PFS3: przydział bloków z bitmapy zamiast stałych obszarów - 64
      i-węzły, nazwy do 63 znaków, pliki do 268 KiB (bloki bezpośrednie
      + pośredni), zwalnianie bloków przy `rm`/truncate, `df` i rozmiar
      dysku z ATA IDENTIFY; zapis metadanych ograniczony do zmienionego
      bloku i-węzłów i bitmapy
- [x] TCP: aktualizacja okna po odczycie z gniazda - odbiór dużych
      odpowiedzi nie czeka już na probnik nadawcy (100 KiB w kilka sekund)
- [x] Nazwy zamiast adresów: wspólny klient DNS (`udns.h`) w `nslookup`
      i przeglądarce, ręczne wskazanie serwera komendą `dns`
- [x] Klient HTTP/1.1 (`uhttp.h`): kodowanie `chunked` sklejane przez
      granice pakietów, przekierowania (bezwzględne i względne), rozbiór
      `http://host[:port]/ścieżka`; w przeglądarce encje HTML (`&amp;`)
      i bajty UTF-8 zamieniane na czytelne znaki
- [x] Kryptografia pod TLS: SHA-256, HMAC-SHA256, HKDF (w tym
      `HKDF-Expand-Label` z RFC 8446), ChaCha20, Poly1305 i AEAD
      ChaCha20-Poly1305, X25519 - wszystko w userspace i sprawdzane
      wektorami z FIPS 180-4, RFC 4231, RFC 5869, RFC 8439 i RFC 7748
      (`run cryptotest`; jedno mnożenie skalarne X25519 ~20 ms)
- [x] `printk` niepodzielny: komunikat z wątku nie wchodzi już w środek
      linii wypisywanej przez inny wątek albo obsługę przerwania
- [x] Klient **TLS 1.3** (RFC 8446) w ring 3: ClientHello z SNI i
      `key_share`, wspólny sekret X25519, harmonogram kluczy na HKDF,
      zaszyfrowana reszta uzgodnień, sprawdzenie `Finished` serwera,
      własne `Finished`, osobne klucze aplikacyjne i warstwa rekordów
      z ChaCha20-Poly1305. `httpget https://...` i przeglądarka pobierają
      strony po HTTPS; uzgodnienie z OpenSSL-em trwa ~90 ms
      (certyfikat jeszcze niesprawdzany - to następny krok)
- [x] Źródło losowości dla programów (`urand.h`): RTC, licznik timera,
      pid i adresy ze stosu zmieszane przez SHA-256
- [x] Weryfikacja certyfikatów: wielkie liczby w postaci Montgomery'ego,
      podpisy RSA (PKCS#1 v1.5 i PSS), parser X.509/DER, sprawdzanie
      łańcucha do zaufanego wystawcy, dat ważności i nazwy hosta
      (SubjectAltName z wieloznacznikiem); magazyn zaufania czytany
      z pliku `roots.der` na PFS (`run certtest`)
- [x] Stos procesu urósł z 4 KiB do 32 KiB - RSA na 2048 bitach
      potrzebuje kilku kilobajtów na same liczby robocze
- [x] ECDSA na krzywej NIST P-256: arytmetyka punktów w postaci Jacobiego
      na tych samych wielkich liczbach co RSA, weryfikacja podpisów
      w certyfikatach i w `CertificateVerify` (~80 ms na podpis) -
      serwery z kluczem EC przestały być poza zasięgiem
- [x] Wbudowany magazyn zaufania: 14 korzeni publicznych CA w obrazie
      systemu (skrypt `mkcaroots.py` odświeża je z zasobu hosta), do tego
      własne korzenie z pliku `roots.der`; weryfikacja jest domyślnie
      wymagana dla `https://` (`httpget -k` ją pomija)
- [x] Klawisze rozszerzone (strzałki, PgUp/PgDn, Home/End) w sterowniku
      klawiatury - dostarczane programom jako własne kody sterujące
- [x] Przeglądarka: przewijanie strony, odnośniki `<a href>` wyróżnione
      kolorem i klikalne (adresy względne i bezwzględne), rozdzielanie
      tekstu na granicy znaczników blokowych, pasek adresu odświeżany
      po przejściu za odnośnikiem
- [x] PFS3: blok podwójnie pośredni - sufit rozmiaru pliku rośnie
      z 268 KiB do 64 MiB (test pobiera z sieci 400 KB prosto na dysk
      i czyta je z powrotem przez potok)
- [x] ECDSA na krzywej P-384 i skrót SHA-384/SHA-512: arytmetyka krzywych
      sparametryzowana (jeden komplet wzorów dla P-256 i P-384), stałe
      krzywych i tablice SHA-512 generowane, a nie przepisywane ręcznie;
      `CertificateVerify` przyjmuje `ecdsa_secp384r1_sha384`
- [x] Odporność parserów: `uhtml.h` (HTML -> tekst) wydzielony z
      przeglądarki i obwarowany granicami, a `make fuzz` przepuszcza
      przez sanitizery uszkodzone certyfikaty, HTML, odpowiedzi HTTP,
      podpisy DER i **ramki sieciowe trafiające do jądra** (net.c i tcp.c
      kompilowane na hoście z zaślepkami sprzętu)
- [x] Shell jądra jako zwykły wątek: przerwanie klawiatury tylko wrzuca
      znak do bufora kołowego i budzi wątek, więc komenda może spać,
      czekać na sieć albo czytać dysk, a znaki wpisane w tym czasie nie
      giną (`ping` i `dhcp` nie potrzebują już wątków-obejść)
- [x] `fork()` z kopiowaniem przy zapisie: dziecko wraca z syscalla tam,
      gdzie rodzic, i widzi jego pamięć, ale nic nie jest kopiowane -
      obie przestrzenie dostają te same ramki bez prawa zapisu, ramka
      liczy właścicieli, a prywatna kopia strony powstaje dopiero przy
      pierwszym zapisie. Bit WP w CR0 obejmuje tym również zapisy jądra
      (syscall wypełniający bufor procesu), dziecko dziedziczy
      deskryptory i końce potoków (`run forktest`: 256 KiB bufora,
      przyrost RAM po `fork` - 12 KiB)
- [x] `exec()` w miejscu (odpowiednik `execv`): proces podmienia własny
      program, zachowując pid, deskryptory i katalog roboczy - razem
      z `fork()` daje to klasyczną parę uniksową (rozwidl się, w dziecku
      wskocz w nowy program), a `ps` pokazuje już nową nazwę
- [x] Drzewo procesów: każde zadanie zna swojego rodzica (`ps` pokazuje
      kolumnę RODZIC), `waitany` czeka na **dowolne** własne dziecko
      i oddaje je w kolejności kończenia (pid + kod wyjścia), a osierocone
      procesy trafiają pod opiekę dziadka - ich kod wyjścia nie przepada
      razem z rodzicem (`run ptree`)
- [x] Sygnały: `signal()` z własną procedurą obsługi w ring 3, `SIG_IGN`
      i reakcje domyślne, `kill(pid, sygnal)`, `SIGCHLD` do rodzica po
      śmierci dziecka, nieprzechwytywalny `SIGKILL`. Jądro doręcza sygnał
      przy powrocie do ring 3: odkłada ramkę przerwanego stanu na stos
      procesu i skacze w handler, a trampolina z `usys.h` prosi przez
      `sigreturn` o odtworzenie tego stanu - z ramki wracają tylko
      rejestry ogólne, `eip`, `esp` i flagi arytmetyczne, nigdy selektory
      ani IOPL. **Ctrl+C** z klawiatury idzie jako `SIGINT` do procesu
      trzymającego konsolę i przerywa jego oczekiwanie na klawisz;
      `kill P S` w shellu jądra wysyła dowolny sygnał, także pod nazwę
      procesu (`run sigtest`)
