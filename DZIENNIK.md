# ProtonOS - dziennik rozwoju

Szczegółowy opis projektu po polsku: struktura, sposób budowania
i chronologiczna lista tego, co powstawało - **wraz z powodami**.
Prawie każda pozycja tłumaczy, dlaczego coś wygląda tak, a nie inaczej,
i jakie błędy po drodze wyszły; to one tłumaczą kształt kodu lepiej niż
sam kod.

Krótki opis po angielsku jest w [README.md](README.md).

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
│   ├── paging.c        # stronicowanie - identycznościowe całe RAM, COW
│   ├── swap.c          # wymiana stron na dysk (plik swap.sys)
│   ├── ahci.c          # kontroler SATA (AHCI): polecenia przez DMA
│   ├── blk.c           # warstwa blokowa: AHCI albo zapasowo ATA PIO
│   ├── fbcon.c         # konsola tekstowa rysowana na buforze obrazu
│   ├── mb2.c           # Multiboot 2: znaczniki od GRUB-a
│   ├── acpi.c          # tablice ACPI: wyłączanie i restart komputera
│   ├── dynlink.c       # biblioteka współdzielona: symbole i relokacje
│   ├── smpmem.c        # próba zamków: pamięć jądra ze wszystkich rdzeni
│   ├── smpthreads.c    # próba schedulera: wątki jądra na wszystkich rdzeniach
│   ├── biglock.c       # wielki zamek jądra (zagnieżdżalny, jeden na system)
│   ├── usbhc.c         # wybór kontrolera USB (xHCI albo UHCI) + powitanie
│   ├── uhci.c          # kontroler USB 1.1: przesyłki sterujące i przerwaniowe
│   ├── xhci.c          # kontroler USB 3: pierścienie poleceń, zdarzeń, przesyłek
│   ├── usbkbd.c        # klawiatura USB (HID, protokół rozruchowy)
│   ├── heap.c          # sterta jądra: kmalloc/kfree (first-fit + scalanie)
│   ├── serial.c        # COM1 - logi widoczne w QEMU przez -serial stdio
│   ├── printk.c        # formatowane wypisywanie (%s %c %d %u %x %p)
│   ├── vga.c           # 4 konsole wirtualne (VGA albo bufor obrazu)
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
│   ├── cores.c         # rozwidla się i mówi, które rdzenie go prowadziły
│   ├── syscalls.c      # mierzy same wejścia do jądra (milion sys_getpid)
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
│   ├── ptest.py        # wspólna część testu: klasa Qemu, serwery, check
│   ├── run_tests.py    # uruchamia grupy testu równolegle (make test)
│   └── testy/          # grupy: rdzeń, k64, gfx64, sieć, pulpit, ISO…
├── boot64/             # wejście w tryb długi, stuby przerwań, syscall
├── kernel64/           # jądro trybu długiego (x86-64) - tylko to,
│   │                   # co naprawdę zależy od trybu procesora
│   ├── kmain64.c       # kolejność startu i sprawdziany podsystemów
│   ├── paging64.c      # cztery poziomy tablic, NX, przestrzenie procesów
│   ├── task64.c        # zadania, sygnały, deskryptory, katalog roboczy
│   ├── proc64.c        # fork, exec, uruchamianie programów z initrd
│   ├── elf64.c         # ładowanie ELF64 z prawami wprost z segmentów
│   ├── signal64.c      # dostarczanie sygnału przez podmianę ramki
│   ├── pipe64.c        # potoki: pierścień plus liczniki właścicieli
│   ├── fpu64.c         # SSE i stan jednostki zmiennoprzecinkowej
│   ├── syscall64.c     # wszystkie wywołania systemowe
│   ├── idt64.c         # bramki 16-bajtowe, IST
│   ├── gdt64.c         # segmenty w kolejności wymaganej przez sysret
│   ├── keyboard64.c    # IRQ 1 i kolejka znaków
│   ├── shell64.c       # shell jako wątek jądra
│   ├── isr64.c         # wyjątki: NX, brak strony, śmierć programu
│   └── compat64.c      # słownik zadań dla kodu wspólnego (net, tcp)
├── user64/             # programy trybu długiego (ELF64 w initrd64)
│   ├── usys64.h        # syscalle przez instrukcję syscall
│   ├── hello64.c       # pierwszy program 64-bitowy ładowany z pliku
│   ├── nx64.c          # próbuje wykonać własne dane - ginie na NX
│   ├── wild64.c        # sięga po pamięć jądra - ginie na braku USER
│   ├── sep64.c         # dwie kopie naraz, ten sam adres, osobna pamięć
│   ├── fork64.c        # proces powiela sam siebie i sprawdza kopię
│   ├── spawn64.c       # fork + exec + wait - tak działa każdy shell
│   ├── sigtest64.c     # przechwytuje Ctrl+C i wraca w przerwane miejsce
│   ├── spin64.c        # pętla bez końca - do zatrzymania z zewnątrz
│   ├── ush64.c         # shell w ring 3: read + fork + exec + wait
│   ├── fileio64.c      # plik od strony programu: open, write, read
│   ├── uecho64.c       # wypisuje swoje argumenty
│   ├── ucat64.c        # 'cat' jako zwykły program, nie wbudowany
│   ├── heap64.c        # sterta: prosi o 4 MiB, dotyka 4 stron
│   ├── fpu64test.c     # liczby zmiennoprzecinkowe w dwóch procesach
│   ├── uls64.c         # 'ls' jako program: pyta o wpisy po kolei
│   ├── uupper64.c      # filtr: czyta wejście, pisze wielkimi literami
│   └── unet64.c        # sieć z ring 3: DHCP, UDP i TCP przez syscalle
│   └── user64.ld       # sekcje wyrównane do stron (prawa dostępu!)
├── linker.ld           # skrypt linkera - jądro ładowane pod adres 1 MiB
├── linker64.ld         # to samo dla jądra w trybie długim
├── grub.cfg            # konfiguracja GRUB-a dla obrazu ISO (BIOS)
├── grub64.cfg          # konfiguracja dla płyty 64-bitowej (BIOS + UEFI)
└── Makefile
```

## Wymagania

- `gcc` z obsługą `-m32` (na Ubuntu/Debianie: `sudo apt install gcc-multilib`)
- `make`
- do uruchomienia: `qemu-system-i386` (pakiet `qemu-system-x86`)
- do zbudowania ISO: `grub-mkrescue` + `xorriso` (pakiety `grub-pc-bin`, `xorriso`)
- do płyty i rozruchu przez UEFI: `grub-efi-amd64-bin`, `mtools`, `ovmf`
  oraz `qemu-system-x86_64`

## Budowanie i uruchamianie

```sh
make          # buduje build/protonos.bin
make run      # uruchamia jądro w QEMU (najprostsza droga)
make test     # automatyczny test e2e w QEMU (grupy równolegle)
make test T=k64      # jedna grupa - szybka pętla przy pracy (~80 s)
make test J=1        # grupy jedna po drugiej, do debugowania
make fuzz     # parsery na uszkodzonych danych (sanitizery, na hoście)
make iso      # buduje bootowalne ISO z GRUB-em (menu: pulpit / konsola)
make run-iso  # uruchamia ISO w QEMU - z dyskiem i kartą sieciową
make kernel64 # buduje build/protonos64.bin + initrd64 (tryb długi)
make iso64    # płyta hybrydowa z jądrem 64-bitowym (BIOS + UEFI)
make run-iso64-uefi  # uruchamia tę płytę pod firmware UEFI (OVMF)
make clean
```

Po uruchomieniu jądro wypisuje status inicjalizacji każdego podsystemu,
w prawym górnym rogu tyka licznik uptime, a wszystko co wpiszesz na
klawiaturze jest echo'wane na ekran. Logi jądra idą równolegle na port
szeregowy (`make run` + opcja `-serial stdio` w QEMU pokaże je w terminalu).

Testy uruchamiają część instancji z `-append nodesktop` (sprawdzają
konsolę), a część bez niej - żeby przejść ścieżkę, którą widzi użytkownik.

### Jądro 64-bitowe

```sh
make kernel64        # build/protonos64.bin + initrd64.img
make run-iso64-uefi  # płyta hybrydowa pod firmware UEFI (OVMF)
make test T=k64      # sprawdzenia tego jądra (~80 s)
```

Po starcie wita shell `proton64>`. Polecenia: `help`, `ps`, `mem`,
`uptime`, `clear`, `ls` (initrd), `fls`, `pwd`, `cd`, `mkdir`, `cat`,
`write`, `rm`, `disk`, `gfx`, `ifconfig`, `arp`, `ping` oraz
`run <program> [argumenty]`.

W initrd leżą programy: `hello64`, `ush64` (shell w ring 3 - `fork`,
`exec`, `wait`, potoki przez `|`), `uecho64`, `ucat64`, `uls64`,
`uupper64`, `unet64`, `heap64`, `fileio64`, `fpu64test`, `sep64`,
`fork64`, `spawn64` oraz trzy, które **mają zginąć**: `nx64` próbuje
wykonać własne dane, `wild64` sięga po pamięć jądra, a `spin64` kręci
się bez końca, dopóki nie dostanie Ctrl+C.

### Jądro 32-bitowe

Wszystko poniżej opisuje jądro 32-bitowe - ono ma pulpit graficzny,
TLS, przeglądarkę i wymianę stron na dysk.

`make test` uruchamia jądro w QEMU bez okna i przez QMP sprawdza
zawartość ekranu, tykanie timera oraz echo klawiatury (wstrzykuje
klawisze poleceniem `sendkey`) - można rozwijać system bez wpatrywania
się w okno. Ponad czterysta sprawdzeń podzielonych jest na **grupy
chodzące równolegle**, bo każda potrzebuje własnej maszyny, a te
większość czasu i tak czekają: na rozruch, na odpowiedź, na
przerysowanie ekranu. Grupy leżą w `scripts/tests/`, jedna na plik,
a `T=` wybiera pojedynczą - przy pracy nad jednym podsystemem to
różnica między osiemdziesięcioma sekundami a pięcioma minutami.

Kilka grup uruchamia QEMU **dwukrotnie na tym samym obrazie dysku**,
więc weryfikują też, że pliki zapisane na PFS przeżywają restart - i to
jest jedyny rozstrzygający dowód, że leżą na dysku, a nie w pamięci.

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

**Jądro 32-bitowe.** GRUB albo QEMU znajduje w pierwszych 8 KiB obrazu
nagłówek Multiboot (`boot/boot.S`), ładuje jądro pod adres 1 MiB
i skacze do `_start` już w trybie chronionym. Stamtąd `kmain()` stawia
po kolei GDT, IDT, PIC i PIT, buduje mapę pamięci fizycznej ze
znacznika od loadera, włącza stronicowanie, a na końcu uruchamia
scheduler i shell.

**Jądro 64-bitowe.** Multiboot 2 zostawia procesor w trybie chronionym,
więc przejście w tryb długi trzeba wykonać samemu i w ściśle określonej
kolejności (`boot64/boot64.S`): najpierw tablice stron, bo stronicowanie
jest w trybie długim **obowiązkowe**, potem PAE, potem bit LME w EFER,
potem samo stronicowanie - i dopiero skok daleki do segmentu z bitem L
przełącza procesor naprawdę. Że się udało, mówi sam procesor: bit LMA
w EFER ustawia on, nie my.

**Rozruch przez UEFI.** `make iso64` składa płytę **hybrydową**:
z jednej strony sektor rozruchowy dla BIOS-u, z drugiej partycja FAT
z programem `BOOTX64.EFI`. Ta sama płyta wstaje więc i na starej, i na
nowej maszynie. Pod UEFI nie ma pamięci znaków spod `0xB8000` - jedyne,
co dostajemy od firmware, to bufor obrazu, więc znaki rysuje sobie
`fbcon.c` własną czcionką, piksel po pikselu.

**Programy.** Leżą w initrd jako ELF-y i jądro nie wie o nich nic ponad
to, co przeczyta z nagłówka: gdzie zacząć, co załadować i z jakimi
prawami. Prawa biorą się wprost z segmentów, więc kod jest tylko do
odczytu, a dane i stos dostają zakaz wykonywania - i pilnuje tego
procesor, nie jądro.

## Dziennik rozwoju

Lista jest chronologiczna i opisuje **dlaczego** coś wygląda tak, a nie
inaczej - łącznie z błędami, które po drodze wyszły, bo one tłumaczą
kształt kodu lepiej niż sam kod. Wszystko poniżej działa i jest
sprawdzane przez `make test`.


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
- [x] Cała wykryta pamięć pod zarządem: mapowanie identycznościowe nie
      kończy się już na 64 MiB, tylko sięga szczytu RAM-u (do 512 MiB).
      Pierwsze 64 MiB opisują tablice statyczne - jądro musi widzieć
      pamięć, zanim poprosi PMM o pierwszą ramkę - a dalsze tablice
      biorą się już z PMM, więc granicę wyznacza ilość RAM-u, a nie
      rozmiar tablicy w `.bss`. Przestrzeń użytkownika przeprowadziła
      się z `0x08048000` pod **1 GiB**, żeby zejść mapowaniu z drogi:
      jeden proces potrafi teraz zająć 80 MiB, czyli więcej, niż cały
      system widział wcześniej
- [x] Stronicowanie na żądanie: `sbrk` i `mmap` nie przydzielają już ani
      jednej ramki - obszar jest procesowi obiecany, a strony powstają
      dopiero przy pierwszym dotknięciu (prośba o 64 MiB kosztuje 0 KiB,
      zapis 1 MiB kosztuje dokładnie 1024 KiB). Walidacja wskaźników
      w syscallach umie taką stronę urzeczywistnić, więc świeżo
      przydzielony, nietknięty bufor nie wygląda z jądra jak zły
      wskaźnik (`run lazymem`)
- [x] Naprawiony stary błąd rozpoznawania tablic jądra: były rozpoznawane
      po **pełnej wartości** wpisu w katalogu stron, a procesor sam
      ustawia w nim bit „odwiedzony" - ta sama tablica miała więc po
      chwili różne wartości w różnych katalogach i sprzątanie procesu
      potrafiło zwolnić tablicę **jądra**, oddając kawałek własnego
      obrazu jądra do puli wolnych ramek. Teraz rozstrzyga bit USER,
      którego tablice jądra nie mają; w PMM stoi dodatkowo bezpiecznik
      odmawiający zwolnienia ramki z obrazu jądra
- [x] Wymiana stron na dysk: gdy zabraknie ramek, jądro wyrzuca na dysk
      strony leżące odłogiem (algorytm drugiej szansy - bit „odwiedzona"
      ustawiany przez procesor) i sprowadza je z powrotem przy pierwszym
      dotknięciu. Miejsce rezerwuje przez system plików (`swap.sys`,
      widoczny w `fls` i `df`), ale same strony wędrują tam wprost przez
      ATA, z pominięciem kodu PFS - wymiana potrafi ruszyć w środku
      operacji na plikach, a PFS ma jeden wspólny bufor na blok. Wpis
      wyrzuconej strony trzyma numer miejsca w górnych bitach, `mem`
      pokazuje bilans, a `run swaptest 18` mieści 18 MiB na maszynie
      z 16 MiB RAM-u
- [x] Konsole wirtualne (Alt+F1..F4): każda ma własny ekran 80x25, własny
      shell, własny bufor klawiszy i własnego właściciela klawiatury.
      To, na którą konsolę idzie wypis, wynika z **zadania**, które pisze
      (jak uniksowy terminal kontrolujący) - program uruchomiony na
      drugiej konsoli pisze na niej dalej, kiedy patrzymy na pierwszą,
      a `Ctrl+C` trafia w proces trzymający tę konsolę, którą widać
- [x] Sterownik **AHCI (SATA)** i warstwa urządzenia blokowego: kontroler
      znajdowany po klasie PCI (0x01/0x06/0x01), polecenia składane
      w pamięci (lista poleceń, tablica polecenia z ramką FIS i PRDT),
      dane przenoszone przez **DMA** zamiast przepisywania słowo po
      słowie. System plików i wymiana stron nie wiedzą już, jaki dysk
      mają pod spodem - wybór zapada raz przy starcie, ATA PIO zostaje
      drogą zapasową. To warunek zobaczenia dysku na nowszej płycie,
      która nie ma trybu zgodności; przy okazji ta sama wymiana 18 MiB
      na maszynie z 16 MiB RAM-u trwa 4,3 s zamiast 8,3 s
- [x] **Klawiatura USB** (HID, protokół rozruchowy) na kontrolerze UHCI:
      rozpoznanie urządzenia na magistrali (reset portu, deskryptory,
      nadanie adresu, odszukanie interfejsu klasy HID i jego punktu
      końcowego), przełączenie w protokół rozruchowy i tłumaczenie
      ośmiobajtowych raportów na klawisze - z modyfikatorami, strzałkami,
      Ctrl+C i Alt+F1..F4. Dla reszty systemu nie ma różnicy, która
      klawiatura jest podpięta: znaki idą tą samą drogą co z PS/2.
      Sprawdzone też przy **wyłączonej obsłudze PS/2** - system dawał się
      wtedy obsługiwać wyłącznie przez USB
- [x] Skanowanie PCI obejmuje wszystkie funkcje urządzenia, nie tylko
      zerową - kontrolery wbudowane w mostek siedzą na funkcji 2 albo 3
      i przeglądanie samych zerowych nie znajdowało ich nigdy
- [x] **ACPI**: wyłączanie i restart komputera. Wskaźnik RSDP znajdowany
      w pamięci poniżej megabajta, przez niego spis tablic (RSDT), a w nim
      tablica zasilania (FADT). Sama wartość, którą trzeba wpisać, żeby
      komputer zgasł, siedzi jeszcze głębiej - w bajtkodzie AML (DSDT),
      pod nazwą `_S5_`; pełnego interpretera nie ma, ten jeden obiekt
      czytamy wprost. Komendy `poweroff` i `reboot` (restart przez
      rejestr resetu ACPI, a gdy firmware go nie podaje - impulsem przez
      kontroler klawiatury). Tablice leżą zwykle poza pamięcią zarządzaną
      przez jądro, więc każda jest doklejana do mapowania
      identycznościowego przed odczytem
- [x] **Multiboot 2**: obraz jądra nosi teraz oba nagłówki naraz. QEMU
      z opcją `-kernel` rozumie tylko starszy, GRUB woli nowszy - loader
      wybiera ten, który zna, i mówi w `magic`, którego użył. Informacje
      z nowszej postaci (linia poleceń, moduły, mapa pamięci) są
      przepisywane do tej samej struktury, której reszta jądra używa od
      początku, więc ani PMM, ani initrd nie widzą różnicy. Nowość, która
      naprawdę coś zmienia: **wskaźnik na tablice ACPI podaje loader** -
      pod UEFI nie ma pamięci BIOS-u, w której dałoby się go poszukać
      samemu. To pierwszy krok w stronę rozruchu przez UEFI
- [x] **Konsola na buforze obrazu**: nagłówek Multiboot 2 prosi loader
      o tryb graficzny, a jądro rysuje w nim znaki samo - tą samą
      czcionką 8x8, co reszta grafiki, w kolorach trybu tekstowego
      przeliczonych na RGB. Bufor znaków pod `0xB8000` przestaje być
      potrzebny, a to warunek działania pod UEFI, gdzie trybu tekstowego
      VGA nie ma w ogóle. Cztery konsole wirtualne działają bez zmian -
      różni się tylko to, dokąd trafia gotowa komórka. Pulpit korzysta
      z tego samego bufora: `video_init_vesa` sięga po interfejs
      Bochs/QEMU, który istnieje wyłącznie na maszynie wirtualnej, więc
      gdy tryb ustawił loader, zostajemy przy nim (i dostajemy 1024x768
      zamiast 640x480)
- [x] **Pierwszy krok na x86-64**: drugi obraz jądra (`make kernel64`)
      wchodzi w tryb długi. GRUB zostawia nas w trybie chronionym 32-bit,
      więc `boot64/boot64.S` sam układa tablice stron (pierwsze 4 GiB
      stronami po 2 MiB - nie dla pamięci, tylko dlatego, że wysoko leżą
      rejestry urządzeń i bufor obrazu), włącza PAE, ustawia bit LME
      w EFER, włącza stronicowanie i skacze daleko do segmentu z bitem L.
      Procesor sam potwierdza wynik: bit LMA w EFER ustawia on, nie my.
      Najważniejsze jest to, czego w tym obrazie **nie ma** - żadnej
      kopii kodu: wypisywanie, konsola na buforze obrazu, znaczniki
      Multiboota 2 i obsługa łańcuchów to te same pliki kompilowane
      drugi raz, a cała różnica między trybami zamyka się w `io.h`
      i w kilku rzutowaniach przez `uintptr_t`
- [x] Przerwania i wyjątki w trybie długim: własna tablica IDT (wpis ma
      teraz 16 bajtów, bo adres procedury jest 64-bitowy i nie mieści
      się w jednym kawałku), stuby odkładające szesnaście rejestrów
      ręcznie - `pushal` w trybie długim już nie istnieje - i powrót
      przez `iretq`. `struct regs` ma od teraz wersję zależną od trybu,
      dzięki czemu sterowniki PIC i PIT kompilują się w obu jądrach bez
      jednej zmiany. Dowód: pułapka `int3` wraca, nie zatrzymując
      systemu, a timer tyka sam z siebie (1 -> 201 tyknięć)
- [x] Naprawiony błąd w sterowniku USB: deskryptor odczytu był przy
      braku danych **porzucany**, a kontroler potrafił dokończyć
      przesyłkę już po tej decyzji - raport wpadał do bufora, którego
      nikt nie czytał, a urządzenie przestawiało przełącznik danych
      i przestawał się on zgadzać z naszym już na zawsze. Objawiało się
      to tak, jak takie błędy zwykle: działało, działało, po czym
      klawiatura milkła. Teraz deskryptor jest uzbrojony bez przerwy,
      a kontroler ponawia go sam co ramkę
- [x] Pamięć w trybie długim: PMM i sterta jądra przeniosły się **bez
      jednej zmiany logiki** (bitmapa ramek nie zależy od trybu -
      wystarczyło kilka rzutowań przez `uintptr_t`), a stronicowanie
      napisane od nowa: cztery poziomy zamiast dwóch, wpisy 64-bitowe,
      tablice budowane z ramek od PMM zamiast leżeć na sztywno
      w obrazie jądra. Doszedł bit, którego 32-bitowe stronicowanie nie
      miało - **zakaz wykonywania**: bufor obrazu i pamięć danych są
      oznaczone tak, że procesor odmówi wykonania czegokolwiek, co w nich
      leży. Sprawdzian: strona 4 KiB wstawiona **osiem gigabajtów** ponad
      obszar opisany identycznościowo, zapisana i odczytana przez pełną
      czterostopniową ścieżkę
- [x] Wątki jądra w trybie długim: przełączanie kontekstu zapisuje sześć
      rejestrów zamiast czterech i po osiem bajtów każdy, reszta stanu
      wątku zostaje na jego własnym stosie - tak samo jak w jądrze
      32-bitowym. Doszedł osobny **wątek bezczynności**: dzięki niemu
      przypadek „nikt nie jest gotowy" po prostu nie występuje,
      a scheduler nie musi czekać na przerwanie w środku własnego
      wywołania (co przy wywołaniu z obsługi timera zagnieżdżałoby się
      bez końca). Sprawdzian jest arytmetyczny: przez 600 ms wątek
      śpiący po 20 ms budzi się dokładnie 30 razy, a ten po 50 ms - 12
- [x] **Ring 3 i instrukcja `syscall`** w trybie długim - port jądra
      domyka się. GDT z segmentami użytkownika (ich kolejność nie jest
      dowolna: `sysret` składa selektory z jednej wartości w rejestrze
      STAR, więc dane muszą leżeć **przed** kodem) i zadanie TSS, które
      w trybie długim ma już tylko jedno zadanie - powiedzieć, na który
      stos przejść przy przerwaniu z ring 3. Wejście do jądra nie idzie
      przez bramkę przerwania, tylko przez `syscall`: procesor bierze
      adres z rejestru modelu, a adres powrotu zostawia w RCX. Cena jest
      taka, że `syscall` **nie przełącza stosu** - własny trzeba podać
      z góry, i to dwa razy, bo przerwanie bierze go skądinąd (z TSS)
      niż wywołanie systemowe. Dowód: program mówi jądru swój selektor
      kodu, którego zna tylko procesor - `cs=23`, pierścień 3
- [x] **Rozruch przez UEFI** - jądro 64-bitowe startuje na maszynie bez
      BIOS-u. `make iso64` składa płytę **hybrydową**: z jednej strony
      sektor rozruchowy dla BIOS-u, z drugiej partycja FAT z programem
      `BOOTX64.EFI`, którego szuka firmware. Ta sama płyta wstaje więc
      i na starej, i na nowej maszynie. Dwie rzeczy trzeba było przy tym
      poprawić. Po pierwsze GRUB pod UEFI nie ustawia sam trybu
      graficznego i kończy się błędem „no suitable video mode found",
      dopóki nie każe mu się wczytać sterowników obrazu (`insmod
      all_video`) i zostawić tryb ustawiony przez firmware
      (`set gfxpayload=keep`) - a znacznik Multiboota 2 musi prosić
      o rozdzielczość „dowolną" (0x0x0), bo firmware i tak da to, co ma.
      Po drugie okazało się, że OVMF podaje bufor obrazu **24-bitowy**,
      a konsola umiała rysować tylko po 32 bity na piksel i po cichu
      odmawiała. Różnica jest drobna, ale zasadnicza: przy 24 bitach
      piksel nie ma bajtu dopełnienia, więc zapis całym słowem zadeptuje
      sąsiada - trzeba pisać trzema bajtami. Teraz `fbcon` obsługuje oba
      formaty i pod UEFI widać na ekranie dokładnie ten sam log, co pod
      BIOS-em, narysowany naszą czcionką piksel po pikselu, bez
      pomocy firmware i bez pamięci znaków spod 0xB8000, której tam
      w ogóle nie ma
- [x] **Programy z pliku w trybie długim**: jądro 64-bitowe uruchamia
      ELF-y wczytane z initrd, a nie własny kawałek siebie. Do tej pory
      w ring 3 działało kilkanaście bajtów wpisanych na stałe w sekcję
      `.rodata` jądra - dowód, że przejście do pierścienia 3 działa,
      ale niczego więcej. Teraz `kernel64/elf64.c` czyta nagłówek ELF64
      i wszystkiego dowiaduje się z pliku: gdzie zacząć, co załadować
      i **z jakimi prawami**. Prawa biorą się wprost z segmentów, więc
      kod programu jest tylko do odczytu, a jego dane i stos dostają
      zakaz wykonywania - pilnuje tego procesor, nie jądro. Doszła
      biblioteczka `user64/usys64.h`: wejście do jądra idzie instrukcją
      `syscall` (numer w RAX, argumenty w RDI/RSI, wynik w RAX,
      a RCX i R11 giną, bo procesor zostawia w nich adres powrotu
      i flagi). Wskaźnikowi z ring 3 jądro nie wierzy na słowo -
      `paging64_user_range` sprawdza bit USER na **wszystkich czterech**
      poziomach tablic, dokładnie tak, jak liczy dostęp procesor;
      inaczej wystarczyłoby podać adres pamięci jądra, żeby kazać mu ją
      sobie wypisać. Program `hello64` pisze, liczy, usypia się, pyta
      o licznik tyknięć i kończy z kodem 7 - i to wszystko przechodzi
      tak samo pod BIOS-em, jak i pod UEFI
- [x] **Ochrona pamięci, która naprawdę broni** - i przeżywa własny
      dowód. Wyjątek przychodzący z pierścienia 3 nie zatrzymuje już
      systemu: jądro wypisuje, co się stało, zabija winne zadanie
      i pracuje dalej. O tym, skąd przyszedł wyjątek, mówią dwa
      najniższe bity selektora kodu w ramce przerwania - odkłada je tam
      procesor, więc program nie ma jak skłamać. Handler z takiego
      wyjątku nie wraca: `schedule()` przełącza się na inny wątek,
      a ramka przerwania ginie razem ze stosem martwego zadania.
      Dopiero to pozwoliło **sprawdzić** ochronę zamiast wierzyć jej na
      słowo. `nx64` wpisuje instrukcję do swojej tablicy w `.bss`
      i skacze pod jej adres - zapis przechodzi, skok kończy się błędem
      strony `err=0x15` (strona obecna, dostęp z ring 3, pobranie
      instrukcji), czyli zadziałał bit NX, a nie zwykły brak mapowania.
      `wild64` sięga po pamięć jądra dwiema drogami: najpierw prosi
      o nią samo jądro (`paging64_user_range` odmawia), potem sięga
      sam - `err=0x05`, odczyt strony bez bitu USER. Po obu zgonach
      wątki jądra liczą dalej, i to jest ta asercja, o którą naprawdę
      chodzi
- [x] Naprawiony błąd w umowie o wywołaniach systemowych: jądro
      niszczyło RDX, RSI, RDI i R8-R10, bo tak wolno funkcji C - ale
      program o tym nie wiedział. Kompilator trzymał sobie stałą w RDX
      przez całe wywołanie i użył jej potem jako numeru następnego,
      trafiając na śmieć po jądrze (objawiło się jako „nieznane
      wywołanie systemowe 1016"). Teraz stub odkłada te rejestry
      u siebie: poza RAX z wynikiem oraz RCX i R11, które zabiera sam
      procesor, wszystko wraca nietknięte. Taniej raz odłożyć sześć
      rejestrów niż w każdym programie pamiętać o sześciu pułapkach
- [x] **Osobne przestrzenie adresowe procesów** w trybie długim - dwa
      programy mogą wreszcie działać naraz, każdy z własną pamięcią pod
      tym samym adresem. Podział wyszedł sam z rozkładu adresów:
      najwyższy poziom tablic dzieli przestrzeń na kawałki po 512 GiB,
      drugi - po gigabajcie, a cała pamięć fizyczna, jądro i bufor
      obrazu mieszczą się w innych gigabajtach niż ten, w którym leżą
      programy. Proces dostaje więc własną tablicę drugiego poziomu
      z wyczyszczonym jednym wpisem, a **reszta zostaje wspólna** - te
      same ramki, co u jądra. Mapowanie dołożone przez jądro po starcie
      widać natychmiast we wszystkich przestrzeniach naraz, a przy
      przełączaniu zadań CR3 przeładowuje się tylko wtedy, gdy naprawdę
      się zmienia (każde przeładowanie wyrzuca cały bufor tłumaczeń
      adresów). Zadanie nosi teraz też swój stos jądra - `syscall`
      i przerwanie z ring 3 biorą go z dwóch różnych miejsc, więc oba
      trzeba ustawić przy każdym przełączeniu. Umierające zadanie
      najpierw schodzi na przestrzeń jądra, a dopiero potem oddaje
      swoją - trudno zwalniać tablice, na których się właśnie stoi.
      Dowód: `sep64` uruchomiony w dwóch kopiach naraz pisze pod ten
      sam adres `40002060`, każda swoją liczbę, i po oddaniu procesora
      drugiej kopii **obie znajdują swoją nietkniętą**; po ich śmierci
      licznik zajętych ramek wraca dokładnie tam, gdzie był
- [x] Przy okazji: `iretq` do ring 3 zeruje wszystkie rejestry poza
      tymi, które program ma dostać. Jądro zostawiało w nich adresy
      własnych struktur, a pierścień 3 nie ma powodu ich nawet znać.
      Program dostaje w RDI jedną liczbę od jądra - to cały „argv" na
      dzisiaj, ale wystarcza, żeby dwie kopie tego samego pliku
      wiedziały, która jest którą
- [x] **`fork()` w trybie długim** - drugi proces bierze się już nie
      z decyzji jądra, tylko z decyzji samego programu. Żeby to było
      możliwe, stub wywołań systemowych odkłada teraz **dokładnie taką
      ramkę, jaką odkłada przerwanie**: instrukcja `syscall` sama nie
      odkłada niczego (adres powrotu zostawia w RCX, flagi w R11), więc
      te pięć słów dopisujemy ręcznie. Kosztuje to kilkanaście
      instrukcji i powrót przez `iretq` zamiast `sysretq`, a daje jedną
      rzecz: jądro widzi wywołanie systemowe i przerwanie tak samo.
      `fork` sprowadza się wtedy do skopiowania ramki i kazania nowemu
      zadaniu wrócić nią do ring 3 - z zerem w RAX. Żadne rejestry nie
      są odtwarzane „ręcznie": robi to ta sama ścieżka, którą wraca
      każde inne wywołanie. Pamięć na razie kopiujemy wprost, strona po
      stronie (kopiowanie przy zapisie przyjdzie po tym, jak samo
      powielanie zacznie działać). Dowód: `fork64` wpisuje 100, powiela
      się, dziecko wpisuje 200 - i rodzic po przebudzeniu **nadal ma
      swoje 100**
- [x] Naprawiony błąd, który czekał na drugi proces: stos programu był
      zapamiętywany w jednej globalnej komórce na czas **całego**
      wywołania systemowego. Wystarczyło, żeby wywołanie uśpiło
      zadanie, a drugi proces wszedł w tym czasie do jądra - i pierwszy
      wracał do ring 3 na cudzy stos. Nie wybuchło wcześniej tylko
      dlatego, że oba procesy miały stos pod tym samym adresem
      wirtualnym, więc podmieniona wartość była przypadkiem ta sama.
      Teraz wartość ląduje na stosie jądra tego konkretnego zadania
      i nikt inny jej nie widzi
- [x] **Kopiowanie przy zapisie** w trybie długim - `fork` przestał
      kopiować pamięć. Rodzic i dziecko dostają te same ramki, a strona,
      która była zapisywalna, traci u **obu** prawo zapisu i dostaje
      znacznik w bicie 9 (procesor zostawia go do dyspozycji systemu).
      Przy pierwszej próbie zapisu procesor zgłasza błąd strony, a jądro
      dopiero wtedy robi kopię - i to tylko tej jednej strony. Strony
      tylko do odczytu (kod, dane stałe) nie potrzebują nawet tego:
      skoro nikt ich nie zmieni, zostają wspólne na zawsze, a o tym,
      kiedy wrócą do puli, decyduje licznik właścicieli w PMM. Doszedł
      bit ochrony zapisu w CR0 (WP): bez niego jądro może pisać po
      stronach oznaczonych jako tylko do odczytu, więc strona
      współdzielona zostałaby po cichu nadpisana zamiast rozdzielona.
      Rachunek z prawdziwego biegu: `fork64` ma siedem stron (kod, dane
      stałe, dane, cztery strony stosu), więc kopiowanie wprost
      kosztowałoby siedem ramek plus tablice - **z dzieleniem stron fork
      bierze pięć ramek, wszystkie na tablice, a rozdzielają się dwie
      strony**: ta, którą dziecko zapisało, i jedna strona stosu.
      Semantyka nie zmieniła się o włos: rodzic po przebudzeniu nadal
      ma swoje 100
- [x] **`exec` i `wait`** - model procesów się domyka. Program potrafi
      uruchomić **inny** program i doczekać jego końca, czyli dokładnie
      to, co robi każdy shell na świecie. `exec` nie potrzebował żadnej
      osobnej ścieżki powrotu: jesteśmy w obsłudze wywołania
      systemowego, a ta wraca do ring 3 przez ramkę, którą dostała -
      wystarczy ją **przepisać** (nowy adres startu, nowy stos,
      wyzerowane rejestry) i pozwolić wrócić. Program po drugiej
      stronie nie ma jak zauważyć, że wrócił gdzie indziej, niż wołał.
      Kolejność jest tu jedyna możliwa: nazwę trzeba przepisać do jądra,
      **zanim** zniknie pamięć, w której leży, a starą przestrzeń
      zwolnić dopiero wtedy, gdy procesor z niej zejdzie. `wait`
      opiera się na śladzie po zakończonym zadaniu: samo zadanie znika
      z listy przy najbliższym przełączeniu, ale rodzic wcale nie musi
      zdążyć zapytać, zanim dziecko się skończy - zostaje więc po nim
      jeden wpis do odebrania. Gdy dzieci nie ma wcale, `wait` mówi to
      wprost, zamiast czekać w nieskończoność. Dowód: `spawn64` powiela
      się, dziecko podmienia swój obraz na `hello64`, a rodzic odbiera
      jego **siódemkę** - i to ta siódemka jest dowodem, że doczekał się
      tego dziecka, a nie dowolnego zakończonego zadania
- [x] **Klawiatura i shell w trybie długim** - z systemem da się wreszcie
      rozmawiać, zamiast oglądać scenariusz wpisany w `kmain64`.
      Tłumaczenie scancode'ów wyprowadziłem ze sterownika do
      `kernel/kbdmap.c`: to jedyna jego część, która nie zależy od
      niczego - ani od konsol wirtualnych, ani od zadań, ani od trybu
      procesora - więc ten sam automat obsługuje klawiaturę w obu
      jądrach, a decyzja, **dokąd** trafi gotowy znak, zostaje po
      stronie sterownika. Stan musi w nim siedzieć, bo jedno wciśnięcie
      klawisza rozkłada się na kilka bajtów: prefiks `0xE0`, osobne kody
      wciśnięcia i puszczenia modyfikatorów. `kernel64/shell64.c` to
      zwykły wątek jądra z pętlą - poleceń ma tyle, ile jądro naprawdę
      umie: `help`, `ps`, `mem`, `ls`, `uptime`, `clear` i `run
      <program>`. `run` **czeka** na swoje dziecko przez `task64_wait`
      i podaje jego kod wyjścia, dokładnie tak, jak zrobiłby to program
      przez wywołanie systemowe - przez ten czas shell nie odpowiada
      i tak ma być. Przy okazji uruchamianie programów przeniosło się
      z `kmain64` do `proc64_spawn`, gdzie jego miejsce: jądro przy
      starcie i shell na żądanie idą teraz dokładnie tą samą drogą
- [x] **Sygnały i Ctrl+C** w trybie długim - program w pętli bez końca
      przestał być nie do zatrzymania. Sygnał nie jest wiadomością do
      przeczytania, tylko przerwaniem toku programu, i jądro nie może go
      dostarczyć w chwili wysłania: adresat jest wtedy gdzie indziej -
      może spać, może siedzieć w środku wywołania systemowego. Czeka
      więc przy zadaniu do **najbliższego powrotu do pierścienia 3**,
      bo tylko wtedy ramka opisuje program, a nie jądro. Sprawdzamy to
      w dwóch miejscach: przy powrocie z wywołania systemowego i przy
      powrocie z przerwania - to drugie działa nawet wtedy, gdy program
      o nic jądra nie prosi. Samo dostarczenie to znowu operacja na
      ramce: odłożyć ją na stosie programu, przestawić na procedurę
      obsługi i wsunąć pod nią adres powrotu. Adres musi przyjść **od
      programu**, bo jedyne strony wykonywalne, jakie proces ma, to jego
      własny kod - stos jest nieskutecznie wykonywalny z założenia (NX).
      Ramka wraca ze stosu użytkownika, więc program mógłby ją podmienić:
      rejestry i adres powrotu są jego sprawą, ale segmenty i flagi nie,
      bo tędy wchodzi się w jądro. Dowód: `spin64` bez własnej procedury
      ginie od Ctrl+C i shell odzyskuje konsolę, a `sigtest64` sygnał
      **przechwytuje** - jego procedura wypisuje się i wraca, a program
      kończy się dalej normalnie, kodem zero
- [x] **Wejście dla programów i shell w przestrzeni użytkownika**.
      `read` blokuje do pierwszego znaku, a potem oddaje wszystko, co
      zdążyło się uzbierać - tak jak każde sensowne wejście: czeka się
      na cokolwiek, a nie na pełny bufor. Sygnał przerywa to czekanie
      i wraca błędem, bo inaczej Ctrl+C nie miałby jak dosięgnąć
      programu, który właśnie czeka na klawisz - a to jest dokładnie
      ten program, któremu Ctrl+C jest najczęściej potrzebny.
      `user64/ush64.c` jest zwykłym programem w ring 3: nie ma żadnych
      uprawnień ponad te, które ma każdy inny proces - klawiaturę czyta
      wywołaniem systemowym, programy uruchamia przez `fork` i `exec`,
      czeka na nie przez `wait`. Wszystko, co robi, mógłby zrobić
      dowolny inny program, i to jest sedno: shell w jądrze był
      potrzebny, żeby w ogóle było czym sterować systemem, a ten jest
      dowodem, że **już nie musi tam być**. Nieudany `exec` kończy tylko
      dziecko (kod 127), rodzic żyje dalej - jak wszędzie
- [x] Naprawiony **zawis znaleziony przez użytkownika**: `run ush64`,
      w nim `spin64`, Ctrl+C - i nic. Program kręcił się dalej, konsola
      nie wracała. Dwie przyczyny, obie po mojej stronie. Po pierwsze
      sygnał wysłany do zadania czekającego w `task64_wait` **nigdy nie
      był dostarczany**: sygnał dostarcza się wyłącznie przy powrocie
      do pierścienia 3, a ta pętla nigdy tam nie wraca, więc czekał na
      dostarczenie tak długo, jak długo trwało czekanie - a gdy dziecko
      nie kończy się samo, znaczy to: na zawsze. `read` sprawdzał
      oczekujący sygnał i wracał błędem, `wait` nie. Po drugie Ctrl+C
      trafiał w shella, a nie w program: cel przerwania wskazywał na
      `ush64`, czyli na tego, kto **czeka**, więc nawet działający
      sygnał zabiłby shella i zostawił program przy życiu. Doszło
      `task64_foreground` - schodzi w dół łańcucha dzieci do tego, co na
      końcu faktycznie działa. Zejście jest skończone samo z siebie
      (dziecko dostaje numer większy od rodzica), ale ogranicznik
      głębokości i tak jest: pętla szukająca końca pętli to ostatnie
      miejsce, w którym chce się polegać na rozumowaniu. `ush64` ma też
      od teraz własną procedurę na SIGINT, więc przypadkowe Ctrl+C
      przerywa wpisywaną linię zamiast kończyć sesję - tak jak każdy
      shell. Wcześniejszy audyt pętli **przeoczył to miejsce**: sprawdziłem
      w `task64_wait` drogi wpisu o zakończeniu dziecka, a nie ścieżkę
      sygnału, i właśnie ona była dziurą
- [x] **Dysk w trybie długim**: PCI, ATA PIO, AHCI i warstwa blokowa to
      te same pliki, co w jądrze 32-bitowym, skompilowane drugi raz.
      Sterowniki nie wiedzą nic o trybie procesora - gadają z urządzeniem
      przez porty i pamięć, a jedno i drugie wygląda tak samo po obu
      stronach. `pci.c`, `ata.c` i `blk.c` przeszły **bez jednej zmiany**;
      `ahci.c` potrzebował rzutowań przez `uintptr_t` (adresy DMA zostają
      32-bitowe, bo ramki i tak leżą poniżej 512 MiB) i jednego warunku
      na tryb: rejestry kontrolera leżą wysoko, poza obszarem opisanym
      przy starcie, więc trzeba je dokleić do mapowania - bez tego
      pierwszy odczyt z BAR-a kończy się błędem strony. Doszło
      `paging64_map_phys`: mapuje zakres fizyczny z prawem zapisu
      i zakazem wykonywania, bo w rejestrach urządzenia nie ma kodu.
      Sprawdzone obie drogi - `disk` zapisuje ostatni sektor, czyta go
      z powrotem i liczy różnice: zero, tak samo przez ATA PIO, jak
      i przez AHCI z DMA
- [x] **System plików w trybie długim** - i pierwszy moment, w którym
      cokolwiek w tym jądrze przeżywa wyłączenie maszyny. `fs.c`
      przeszedł **bez zmiany ani jednego znaku**: dysk widziany przez
      warstwę blokową wygląda tak samo niezależnie od trybu procesora,
      a PFS nie dotyka ani stronicowania, ani zadań. Shell dostał `fls`,
      `cat <plik>` i `write <plik> <treść>` - trzy polecenia wystarczą,
      żeby sprawdzić, czy plik naprawdę leży na dysku, a nie tylko
      w pamięci: zapisać, wypisać, zobaczyć na liście. Czwarty dowód
      przychodzi sam, przy ponownym uruchomieniu, i to on jest jedyny
      rozstrzygający - test uruchamia maszynę **drugi raz na tym samym
      dysku** i sprawdza, że plik tam jest i ma tę samą treść, a system
      plików nie został sformatowany od nowa
- [x] **Deskryptory plików dla programów** - do dysku sięga już nie
      tylko jądro. `open`, `close`, `read` i `write` działają w ring 3,
      a deskryptor jest zwykłym numerem w **tablicy należącej do
      zadania**: numer jednego procesu nie znaczy nic w drugim, i to
      jest cała różnica między nim a i-węzłem, który jest wspólny dla
      całego systemu. Każdy deskryptor niesie własne miejsce w pliku,
      bo dwa procesy mogą czytać ten sam plik w innym tempie. Trzy
      pierwsze numery są zajęte od zawsze i nie odnoszą się do żadnego
      pliku: zero to klawiatura, jeden to konsola. Dzięki temu
      wypisanie na ekran jest **zwykłym zapisem do deskryptora numer
      jeden**, dokładnie takim samym jak zapis do pliku - `read`
      i `write` przyjmują więc deskryptor jako pierwszy argument, tak
      jak wszędzie. Dziecko `fork`a dziedziczy otwarte pliki razem
      z miejscem, w którym stoją - tak samo, jak dziedziczy pamięć.
      `fileio64` sprawdza się sam: zakłada plik, pisze, zamyka, otwiera
      z powrotem, czyta i porównuje bajt po bajcie - gdyby cokolwiek
      się nie zgadzało, kończy się błędem, a nie komunikatem, który ktoś
      musi przeczytać. Plik napisany przez program przeżywa restart tak
      samo, jak napisany przez jądro: droga przez deskryptor kończy się
      na tym samym dysku
- [x] **Argumenty programów (`argv`)** - narzędzia przestały musieć
      siedzieć w shellu. Do tej pory `echo` i `cat` były wbudowane, bo
      nie było jak przekazać programowi ani jednego słowa. Układ na
      stosie jest ten sam, co wszędzie: najpierw same napisy, nad nimi
      tablica wskaźników zakończona zerem, a program dostaje
      w rejestrach ich liczbę i adres tablicy - nic w tym magicznego,
      to zwykła pamięć programu, którą akurat jądro wypełniło przed
      jego pierwszą instrukcją. Argumenty kopiujemy do jądra **zanim**
      zniknie pamięć, w której leżą - tak samo jak nazwę programu, i to
      jest ta sama pułapka, co przy `exec`. Napisy przepisujemy stroną
      po stronie, bo napis może kończyć się tuż przed granicą tego, co
      program ma zmapowane. Limity są skromne i celowo: to ma być
      `argv`, a nie kanał do przesyłania danych. `uecho64` wypisuje, co
      dostał, a `ucat64` bierze nazwę pliku z argumentu, otwiera go
      deskryptorem i czyta - jedno i drugie działa tak samo z shella
      jądra, jak i z shella użytkownika. `sep64` też przestał dostawać
      swój numer wpisany do rejestru: przychodzi jako `argv[1]`, czyli
      tak, jak dostaje go każdy program
- [x] **Sterta programów (`sbrk`) z pamięcią na żądanie** - do tej pory
      każdy bufor musiał być wpisany w program na sztywno. Jądro nie
      przydziela tu ani jednej strony: przesuwa tylko granicę
      i zapamiętuje, dokąd program o nią poprosił, a strony powstają
      dopiero przy pierwszym dotknięciu, w obsłudze błędu strony -
      zapisywalne i z zakazem wykonywania, bo w danych programu nie ma
      kodu. Różnica jest większa, niż wygląda: **`heap64` prosi
      o 4 MiB, czyli 1024 strony, a system przydziela mu cztery** -
      dokładnie te, które program dotknął. Zwężenie oddaje strony od
      razu, inaczej pamięć wracałaby do puli dopiero razem z całym
      procesem. Prośba poniżej własnej sterty albo w stos jest
      odrzucana
- [x] Po drodze wyszła różnica x86-64, która nie mogła wyjść wcześniej:
      **SSE2 jest tam bazowe**, więc GCC wektoryzuje pętle bez pytania -
      pierwsza pętla przepisująca 4 KiB dostała `movdqa ... %xmm9`
      i skończyła się wyjątkiem „nieprawidłowa instrukcja", bo jądro
      nie ustawiło jeszcze `CR4.OSFXSR`. Na razie programy budują się
      z `-mno-sse`, tak jak jądro; docelowo trzeba włączyć SSE i zacząć
      zapisywać stan FPU przy przełączaniu zadań. Przy okazji wyszła
      druga rzecz: reguła budowania programów nie zależała od
      `Makefile`, więc zmiana flag kompilacji **nic nie przebudowywała**
      - dokładnie ta sama pułapka, co kiedyś z nagłówkami jądra
- [x] **SSE i stan jednostki zmiennoprzecinkowej** - programy budują się
      znowu normalnie, bez `-mno-sse`. Na x86-64 SSE nie jest dodatkiem,
      tylko częścią konwencji wywołań: kompilator używa go bez pytania,
      bo każdy procesor w tym trybie go ma. Procesor jednak startuje
      z wyłączoną obsługą i każda taka instrukcja kończy się wyjątkiem,
      dopóki system nie powie wprost, że umie te rejestry obsłużyć -
      a „umie" znaczy tu jedno: **potrafi je zapisać i odtworzyć przy
      przełączaniu zadań**. Rejestry XMM są takim samym stanem zadania,
      jak RAX czy RSP, z tą różnicą, że jest ich dużo i procesor
      zapisuje je jedną instrukcją do obszaru 512 bajtów. Wyrównanie do
      szesnastu bajtów nie jest tu grzecznością: `fxsave` i `fxrstor`
      po prostu odmawiają pracy na źle wyrównanym adresie. Samo jądro
      zostaje bez SSE - w obsłudze przerwania nie ma czego liczyć,
      a każda taka instrukcja wymagałaby zapisania stanu przerwanego
      programu. Dowód: dwie kopie `fpu64test` liczą równocześnie, każda
      z inną liczbą początkową, oddając po drodze procesor - i **każda
      dostaje swój wynik**, a nie wynik sąsiada. Działanie dobrane tak,
      by było dokładne co do jednostki (`v = v*2 + 1` na liczbach
      całkowitych mieszczących się w podwójnej precyzji), więc test nie
      musi niczego przybliżać
- [x] **Katalogi i ścieżki** w trybie długim - system plików przestał
      być płaski. `mkdir`, `cd`, `pwd`, `rm` i `readdir` działają
      z ring 3, a **katalog roboczy jest własnością zadania**, tak samo
      jak deskryptory: dwa procesy mogą stać w innych miejscach drzewa
      i żadne z nich nie przeszkadza drugiemu. Dziecko zaczyna tam,
      gdzie stał rodzic - więc `uls64` uruchomiony z shella wypisuje
      ten katalog, w którym shell akurat stał. `open` też patrzy na
      katalog roboczy, a nie zawsze na główny. Po drodze błąd wart
      zapamiętania: `fs_readdir` oddaje **1 przy trafieniu i 0 na
      końcu** katalogu, a nie ujemną wartość - sprawdzanie „mniej niż
      zero" zamieniało koniec katalogu w sukces i wypisywanie kręciło
      się bez końca na starej zawartości bufora. Popełniłem to samo
      w dwóch miejscach naraz, bo założyłem konwencję zamiast ją
      przeczytać
- [x] **Potoki** - programy dają się łączyć w łańcuchy. Potok nie jest
      plikiem i nie ma nic wspólnego z dyskiem: to pierścień w pamięci
      jądra plus **dwa liczniki właścicieli**, i to one są w nim
      ważniejsze od samego bufora, bo mówią, kiedy skończyć. Gdy zniknie
      ostatni piszący, czytający dostaje koniec danych zamiast czekać
      w nieskończoność. Doszedł `dup2` - to tędy przekierowuje się
      wejście i wyjście - a o tym, dokąd idzie zapis, decyduje od teraz
      **rodzaj deskryptora, a nie jego numer**: jedynka bez
      przekierowania to konsola, ale po `dup2` może być końcem potoku
      i program tego nie widzi. `uupper64` czyta z numeru zero i pisze
      na numer jeden; to, że po drugiej stronie siedzi drugi program,
      a nie klawiatura i ekran, jest sprawą tego, kto go uruchomił.
      Najłatwiej tu o zawieszenie i dlatego dwie rzeczy są zrobione
      wprost: shell zamyka **oba** końce potoku u siebie zaraz po
      rozdaniu ich dzieciom (inaczej jeden piszący - on sam - zostałby
      przy życiu na zawsze i czytający nigdy nie zobaczyłby końca),
      a czekanie w `pipe64_read`/`pipe64_write` sprawdza oczekujący
      sygnał, bo w pętli wewnątrz jądra nikt go za nie nie dostarczy.
      Zadanie zamyka też wszystkie swoje deskryptory, gdy się kończy
- [x] **Sieć w trybie długim**: karta RTL8139, warstwa IP, DHCP i TCP -
      znowu te same pliki, co w jądrze 32-bitowym. **Trzy z czterech
      przeszły bez jednej zmiany**; sterownik karty potrzebował tylko
      jawnego rzutowania adresów, bo karta adresuje pamięć 32 bitami,
      a wskaźnik ma tu osiem bajtów (bufory leżą w obrazie jądra, tuż
      nad megabajtem, więc adres mieści się z zapasem - ale rzutowanie
      musi być widoczne). Nie kompilowały się natomiast z innego
      powodu: kod sieciowy mówi do jądra kilkoma funkcjami - uśpij
      mnie, czy mam zginąć, czy tamto zadanie jeszcze żyje, zrób nowy
      wątek - a scheduler nazywa się tu inaczej. Zamiast rozsypywać po
      nich warunki na tryb, doszedł `kernel64/compat64.c`: **te nazwy
      są umową, a co za nimi stoi, jest sprawą jądra**. Ten jeden plik
      jest całą różnicą. Dowód z prawdziwego biegu: DHCP przyznaje
      adres, `ifconfig` pokazuje maskę, bramę i DNS, `ping` na bramę
      wraca 3/3, a tablica ARP zna jej adres sprzętowy - czyli ramki
      naprawdę wychodzą i wracają
- [x] **Sieć od strony programu** - z sieci korzystało dotąd tylko
      jądro, bo `ping` i `ifconfig` to jego własne polecenia. Teraz
      program w ring 3 pyta o swój adres, otwiera gniazdo UDP, wysyła
      datagram i łączy się po TCP - bez żadnych szczególnych uprawnień.
      Gniazdo należy do zadania, które je otworzyło: jądro trzyma jego
      numer i sprząta po jego śmierci, więc program nie sięgnie po cudze
      gniazdo, podając jego numer. Przy okazji lekcja o ABI: pierwsza
      wersja upychała numer gniazda i limit czasu w jednym rejestrze,
      bo trzy rejestry na argumenty to za mało na gniazdo, adres, port,
      długość i limit naraz. Czytało się to fatalnie i psuło przy
      pierwszej zmianie - teraz wszystko poza samymi danymi idzie
      w jednym opisie przekazywanym wskaźnikiem, a jądro wpisuje w ten
      sam opis adres nadawcy przy odbiorze. `unet64` sprawdza trzy
      rzeczy po kolei i sam wystawia sobie ocenę: adres z DHCP,
      datagram wysłany **do samego siebie** (pętla zwrotna nie wychodzi
      na kabel, więc próba dotyczy stosu, a nie sieci dookoła) oraz
      połączenie TCP z serwerem HTTP stojącym na hoście - w sieci
      użytkownika QEMU brama `10.0.2.2` **jest** hostem, więc żadne
      przekierowanie portów nie jest do tego potrzebne
- [x] **Tryb graficzny w jądrze 64-bitowym** - `video.c` przeszedł jak
      reszta, ale wyszła przy tym rzecz, która w jądrze 32-bitowym nie
      mogła wyjść: przyjmując bufor od loadera wpisywał sobie **na
      sztywno 32 bity na piksel**. Działało to, dopóki bufor przychodził
      z BIOS-u; firmware UEFI potrafi podać 24, a wtedy zapis całym
      słowem zadeptuje sąsiedni piksel. Teraz głębia jest taka, jaką
      loader naprawdę dał - dokładnie ta sama poprawka, co wcześniej
      w konsoli. Shell dostał `gfx`: cztery prostokąty w rogach, ramka
      i napis własną czcionką. Rysunek jest prosty celowo, bo każdy błąd
      w liczeniu adresu piksela widać na nim od razu - zły bajt na
      piksel rozjeżdża obraz w skos, a zamienione kanały widać po
      kolorach. Test sprawdza go pod BIOS-em **i** pod UEFI, bo to
      firmware decyduje, ile bitów ma piksel. Po drodze własna pomyłka
      warta zapamiętania: pierwszy wzorzec użył numerów kolorów trybu
      **tekstowego**, a paleta `video.c` ma zdefiniowane własne indeksy
      0-5 - kod rysował dobrze, to wzorzec był w złych kolorach
- [x] **Pulpit graficzny w jądrze 64-bitowym** - i to jest ta pozycja,
      przy której najbardziej opłacił się cały sposób prowadzenia tego
      portu. `desktop.c` (736 wierszy: kompozytor, okna, paski tytułu,
      ikony, pasek zadań, menu, przeciąganie) i `mouse.c` przeszły
      **bez jednej zmiany**. Cała różnica zamknęła się w trzech
      funkcjach dopisanych do słownika zgodności: `exec_spawn` po
      nazwie, przejęcie klawiatury przez pulpit i oddanie jej z
      powrotem. Reszta - zadania, sen, system plików do zapisu pozycji
      ikon - była już na miejscu z portu sieci. Bałem się tego kroku
      i okazał się najtańszy z dotychczasowych: nie dlatego, że pulpit
      jest prosty, tylko dlatego, że nie zależy od trybu procesora
      w niczym poza nazwami, którymi woła jądro. Czego jeszcze nie ma:
      aplikacji graficznych w ring 3 - ikony `App`, `Zegar`, `Pliki`
      wołają programy, których w 64-bitowym initrd nie ma, więc klik
      w nie nic nie robi. To wymaga wywołań systemowych okien i pamięci
      współdzielonej, czyli osobnego kroku
- [x] **Aplikacje graficzne w ring 3 (jądro 64-bitowe)** - ikony
      pulpitu przestały być martwe. Cztery wywołania systemowe
      (`win_create`, `win_rect`, `win_text`, `win_poll`) i program
      `guiapp64` w initrd: rysuje we własnym oknie, dostaje z powrotem
      kliknięcia i klawisze, liczy je i odpowiada rysunkiem. Kompozytor
      nie dostał ani jednej nowej linii - `desktop_win_*` istniało od
      czasów 32-bitowych, tu doszła tylko droga do niego z pierścienia 3.
      Programu nie trzeba było nawet nazywać inaczej: `mkinitrd` wpisuje
      go do obrazu pod nazwą `guiapp`, tą samą, którą woła wspólna
      tablica ikon. Ochrona jest tam, gdzie była: właścicielem okna jest
      proces, a nie ten, kto poda numer, więc podanie cudzego numeru nic
      nie daje; napisy i polecenia rysowania jądro przepisuje do siebie,
      bajt po bajcie sprawdzając, czy program ma prawo tam sięgać -
      napisu nie da się sprawdzić hurtem, bo może kończyć się na
      ostatnim bajcie mapowania.

      I przy tym wyszedł błąd, który spał w jądrze od pierwszego
      programu: pierwszy `movaps` w `guiapp64` kończył się wyjątkiem
      ochrony ogólnej. Nie w SSE rzecz - stan rejestrów SSE jądro
      zapisuje od dawna - tylko w **wyrównaniu stosu**. `_start` jest
      napisany w C, a kompilator traktuje go jak każdą inną funkcję:
      zakłada stos taki, jaki zastaje się po instrukcji `call`, czyli
      z adresem powrotu na wierzchu. Jądro dawało wierzchołek wyrównany
      do szesnastu bajtów, czyli o osiem za dużo, i każde szesnastobajtowe
      zapisanie rejestru SSE trafiało w adres niewyrównany. Dotychczasowe
      programy przeżyły tylko dlatego, że kompilator nie miał w nich
      czego wektoryzować. Ciekawe, że doręczenie sygnału liczyło stos
      **poprawnie** od początku - tam komentarz wprost mówi "tak, jak
      zastaje każda funkcja wołana instrukcją call" - a start procesu
      nie. Jedna linia w `build_args`, ta sama zasada w obu miejscach
- [x] **ACPI, konsole wirtualne, klawiatura USB i wymiana stron
      w jądrze 64-bitowym** - cztery rzeczy, które 32-bitowe jądro
      miało od dawna, przeszły jedna po drugiej. Trzy z nich prawie za
      darmo, czwarta pokazała błąd, którego 32-bitowa wersja nie mogła
      pokazać.

      **ACPI** to `kernel/acpi.c` bez zmian w logice: droga do
      wyłączenia zasilania - RSDP, spis tablic, FADT, obiekt `_S5_`
      w bajtkodzie AML - nie zależy od trybu procesora. Zależy od niego
      tylko doklejanie tablic do mapowania, więc plik dostał ten sam
      wzorzec makr, co `initrd.c` i `video.c`, plus jawne rzutowania
      adresów na wskaźniki. `poweroff` gasi maszynę, `reboot` ją
      resetuje - i tego drugiego nie da się sprawdzić inaczej niż tym,
      że QEMU (z `-no-reboot`) po prostu znika.

      **Konsole wirtualne** siedziały już w `kernel/vga.c`, wspólnym od
      początku - jądro 64-bitowe po prostu miało tylko jedną, bo
      `task_vc()` zwracało zero na sztywno. Doszła więc własność
      zadania (konsola dziedziczy się po rodzicu, tak samo jak katalog
      roboczy), osobna kolejka klawiszy na każdą konsolę i Alt+F1..F4
      w obsłudze przerwania. Shell przy okazji przestał trzymać wiersz
      polecenia w zmiennej globalnej: cztery shelle to cztery wątki,
      a wiersz jest własnością wątku i leży na jego stosie.

      **Klawiatura USB** wymagała w `uhci.c` tego samego, co wcześniej
      karta sieciowa: kontroler adresuje pamięć 32 bitami, wskaźnik ma
      tu osiem bajtów, więc każdy adres struktury idzie do niego przez
      jawne rzutowanie. `usbkbd.c` przeszedł bez zmian.

      **Wymiana stron** to jedyna pozycja z prawdziwą nową robotą po
      stronie trybu długiego. Magazyn (`kernel/swap.c`) jest wspólny,
      ale polityka wyrzucania musiała powstać od nowa: obchód czterech
      poziomów tablic zamiast dwóch, algorytm drugiej szansy na bicie
      "odwiedzona" i wpis strony leżącej na dysku - bez bitu obecności,
      więc procesor go nie ogląda, a numer miejsca siedzi w górnych
      bitach.

      I tu wyszedł błąd wart zapamiętania. Pierwsza wersja wpisu na
      dysk niosła sam numer miejsca, a przy powrocie strona dostawała
      prawa "zapisywalna, bez wykonywania" - bo takie ma większość
      stron. Program padał zaraz po starcie z wyjątkiem 14 o kodzie
      `0x15`. Przeczytałem to najpierw jako **piętnaście dziesiętnie**
      i szukałem naruszenia bitu zarezerwowanego, którego tam nie było;
      szesnastkowo to jest obecna strona, dostęp z pierścienia 3 i
      **pobranie instrukcji**. Wyrzucona została strona *kodu*, a
      wróciła z zakazem wykonywania. Prawa dostępu jadą więc teraz na
      dysk razem ze stroną - i to jest właściwa zasada, nie łatka:
      strona kodu jest tylko do odczytu i wykonywalna, strona danych
      dokładnie odwrotnie, a jądro nie ma prawa zgadywać, która jest
      która, skoro samo je wcześniej rozdało.

      Przy okazji przestrzeń programu urosła z ośmiu megabajtów do pół
      gigabajta - wymiana ma sens dopiero wtedy, gdy program może
      poprosić o więcej pamięci, niż maszyna ma RAM-u. Test bierze 18
      MiB na maszynie z 16 MiB i sprawdza dwie rzeczy: że każda strona
      wróciła z dysku bez zmiany choć jednego bajtu, i że wymiana
      **naprawdę ruszyła** - bo bez tej drugiej asercji przeszedłby
      także test, w którym wszystko zmieściło się w pamięci.

      Testów przybyło pięć osobnych grup: `gui64`, `acpi64`,
      `konsole64`, `usbkbd64`, `swap64`. Wszystkie 64-bitowe czytają
      teraz ekran tak, jak widzi go człowiek: GRUB, widząc w nagłówku
      jądra żądanie trybu obrazu, zawsze daje grafikę, więc pamięć
      znaków spod `0xB8000` jest pusta, a konsola rysuje się sama
      czcionką 8x8. Test rozpoznaje te glify tą samą tablicą, z której
      korzysta jądro - jedno źródło prawdy, i odczyt z tego, co
      naprawdę widać.
- [x] **Programy z dysku, nie tylko z initrd** - loader ELF i system
      plików leżały obok siebie od dawna i nigdy nie były połączone:
      `run` zaglądał wyłącznie do initrd, czyli do obrazu wgrywanego
      razem z jądrem i tylko do odczytu. Teraz program może leżeć na
      dysku - `cp hello64 cokolwiek`, a potem `run cokolwiek`.

      Całe połączenie to jeden plik, `kernel/prog.c`, i nie ma w nim
      ani słowa o trybie procesora: decyduje tylko, skąd wziąć bajty,
      a co z nimi zrobić, wie już loader. Dlatego używają go **oba
      jądra** - w 64-bitowym przez słownik zgodności (`task_cwd`),
      w 32-bitowym wprost. Kolejność jest ustalona: najpierw initrd,
      potem dysk. Initrd jest zawsze i zawsze działa, więc zepsuty
      plik na dysku nie ma jak przykryć programu wgranego razem
      z jądrem - a to znaczy, że nie ma jak odebrać systemu.

      Program szuka się w katalogu roboczym **zadania**, tak samo jak
      przy `cat` czy `ls`: katalog jest własnością procesu, więc `run`
      znaczy to samo, co znaczyłby po `cd`.

      I znowu ta sama pomyłka, co przy `fs_readdir` - a przecież już
      raz ją tu opisałem. `fs_write_at` zwraca **liczbę zapisanych
      bajtów**, nie zero, a ja sprawdzałem `== 0`; `fs_read_at` tak
      samo. Pierwsze `cp` "nie udało się" mimo poprawnego zapisu.
      Wniosek na przyszłość jest chyba tylko jeden: konwencji zwrotu
      nie wolno zgadywać z podobieństwa do sąsiedniej funkcji - trzeba
      ją przeczytać. Teraz jeden warunek sprawdza, że zapisało się
      **tyle, ile miało**, bo plik ucięty w połowie to nie jest ten
      sam program.

      Test bierze najmocniejszy dowód, jaki tu jest: drugi rozruch tej
      samej maszyny z tym samym obrazem dysku. Program, który po
      restarcie dalej się uruchamia, a którego nie ma w initrd, może
      pochodzić już tylko z talerza. Sprawdzamy też drogę z pierścienia
      3 - `ush64` robi `fork` i `exec`, a `exec` szuka programu tak
      samo jak jądro.

      Przy okazji dorobiłem testom `q.polecenie(...)`: wpisuje
      polecenie i oddaje **tylko to, co przybyło po nim**. Bez tego
      łatwo o fałszywy alarm w drugą stronę - shell odbija wpisany
      wiersz, więc nazwa pliku jest na porcie także wtedy, gdy
      polecenie niczego nie znalazło, a wynik poprzedniego leży tam do
      końca biegu. Trzy asercje w tym teście przechodziły z tego
      powodu, zanim funkcja w ogóle zaczęła działać.
- [x] **Pamięć współdzielona i dwie kolejne aplikacje graficzne
      (jądro 64-bitowe)** - `kernel/shm.c` przeszedł bez zmian
      w logice, bo region to po prostu ciągły blok ramek z PMM.
      Nowa jest droga do niego z pierścienia 3: między stertą a stosem
      wydzielony został obszar mapowań (od 0x50000000 w górę), a sterta
      kończy się u jego progu - jedno nie ma jak wejść w drugie.
      Wymiana stron pomija ramki współdzielone, tak jak w jądrze
      32-bitowym: z nimi trzeba by liczyć właścicieli także na dysku.

      Test pokazuje to, o co w tym naprawdę chodzi, a nie tylko to, że
      bajty się zgadzają: pisarz mapuje najpierw region na boku,
      o innym kluczu, więc **wspólny widzi pod innym adresem niż
      czytelnik**. Te same bajty pod dwoma różnymi adresami - bo
      o tym, gdzie coś widać, decyduje przestrzeń adresowa, a nie
      pamięć. Bez tego test przeszedłby także przy przypadkowej
      zgodności układu.

      Przy okazji doszedł zegar CMOS (`rtc.c`, czyste porty I/O -
      przeszedł jak stał), polecenie `date` i wywołanie `SYS64_TIME`.

      Dwie nowe aplikacje w oknach: `gclock64` (godzina ze sprzętu
      i uptime z tyknięć jądra - dwa niezależne źródła czasu obok
      siebie) i `gfiles64` (lista plików z dysku, kliknięcie nazwy
      wczytuje jej treść). Ikony `Zegar` i `Pliki` przestały więc być
      martwe; zostały trzy: Monitor, Term i Web.

      I znowu ta sama pomyłka, co przy `fs_write_at` i przy
      `fs_readdir` - **trzeci raz w tym projekcie**. `sys_readdir`
      w bibliotece programów oddaje ZERO przy trafieniu i -1 na końcu
      katalogu, a `fs_readdir` w jądrze odwrotnie: jedynkę przy
      trafieniu. Napisałem `!= 1`, więc pierwszy wpis wyglądał jak
      koniec katalogu i okno pokazywało "(pusto)". Sąsiedztwo nazw nie
      jest żadną wskazówką co do konwencji zwrotu; trzeba ją
      przeczytać, za każdym razem.
- [x] **Monitor zasobów i terminal w oknie (jądro 64-bitowe)** - piąta
      i szósta aplikacja graficzna, a przy okazji wywołanie
      `SYS64_SYSINFO`: ile pamięci jest, ile zajęte, jak długo maszyna
      działa. Nic z tego nie jest tajemnicą - liczby opisują maszynę,
      a nie cudze procesy - więc program z pierścienia 3 dostaje je bez
      żadnych uprawnień.

      `gterm64` jest tu najciekawszy, bo nie ma w nim ani jednej rzeczy,
      której jądro nie umiałoby wcześniej, a jest wszystko naraz:
      klawiatura przez zdarzenia okna, `fork`, `exec`, przekierowanie
      wyjścia przez `dup2` do pliku, czekanie na dziecko, odczyt pliku
      i rysowanie. Terminal jest zwykłym programem i o kompozytorze nie
      wie nic poza czterema wywołaniami okien - a jednak z tych
      klocków wychodzi powłoka w okienku.

      Kompozytor trzyma najwyżej sześć okien, a dwa zajmuje jądro, więc
      pięć aplikacji naraz się nie mieści - test dostał drugi rozruch
      i to jest właściwa odpowiedź, nie podnoszenie limitu.

      Została jedna martwa ikona: Web. Do niej trzeba najpierw TLS
      i klienta HTTP w pierścieniu 3.
- [x] **TLS 1.3, HTTP i przeglądarka w pierścieniu 3 (jądro
      64-bitowe)** - i tym samym koniec doganiania: wszystko, co robi
      jądro 32-bitowe, robi teraz także to w trybie długim.

      Najciekawsze jest to, ile kodu trzeba było napisać: **jeden plik
      nagłówkowy**. Kryptografia, TLS, HTTP, DNS, X.509 i parser HTML -
      pięć tysięcy wierszy w `user/*.h` - liczą na bajtach i o trybie
      procesora nie wiedzą nic. Do jądra mówią kilkoma nazwami: połącz
      się, wyślij, odbierz, która godzina, otwórz plik. Te nazwy są
      umową; `user64/compat64.h` przekłada je na wywołania trybu
      długiego, a `user/uapi.h` wybiera stronę. Dokładnie ten sam
      wzorzec, co `kernel64/compat64.c` po stronie jądra - i drugi raz
      okazał się tańszy, niż się bałem.

      Zmiany w samym wspólnym kodzie: jedna. Adres na stosie brany do
      zasiewu generatora szedł przez `(uint32_t)` z wskaźnika - w trybie
      długim to obcięcie połowy adresu, więc teraz przez `uintptr_t`.
      Nic więcej.

      Wyszedł za to prawdziwy błąd. `certtest` padał w połowie -
      wyjątek strony tuż pod szczytem stosu. Program dostawał cztery
      strony (16 KiB), a weryfikacja podpisu na liczbach
      dwutysiącbitowych sięga głębiej - i sięga tym głębiej, że
      wskaźniki mają tu osiem bajtów, więc każda ramka z tablicą
      wskaźników jest dwa razy większa. Można było podnieść stałą.
      **Zamiast tego stos rośnie teraz na żądanie**, dokładnie tak jak
      sterta: program dostaje cztery strony na start, reszta dochodzi
      pod palcami, a granica (ćwierć megabajta) jest po to, żeby dziki
      wskaźnik nie zamienił się po cichu w zmapowaną stronę - pod nią
      błąd strony pozostaje błędem. Zgadywanie z góry, ile stosu
      potrzeba, zawsze kończy się albo marnowaniem pamięci, albo
      wyjątkiem w środku głębokiego wywołania.

      Test rozmawia z prawdziwym OpenSSL-em, a nie z własną pętlą, więc
      dojście uzgodnienia do skutku znaczy, że zgadza się każdy bajt:
      X25519, ChaCha20-Poly1305, wyprowadzanie kluczy i podpis
      w Finished. Sprawdzamy też rzecz ważniejszą od samego
      szyfrowania: obcy certyfikat ma połączenie **zerwać**, bo
      szyfrowana rozmowa z kimś nieznanym nie jest bezpieczna, tylko
      prywatna.

      Ostatnia martwa ikona pulpitu ożyła: `gweb` rozwiązuje nazwę
      przez DNS, pobiera stronę, obdziera ją ze znaczników i rysuje
      w oknie - a to jeden program spinający naraz kompozytor,
      klawiaturę, mysz, DNS, TCP, TLS, system plików i własną
      przestrzeń adresową.
- [x] **Podział grupy `rdzen` na trzy** - nie dla porządku, tylko dla
      czasu: jedna grupa robiła 174 asercje na jednej maszynie i sama
      decydowała, ile trwa cały bieg, bo wszystkie pozostałe kończyły
      się dużo wcześniej. Teraz są trzy - rozruch i procesy (`rdzen`),
      sieć z systemem plików (`rdzen2`) i samo TLS (`tls32`) - a że
      liczą się równolegle, cały zestaw schodzi w 259 s zamiast 310 s.
      Asercji tyle samo, 518.

      Podział wyciągnął przy okazji flakę, która czekała tam od dawna.
      Wątek demonstracyjny C kończy się po dwóch sekundach i przez
      kilka następnych odrysowuje napis w zerowym wierszu ekranu -
      i tylko tam był dowód, że się wykonał. Wiersz zerowy znika przy
      każdym przewinięciu, więc na obciążonej maszynie dowód wyjeżdżał
      razem z komunikatami startowymi, zanim test zdążył spojrzeć.
      Przesunięcie sprawdzenia wcześniej nie wystarczyło: pod pełnym
      obciążeniem samo dojście do promptu trwa dłużej niż całe okno
      widoczności.

      Wątek pisze więc teraz jedną linię **wprost na port szeregowy** -
      nie przez `printk`, bo kończy się już po starcie shella i wypis
      na konsoli wpadałby w środek wiersza, który ktoś akurat pisze
      (test klawiatury złapał to od razu). Port szeregowy jest
      dziennikiem, ekran bieżącą pracą; to nie zawsze jest to samo,
      i dobrze, że akurat ta różnica musiała się tu ujawnić.
- [x] **Pliki odwzorowane w pamięci (jądro 64-bitowe)** - druga połowa
      tej samej myśli, co uruchamianie programów z dysku: skoro plik
      może być programem, może też być **pamięcią**. Program dostaje
      adres i czyta zawartość jak zwykłą tablicę, bez ani jednego
      wywołania `read`.

      Najciekawszy jest koszt. Odwzorowanie nie tworzy ani jednej
      strony: jądro zapamiętuje tylko numery bloków, a bajty
      przychodzą z dysku pod palcami. Dotknięcie jednego bajtu
      czterystukilobajtowego pliku kosztuje **jedną stronę**, nie sto -
      i to jest asercja, którą test sprawdza wprost, bo shell podaje po
      każdym programie, ile stron wczytano z odwzorowań.

      Numery bloków ustalamy raz, przy zakładaniu odwzorowania, i potem
      czytamy je warstwą blokową z pominięciem kodu PFS. Powód jest ten
      sam, co przy wymianie stron i wtedy też był zapisany: PFS ma
      jeden wspólny bufor na blok, a błąd strony potrafi trafić
      w środek dowolnej operacji na plikach. Raz opisana zasada
      oszczędziła tu całą klasę pytań.

      Odwzorowanie jest tylko do odczytu i to jest decyzja, nie
      niedoróbka: zapis wymagałby odsyłania stron z powrotem na dysk
      i rozstrzygania, kiedy. Dopóki tego nie ma, lepiej niech próba
      zapisu kończy się błędem ochrony, niż cicho przepada. Test
      sprawdza to dzieckiem: dziecko próbuje zapisać i ginie, a rodzic
      przeżywa i melduje, że treść pliku jest nietknięta.
- [x] **Karta e1000 i warstwa `netdev`** - dyski miały warstwę blokową
      od dawna: ATA i AHCI stoją pod jedną nazwą, a który kontroler
      odpowiedział, jest sprawą rozruchu. Sieć takiej warstwy nie
      miała i było to widać w każdej linii, która potrzebowała adresu
      MAC - `rtl8139_mac()` wprost w kodzie IP i DHCP. Teraz jest
      `netdev.c`, dokładnie z tego samego pomysłu.

      Intel 82540EM jest zbudowany zupełnie inaczej niż RTL8139
      i dlatego był dobrym sprawdzianem dla tej warstwy. Rejestry ma
      w pamięci, nie w portach I/O - trzeba je najpierw dokleić do
      mapowania, bo leżą wysoko. Ruch chodzi przez dwa **pierścienie
      deskryptorów**, po jednym na kierunek, zamiast jednego pierścienia
      bajtów: deskryptor mówi, gdzie leży bufor i co się z nim stało,
      a kto go ma na własność, wynika ze wskaźników głowy i ogona.
      Wygodniejsze od RTL8139 o tyle, że pakiety nie sklejają się ze
      sobą i nie trzeba przepisywać końca bufora na początek.

      Dla ARP-a, IP, DHCP i TCP nie zmieniło się nic - i to jest
      właściwa treść tego kroku. Test uruchamia ten sam zestaw
      sprawdzeń z `-device e1000` na obu jądrach: dzierżawa DHCP,
      ping do bramy, tablica ARP, HTTP z pierścienia 3. Ta sama droga
      pakietu, inna karta pod spodem.

      Odbiór idzie w drugą stronę niż reszta: sterownik sam woła
      `net_rx`, gdy ramka przyjdzie, i warstwa `netdev` w tym nie
      pośredniczy - byłoby to przekładanie wskaźnika bez powodu.
- [x] **Dysk NVMe** - trzeci sterownik pod tą samą warstwą blokową
      i najbardziej niepodobny do dwóch poprzednich: nie ma w nim nic
      z dziedzictwa ATA. Żadnego "wybierz dysk", żadnych ramek FIS.
      Są dwie **kolejki w pamięci** - my wkładamy polecenia po 64 bajty
      do kolejki zgłoszeń, kontroler odkłada odpowiedzi po 16 do
      kolejki ukończeń - i dwa dzwonki (rejestry doorbell), którymi
      mówi się drugiej stronie, dokąd się doszło.

      Najładniejszy drobiazg całego standardu to **bit fazy**:
      kontroler odwraca go przy każdym okrążeniu kolejki, więc żeby
      wiedzieć, czy odpowiedź jest nowa, nie trzeba niczego zerować
      ani liczyć. Wystarczy porównać jeden bit z tym, którego się
      spodziewamy.

      Kolejek jest u nas dwie pary: administracyjna, która powstaje
      sama przy starcie kontrolera, i jedna do danych, zamawiana
      poleceniem na tej pierwszej. Bufor jest jeden, wyrównany do
      strony - wskaźnik PRP opisuje stronę, a nie dowolny kawałek
      pamięci, więc przepisanie przez własną, wyrównaną stronę jest
      tańsze niż tłumaczenie każdego cudzego bufora na listę stron.

      Test wpadł przy okazji w pułapkę, która nie ma nic wspólnego
      z NVMe: pisał polecenia zaraz po komunikacie o gotowym systemie
      plików, a shell rusza dopiero po schedulerze - pierwsze znaki
      przepadały i z `write nvmetest tresc-przez-kolejki` do jądra
      docierało `przez-kolejki`. Klawiatura działa od przerwania,
      ale czytać z niej ma kto dopiero od wątku.
- [x] **Pozostałe procesory: wykrycie i wybudzenie (oba jądra)** -
      pierwszy krok w stronę SMP, i celowo tylko pierwszy.

      Po włączeniu zasilania pracuje jeden rdzeń - ten, który firmware
      wybrał na rozruchowy. Reszta stoi i czeka, i to jest decyzja
      sprzętu: procesor obudzony sygnałem SIPI zaczyna w trybie
      rzeczywistym, szesnastobitowo, pod adresem `(wektor << 12)` -
      dokładnie tak, jak zaczynał pecet czterdzieści lat temu. Ile ich
      jest, mówi tablica MADT w ACPI; budzi się je parą sygnałów przez
      lokalny APIC.

      Najciekawsze jest to, ile drogi taki rdzeń musi przejść **sam**.
      W jądrze 32-bitowym: tryb rzeczywisty, własna tablica
      deskryptorów, tryb chroniony. W 64-bitowym cała droga od nowa -
      szesnaście bitów, trzydzieści dwa, sześćdziesiąt cztery, z PAE
      i EFER po drodze. Tablic stron nie buduje: bierze gotowe, te
      same, po których chodzi jądro, a ich adres dostaje w polu, które
      procesor rozruchowy wypełnia przed wysłaniem sygnału.

      Trampolina jest linkowana z jądrem, ale wykonuje się pod
      `0x8000`, więc każdy adres w niej liczy się jako baza plus
      odległość od początku bloku. Adresy trafiają najpierw do
      rejestru, a dopiero potem w użycie - asembler ma wtedy jedno
      oczywiste znaczenie do wyboru zamiast dwóch.

      I na tym ten krok się kończy: obudzony rdzeń zgłasza się
      w liczniku i staje. Dawanie mu pracy wymaga schedulera per rdzeń
      i zamków w każdej strukturze jądra - a te pisane były dla jednego
      rdzenia i cicho na to liczą. Lepiej, żeby procesor stał, niż żeby
      psuł listę zadań. Dlatego test sprawdza dwie rzeczy naraz: że
      rdzenie się zgłaszają **oraz** że system po ich obudzeniu działa
      dokładnie tak samo - samo "widzę cztery procesory" nie byłoby nic
      warte, gdyby przy okazji coś pękało.
- [x] **Naprawa `make fuzz`** - i lekcja o tym, po co w ogóle jest
      osobne polecenie, którego nie uruchamia `make test`.

      Fuzzing parserów buduje je **na hoście**, pod ASan i UBSan,
      podstawiając zaślepkę zamiast wejścia do jądra. Port TLS-a do
      trybu długiego zmienił to wejście: nagłówki wciągają teraz
      `uapi.h`, a nie `usys.h`, więc zaślepka podstawiała się pod
      nazwę, której nikt już nie woła - i harness przestał się
      budować. Warstwa `netdev` dołożyła drugie takie miejsce po
      stronie jądra.

      Obie rzeczy przeszły niezauważone przez pełny bieg testów, bo
      `make fuzz` nie jest jego częścią - i słusznie nie jest, bo trwa
      swoje i sprawdza co innego. Ale to znaczy, że trzeba go
      uruchamiać osobno po każdej zmianie w tym, jak kod wspólny
      rozmawia z jądrem. Zapisuję to tutaj właśnie dlatego, że łatwo
      o tym zapomnieć: pięćset przypadków na parser, zero błędów -
      ale tylko wtedy, gdy ktoś je puści.
- [x] **Rdzenie wtórne dostają pracę** - i pierwszy zamek w tym
      systemie.

      Wyłączenie przerwań wystarczało, dopóki rdzeń był jeden: nikt
      inny nie mógł wejść w środek operacji. Teraz może - i to
      naprawdę **jednocześnie**, a nie tylko pozornie. Stąd
      `spinlock.h`: instrukcja `xchg` jest niepodzielna z definicji
      i przy okazji stanowi barierę dla porządku zapisów, więc nie
      trzeba tu żadnych dodatkowych ogrodzeń - to, co rdzeń zapisał
      przed zwolnieniem zamka, drugi zobaczy po jego wzięciu.

      Rdzenie wtórne nie prowadzą własnych zadań i długo jeszcze nie
      będą: scheduler, lista procesów i większość jądra pisane były dla
      jednego rdzenia. Dostają za to pracę **policzalną**: wspólną
      kolejkę jednostek, z której każdy bierze kolejny numer pod
      zamkiem i liczy go u siebie. Cały stan dzielony między rdzeniami
      to jedna liczba i jeden zamek - i właśnie dlatego da się o nim
      myśleć.

      Rdzeń rozruchowy liczy razem z pozostałymi; bez tego na maszynie
      jednordzeniowej nie policzyłby nikt. Przerwania gasną na czas
      trzymania zamka także na nim: gdyby timer wywłaszczył wątek
      trzymający zamek, każdy inny chętny czekałby do jego następnej
      kolejki.

      Polecenie `smp` rozdziela 64 jednostki i porównuje wynik
      z policzonym na jednym rdzeniu. To jest właściwe pytanie
      o współbieżność - nie "czy coś się policzyło", tylko "czy
      policzyło się **dokładnie raz każde**". Cztery rdzenie, po
      szesnaście jednostek, suma co do bitu ta sama.

      Błąd po drodze wart zapamiętania: pierwsza wersja trampoliny
      wołała funkcję jądra skokiem względnym. Przesunięcie policzone
      przy linkowaniu opisuje odległość od miejsca, w którym kod
      **leży w obrazie jądra** - a wykonuje się pod `0x8000`. Skok
      trafiał więc w próżnię i maszyna resetowała się w ciszy, bo rdzeń
      wtórny nie ma jeszcze tablicy wyjątków. Adres idzie teraz przez
      rejestr, tak jak w wersji 64-bitowej, gdzie od początku musiał -
      bo tam skok względny sięga najwyżej dwóch gigabajtów.
- [x] **Stos rosnący na żądanie i pliki odwzorowane w pamięci - także
      w jądrze 32-bitowym** - wyrównanie asymetrii, którą sam
      wprowadziłem, robiąc obie te rzeczy najpierw w trybie długim.
      Ten projekt stoi na tym, że oba jądra to te same pliki
      kompilowane dwa razy; różnice, których nie wymusza tryb
      procesora, są w nim po prostu długiem.

      Stos: osiem stron na start, reszta dochodzi pod palcami, granica
      ćwierć megabajta. Test schodzi rekurencją na czterdziesty poziom
      po cztery kilobajty ramki i **po powrocie** sprawdza, czy każda
      ramka jest wciąż jego - bo dokładanie stron pod nadpisywanie
      cudzych ramek wyglądałoby w połowie tak samo.

      Odwzorowanie plików wyciągnęło przy okazji prawdziwy błąd, i to
      w kodzie starszym niż cała ta zmiana. `fork` oznaczał **każdą**
      stronę procesu jako kopiowaną przy zapisie - także taką, która
      celowo nie miała prawa zapisu. Dziecko pisało więc do
      odwzorowanego pliku i zapis się *udawał*: obsługa kopiowania
      przy zapisie robiła mu prywatną, zapisywalną kopię. Strona bez
      prawa zapisu zostaje teraz taka, jaka jest - i to jest cała jej
      własność, więc oznaczanie jej do kopiowania przy zapisie było
      sprzeczne samo w sobie.

      Doszła też para `paging_map_user_in` / `paging_map_user_ro_in`
      nad wspólnym trzonem: to samo mapowanie, jeden bit różnicy,
      wybierany przez wołającego.
- [x] **Zapis przez plik odwzorowany w pamięci** - droga w drugą
      stronę, w obu jądrach naraz.

      Dotąd odwzorowanie było jednokierunkowe: plik dawało się czytać
      jak pamięć, ale próba zapisu kończyła się błędem ochrony. Było
      to uczciwe (lepszy błąd niż cicho zgubiona zmiana), ale połowiczne.
      Teraz `sys_mmap_file_w` daje stronom prawo zapisu, a treść wraca
      do pliku.

      Najważniejsze pytanie brzmi **kiedy**. Odsyłanie po każdym bajcie
      zabiłoby cały sens odwzorowania - wtedy taniej byłoby wołać
      `write`. Decyduje więc program, przez `sys_msync`, a jądro
      dokłada jedno miejsce, o którym program nie musi pamiętać:
      koniec procesu. Odesłanie musi się zdarzyć **tam**, w `task_exit`
      i w `exec`, dopóki przestrzeń adresowa procesu jeszcze żyje -
      sprzątacz zombie ogląda te strony już z cudzego CR3, a `exec`
      za chwilę tej przestrzeni nie ma.

      Które strony odesłać, wie procesor. Bit "brudna" ustawia sam przy
      każdym zapisie, więc nie trzeba niczego liczyć ani zgadywać: test
      czyta cały dwunastokilobajtowy plik, pisze po dwóch pierwszych
      stronach, i `sys_msync` odsyła **dwie**. Drugie z rzędu odsyła
      zero, bo bit gaśnie razem z odesłaniem. To jest asercja, która
      naprawdę coś rozstrzyga - "odesłało się cokolwiek" przeszłoby
      równie dobrze przy przepisywaniu całego pliku za każdym razem.

      Doszedł trzeci bit programowy we wpisie strony - `PTE_PLIK`
      (`PG_PLIK` w trybie długim), ostatni wolny. Znaczy dwie rzeczy
      naraz, obie negatywne:

      - `fork` ma taką stronę **współdzielić**, a nie oznaczać do
        kopiowania przy zapisie. Oba procesy piszą do jednego pliku,
        więc prywatna kopia u dziecka znaczyłaby, że jego zapisy nigdy
        nie wychodzą poza jego własną pamięć. Test rozwidla się i każe
        pisać dziecku, a sprawdza u rodzica.
      - wymiana stron ma ją omijać. Miejscem takiej strony na dysku
        jest sam odwzorowany plik, a nie plik wymiany; wyrzucenie jej
        do `swap.sys` zgubiłoby i związek z plikiem, i informację
        o tym, że jest brudna.

      Do jądra 64-bitowego trzeba było przy okazji dołożyć **czwarty
      argument wywołania systemowego**. Dotąd wystarczały trzy; teraz
      jedzie w R10, tak jak w Linuksie i z tego samego powodu - RCX
      zabiera sama instrukcja `syscall` na adres powrotu.

      Wyszedł też błąd starszy niż ta zmiana: `exec` w jądrze
      32-bitowym zerował licznik wolnych adresów mapowań, ale zostawiał
      **opisy** odwzorowań starego obrazu. Ich zakresy leżą dokładnie
      tam, gdzie nowy obraz zaczyna zakładać własne, więc błąd strony
      w nowej stercie trafiał na opis pliku, którego ten proces już nie
      otwierał. `task_mmap_zwolnij` istniało od początku i nie było
      wołane z żadnego miejsca - wersja 64-bitowa wołała swoje.
- [x] **xHCI - trzeci kontroler USB** i warstwa, ktora go zaslania.

      UHCI wystarczał do tej pory tylko dlatego, że QEMU go udaje.
      Maszyna nowsza niż mniej więcej 2010 rok nie ma już ani PS/2,
      ani UHCI - jest tylko xHCI. Bez niego "działa na prawdziwym
      sprzęcie" zostałoby na zawsze pustym punktem listy.

      Różnica między tymi kontrolerami nie leży w szczegółach, tylko
      w **podziale pracy**. W UHCI to sterownik układał deskryptory
      przesyłek w liście ramek, którą kontroler przemiatał co
      milisekundę, i sam wymyślał urządzeniom adresy. W xHCI rozmawia
      się poleceniami wkładanymi do pierścienia, odpowiedzi przychodzą
      drugim pierścieniem - zdarzeń - a adresy nadaje kontroler.

      Pierścień to najładniejsza rzecz w tym sterowniku. Jest tablicą
      TRB, której ostatni element jest skokiem na początek, a o tym,
      które wpisy należą już do kontrolera, rozstrzyga jeden bit -
      bit cyklu. Skok ma znacznik, który przy każdym obrocie przewraca
      znaczenie tego bitu na przeciwne. Dzięki temu pierścienia nigdy
      nie trzeba czyścić ani liczyć, ile w nim czego zostało: ten sam
      bit raz znaczy "moje", raz "twoje".

      Dokładanie sterownika ODSŁONIŁO granicę, której wcześniej nie
      było widać. `usbkbd.c` wołał `uhci_*` wprost, ale nie dlatego,
      że rozpoznawanie klawiatury ma cokolwiek wspólnego z kontrolerem
      - po prostu kontroler był jeden. Nowy `usbhc.c` stawia tę
      granicę tam, gdzie kończy się różnica: reset portu, nadanie
      adresu i ustalenie rozmiaru pakietu punktu zerowego wyglądają
      w obu kontrolerach zupełnie inaczej, więc siedzą pod jednym
      słowem `podlacz`. Wyżej zostaje już tylko rozmowa z urządzeniem,
      ta sama niezależnie od magistrali - i dlatego cały kod HID,
      tablice kodów i wątek odpytujący nie zmieniły się ani o linijkę.
      To ten sam wzorzec, co `blk.c` i `netdev.c`, i po raz trzeci
      okazuje się, że da się go postawić dopiero wtedy, gdy naprawdę
      są dwie rzeczy do schowania.

      Przy zapisie do PORTSC trzeba uważać na coś, czego w UHCI nie
      było: część bitów **kasuje się zapisem jedynki**, a bit
      włączenia portu zapisem jedynki się port WYŁĄCZA. Zwykłe
      odczytaj-zmień-zapisz wyłączyłoby więc port przy okazji
      ustawienia czegokolwiek innego. Stąd `portsc_bezpiecznie`.

      Test jest podwójny z rozmysłem. Sprawdza, że klawiatura na xHCI
      w ogóle mówi - ale przede wszystkim, że mówi **tak samo**: ta
      sama grupa sprawdzeń co przy UHCI, ten sam znak, ten sam Shift,
      ten sam Alt+F2. Osobne grupy `usbkbd64` i `xhci` pilnują dwóch
      magistral i jednego wniosku: wyżej nie widać, która jest pod
      spodem.
- [x] **Biblioteka współdzielona i wiązanie symboli przy ładowaniu** -
      koniec z kopią kryptografii w każdym programie.

      `httpget` ważył 82 kilobajty, `gweb` 89, `certtest` 51. Prawie
      wszystko w nich to była ta sama kryptografia i ten sam TLS,
      wkompilowane po raz kolejny - raz w initrd i raz w pamięci,
      osobno dla każdego procesu. Teraz te trzy plus `cryptotest` ważą
      razem mniej niż jedna biblioteka, którą dzielą.

      **Skąd wzięło się źródło biblioteki.** Kryptografia żyje
      w ProtonOS w nagłówkach, jako `static inline`. To była dobra
      decyzja i nie chciałem jej cofać: jedno źródło prawdy, zero
      plików pośrednich. Biblioteka potrzebuje jednak dwóch różnych
      spojrzeń na ten sam plik - z ciałami (żeby je skompilować
      i dało się je w niej odnaleźć po nazwie) i bez ciał (żeby
      program zawierał wywołania, a nie kopie). Rozbicie czternastu
      nagłówków na pary .c/.h ręcznie znaczyłoby cztery tysiące linii
      przepisanych i dwa miejsca, które od tej pory mogą się
      rozjechać. Robi to więc `scripts/rozdziel.py` przy każdym
      budowaniu: dopasowuje klamry, omijając te w napisach
      i komentarzach, i wypluwa obie wersje. Źródło zostaje jedno
      i nietknięte.

      **Dlaczego biblioteka ma stały adres.** Konsolidujemy ją pod
      adresem, pod którym jądro ją potem kładzie w każdym procesie.
      Dzięki temu adresy w jej tablicy symboli są już ostateczne -
      ani ona, ani jądro nie mają przy ładowaniu nic do przeliczania.
      Ceny są dwie i obie akceptowalne: biblioteka może być tylko
      jedna i zawsze widać ją pod tym samym adresem. Prawdziwy system
      tak nie robi, bo chce ich wiele i chce losować adresy - ale
      prawdziwy system ma na to konsolidator w przestrzeni użytkownika,
      a u nas rolę `ld.so` pełni kilkadziesiąt linii w jądrze.

      **Co dokładnie robi jądro.** Czyta z biblioteki tablicę symboli
      dynamicznych (nazwa funkcji -> adres), a z programu listę
      relokacji (adres pustego miejsca w jego tablicy skoków -> nazwa
      funkcji). Złożenie jednego z drugim to całe wiązanie. Wiążemy
      wszystko od razu, przy starcie: leniwe wiązanie wymagałoby, żeby
      kawałek konsolidatora siedział w przestrzeni procesu i był wołany
      z zaślepki PLT - cały osobny mechanizm po to, żeby oszczędzić
      mikrosekundy przy starcie.

      **Podział na wspólne i prywatne.** Segment bez prawa zapisu (kod
      i stałe) dostaje RAMKI WSPÓLNE - te same fizyczne strony widzi
      każdy proces, i to jest cała oszczędność. Segment z danymi musi
      być prywatny, bo biblioteka trzyma w nim swój stan: ziarno
      generatora losowego, ustawienia TLS. Stan jednego procesu nie ma
      prawa być widoczny w drugim. Ogon zer (`.bss`) powstaje przy tym
      pod palcami, tak samo jak sterta - program biorący z biblioteki
      sam skrót SHA-256 nie ma powodu płacić za bufory TLS-a.

      **Dwie rzeczy, które trzeba było wyjąć z nagłówków.** Krzywe
      eliptyczne i wbudowany magazyn zaufania były w nich zmiennymi.
      Program, który sięgnąłby po nie po nazwie, dostałby własną kopię
      (relokacja kopiująca) - i od tej chwili biblioteka liczyłaby na
      jednym egzemplarzu, a program patrzył na drugi. Teraz wystawiamy
      je funkcjami, a jądro odmawia załadowania programu, w którym
      taka relokacja by się pojawiła. Lepiej głośno przy starcie niż
      cicho w połowie uzgodnienia TLS.

      **Pułapka, którą widać tylko w liczbach.** Pierwsza wersja
      raportowała "0 stron wspólnych" i nikomu by to nie przeszkadzało,
      bo wszystko działało. Konsolidator wrzucił `.dynamic` do tego
      samego segmentu co kod, a `.dynamic` jest zapisywalne - więc cały
      segment stał się zapisywalny i jądro słusznie dawało każdemu
      procesowi własną kopię. Współdzielenia nie było ani przez chwilę.
      Stąd wyrównanie do granicy strony w skrypcie konsolidatora
      i stąd ta liczba w komunikacie rozruchowym: bez niej ta pomyłka
      nie miała jak wyjść na jaw.

      **Druga pułapka, i ta wyszła dopiero na teście.** Dane biblioteki
      dostawały początkowo strony pod palcami - ogon zer (`.bss`)
      powstawał dopiero przy pierwszym dotknięciu, jak sterta. TLS
      w jądrze 32-bitowym działał, w 64-bitowym nie. Powód: sprawdzanie
      wskaźników z ring 3 wygląda w obu jądrach inaczej. Jądro
      32-bitowe URZECZYWISTNIA stronę, o którą pyta program;
      64-bitowe tylko sprawdza, czy jest, i odmawia, gdy jej nie ma.
      Bufor, do którego pisze wyłącznie jądro - jak ten na odebrane
      bajty TCP - nie zostaje więc dotknięty przez nikogo i syscall go
      odrzuca. Cały segment danych jest teraz zakładany od razu;
      trzydzieści kilka ramek na proces to cena za jeden mechanizm
      mniej i za to, że oba jądra robią to samo.

      Test nie pyta "czy działa", tylko o rzeczy, których nie da się
      sfałszować. Adres wykonywanego kodu leży POZA obrazem programu -
      i podać go musi sama biblioteka, bo program nieprzemieszczalny,
      który bierze adres jej funkcji, dostaje adres własnej zaślepki
      skoku. Drugi proces widzi ten sam adres kodu (ramki wspólne), ale
      licznik generatora znów od zera (dane prywatne). Zapis do kodu
      biblioteki kończy się błędem ochrony - inaczej jeden program
      podmieniłby funkcję wszystkim naraz.
- [x] **Zamki na wspólnych strukturach jądra** - pierwszy krok, bez
      którego prawdziwego SMP nie da się zacząć.

      Rdzenie wtórne umiały dotąd tylko liczyć na tym, co dostały. Cały
      dzielony między nimi stan to była jedna liczba i jeden zamek -
      i właśnie dlatego dało się o nim myśleć. Żeby kiedykolwiek mogły
      prowadzić zadania, muszą najpierw umieć bezpiecznie **wołać
      jądro**; a jądro musi to wytrzymać.

      Wygaszenie przerwań, które wystarczało od początku, chroni przed
      czymś innym niż zamek i warto to nazwać wprost. Zamek chroni
      przed INNYM RDZENIEM, który naprawdę biegnie jednocześnie.
      Wygaszenie przerwań chroni przed SAMYM SOBĄ: timer wchodzący
      w środek sekcji czekałby na zamek trzymany przez kod, który
      właśnie przerwał - i to na tym samym rdzeniu, jedynym, który
      mógłby go zwolnić. Stąd `spin_lock_irq` robiące obie rzeczy
      w jednej kolejności: przerwania gasną PRZED wzięciem zamka
      i wracają PO zwolnieniu.

      Zamki dostały trzy struktury, po które sięga wszystko inne: mapa
      ramek fizycznych, sterta jądra i konsola. Każda z nich jest jedna
      dla całej maszyny, więc i zamek jest jeden.

      **Próba, która naprawdę coś rozstrzyga.** Komenda `smpmem` każe
      każdemu rdzeniowi przydzielać ramki i bloki sterty trzy tysiące
      razy naraz. Nie pytamy jednak, czy się nie wysypało - na to
      odpowiedź bywa przypadkowa. Każdy przydział dostaje wzorzec
      zależny od numeru jednostki i zaraz potem jest czytany: ramka
      wydana dwóm rdzeniom naraz ma dwóch piszących i wzorzec się nie
      zgadza. Na koniec liczymy ramki i bajty sterty - tyle samo przed
      co po.

      Sprawdziłem, czy ta próba w ogóle może paść, wyjmując zamki
      z powrotem. Bez zamka w mapie ramek: sześćdziesiąt jeden
      niezgodności wzorca i pięćdziesiąt osiem ramek, które nie
      wróciły. Bez zamka na stercie: jądro ginie na błędzie strony pod
      adresem `aaa9a8ab`, bo lista wolnych bloków przestaje trzymać się
      kupy w połowie dzielenia bloku. To jest jedyny sposób, żeby
      wiedzieć, że test czegokolwiek pilnuje.

      **Czego rdzeniowi wtórnemu nadal nie wolno.** Nie ma on tablicy
      wyjątków ani przerwań, więc wolno mu wołać tylko to, co nie śpi
      i nie czeka na sprzęt. Przydział pamięci - tak. Dysk, sieć,
      cokolwiek, co mogłoby sięgnąć po wymianę stron - nie. Granica
      jest dziś taka, a nie inna, i przesunie ją dopiero scheduler na
      rdzeń.

      **Asercja, która była wyścigiem.** Pierwsza wersja testu żądała,
      by KAŻDY rdzeń wziął część pracy. Przeszła, ale na jądrze
      64-bitowym udział wyszedł `[1, 20, 21, 22]`: rdzeń rozruchowy
      prowadzi w tym czasie wątek shella i bywa wywłaszczony przez
      timer, podczas gdy wtórne kręcą się w ciasnej pętli i rozbierają
      zlecenie, zanim zdąży sięgnąć. Asercja mówi teraz to, co jest
      prawdą niezależnie od tego, kto wygra wyścig: pracę wzięło więcej
      niż jeden rdzeń, a wszystkie jednostki policzyły się **dokładnie
      raz**.
- [x] **Rdzeń wtórny dostaje tablicę wyjątków i własny zegar** - drugi
      krok w stronę prawdziwego SMP.

      Obudzony rdzeń był dotąd w dziwnym położeniu: wykonywał kod
      jądra, ale nie miał ani tablicy segmentów jądra (siedział na
      tymczasowej z trampoliny), ani żadnej tablicy wyjątków. Każdy
      jego błąd kończył się potrójnym błędem i **cichym resetem całej
      maszyny** - bez jednego słowa na ekranie. Obie tablice są jedne
      dla całego systemu i po zbudowaniu nikt ich nie zmienia, więc
      wystarczyło je wskazać. Segmentu stanu zadania (TSS) rdzeń wtórny
      NIE ładuje: `ltr` oznacza deskryptor jako zajęty, więc tej samej
      tablicy nie da się załadować dwa razy - a i tak nie ma po co,
      skoro programy ring 3 chodzą na razie tylko na rdzeniu
      rozruchowym.

      **Własny zegar.** Przerwanie z PIT idzie przez kontroler PIC do
      JEDNEGO procesora - rdzeń wtórny nie zobaczyłby go nigdy, a bez
      przerwań nie da się go kiedykolwiek wywłaszczyć. Każdy procesor
      ma za to własny zegar w lokalnym APIC. Jego częstotliwości nie da
      się odczytać, więc trzeba ją zmierzyć: rdzeń rozruchowy puszcza
      licznik APIC i patrzy, ile zdąży odliczyć przez znaną liczbę
      tyknięć PIT. Mierzy tylko on, bo tylko on ma czym.

      **Błąd, który kosztował najwięcej czasu i był tego wart.**
      W trybie długim rdzeń wtórny umierał przy pierwszym sięgnięciu po
      lokalny APIC - cicho, razem z całą maszyną. Powód: trampolina
      ustawiała w rejestrze EFER bit zgody na tryb długi, ale nie bit
      NXE. A jądro mapuje bitem zakazu wykonywania wszystko, co nie ma
      być kodem - w tym rejestry sprzętu doklejone do mapowania
      identycznościowego. Bez NXE bit 63 wpisu tablicy stron nie znaczy
      "zakaz wykonywania", tylko jest **zarezerwowany**: każdy dostęp
      przez taki wpis to błąd strony. Ten sam adres, te same tablice,
      ten sam CR3 - a jeden procesor czyta, drugi ginie. Różnicę robił
      jeden bit w rejestrze, którego nikt nie kopiował.

      Znalezienie tego wymagało zejścia do najprostszego możliwego
      wypisu: jednego bajtu prosto na port szeregowy, bez zamków, bez
      funkcji, bez niczego, co samo może paść. Litera po literze widać
      było, dokąd rdzeń dochodzi.

      **Bezczynny rdzeń ma stać, a nie kręcić się w kółko.** Do tej
      pory rdzeń bez pracy wirował na `pause`. Na maszynie wirtualnej
      kosztuje to dokładnie tyle, co praca: trzy rdzenie kręcące się bez
      celu zabierały rdzeniowi rozruchowemu trzy czwarte maszyny, przez
      co zwykły zapis na dysk trwał cztery razy dłużej. **Rozruch do
      shella na czterech rdzeniach: 22 sekundy z okładem przed zmianą,
      6,5 sekundy po.** Teraz rdzeń staje na `hlt`, a budzi go
      przerwanie międzyrdzeniowe wysłane w chwili złożenia pracy -
      najprostsze przerwanie, jakie można komuś posłać, bo cała jego
      treść to fakt, że przyszło.

      Kolejność ma znaczenie i jest w kodzie napisana wprost: pobudka
      idzie PO zapisaniu zlecenia. Wysłana przed zastałaby jeszcze
      pustą kolejkę.

      **I znów wyścig w asercji.** Odkąd rdzenie śpią, podział pracy
      zależy od tego, jak szybko dojdzie do nich pobudka - a to jest
      sprawa emulatora, nie jądra. Asercja "każdy rdzeń wziął część"
      przestała więc cokolwiek znaczyć i mówi teraz to, co jest prawdą
      niezależnie od wyścigu: pracę wzięło więcej niż jeden rdzeń,
      a wszystkie jednostki policzyły się dokładnie raz. Tego pilnuje
      porównanie z wynikiem jednordzeniowym, co do bitu.
- [x] **Wątki jądra na wszystkich rdzeniach** - to, na co czekały trzy
      poprzednie kroki.

      Rdzeń wtórny miał już tablicę wyjątków, własny zegar i zamki na
      wspólnych strukturach. Brakowało jednej rzeczy, i to najbardziej
      niepozornej: **bieżące zadanie było pojedynczą zmienną globalną**.
      Dopóki nią było, więcej niż jeden rdzeń nie miał dokąd zapisać
      tego, co właśnie prowadzi. Teraz jest ich tyle, ile rdzeni, a
      który jest mój, mówi sam procesor - numer w jego lokalnym APIC.

      Słowo `current` zostało, bo całe jądro mówi tym słowem i nie ma
      powodu, żeby przestało. Zmieniło się tylko to, na co pokazuje:
      z jednej zmiennej na pole bloku rdzenia. Sto trzydzieści dziewięć
      miejsc w każdym z dwóch jąder nie musiało o tym wiedzieć.

      **Zamek trzymany przez przełączenie.** To jest ta część, której
      nie da się zrobić po połowie. Lista zadań jest jedna, więc chroni
      ją jeden zamek - ale nie wystarczy wziąć go na czas wyboru
      następnego zadania i zaraz oddać. Zostawałoby okno, w którym
      poprzednie zadanie jest już oznaczone jako gotowe, a jego stos
      wciąż używany: drugi rdzeń wskoczyłby na stos, po którym pierwszy
      jeszcze chodzi. Zamek trzymamy więc **przez** przełączenie
      kontekstu, a zwalnia go ten, kto po drugiej stronie przyjdzie.
      Nowe zadanie nie wraca z przełączenia jak wszystkie inne, więc
      zwalnia zamek samo, w swojej trampolinie. Zadanie, które się
      kończy, nie wraca nigdy - i nie musi: zrobi to za nie jego
      następca.

      Z tego samego powodu stan bieżącego zadania zmienia się teraz
      **pod zamkiem**, razem z wyborem następnego (`switch_to(stan)`).
      Ustawiony wcześniej dawałby okno, w którym zadanie jest oznaczone
      jako śpiące, a jego stos wciąż zajęty - i drugi rdzeń mógłby je
      w tym czasie obudzić.

      **Czego rdzeniowi wtórnemu nadal nie wolno, i dlaczego to jest
      decyzja, a nie niedoróbka.** Zwykłe zadanie zostaje na rdzeniu
      rozruchowym. Rdzeń wtórny bierze tylko to, co ktoś jawnie na to
      pozwolił przez `task_create_anycore` - bo reszta jądra sięga po
      sterowniki, system plików i wymianę stron, a te pisane były dla
      jednego rdzenia i cicho na to liczą. Segment stanu zadania jest
      jeden i należy do rdzenia rozruchowego, więc ring 3 też zostaje
      u niego. Wołający, który prosi o zadanie „wszędzie", bierze na
      siebie, że jego wątek dotyka tylko tego, co zniesie dwa rdzenie
      naraz.

      Każdy rdzeń dostaje własne zadanie bezczynne - inaczej nie byłoby
      dokąd odłożyć jego kontekstu. Stoi ono na stosie, który rdzeń
      dostał przy obudzeniu, i nikt inny nie ma prawa go prowadzić.
      Widać je w `ps` pod ujemnym numerem: to nie są procesy.

      **Dowód jest trzyczęściowy, bo każda część z osobna dałaby się
      podrobić.** Wątek sam zapisuje, na którym rdzeniu właśnie liczy -
      ale gdyby scheduler tylko przerzucał go między rdzeniami po
      kolei, maska wyglądałaby tak samo. Wynik każdej porcji jest co do
      bitu ten sam, co porcji policzonej w wątku shella - ale to
      dowodzi tylko poprawności. Rozstrzyga dopiero **czas**: cztery
      porcje puszczone równolegle mają zająć wyraźnie mniej niż cztery
      razy tyle, co jedna.

      Liczby są ładniejsze, niż się spodziewałem. W trybie długim jedna
      porcja to 64 tyknięcia, a cztery naraz - **61**. Cztery razy
      więcej pracy w tym samym czasie. Ta sama próba na jednym rdzeniu:
      28 tyknięć jedna, 185 cztery. To jest ta różnica, o którą chodzi,
      i widać ją w teście wprost.
- [x] **Ring 3 na wszystkich rdzeniach** - programy użytkownika
      przestały być własnością rdzenia rozruchowego.

      Trzeba było na to czterech rzeczy. Pierwsza: **segment stanu
      zadania na każdy rdzeń**. Procesor czyta z niego stos jądra przy
      wejściu z pierścienia 3, a każdy rdzeń wchodzi tam z innego
      zadania i na inny stos - jeden wspólny segment znaczyłby, że dwa
      rdzenie wchodzą do jądra na ten sam stos. Deskryptorów też musi
      być tyle, ile rdzeni: instrukcja `ltr` oznacza deskryptor jako
      **zajęty**, więc drugi rdzeń ładujący ten sam selektor dostałby
      wyjątek ochrony.

      Druga: **własne wejście w `syscall`**. W trybie długim rejestry
      wejścia (`IA32_LSTAR`, `IA32_STAR`) są własnością rdzenia i ten,
      który ich nie ustawi, nie przyjmie wywołania systemowego wcale.
      Sam stos jądra bierze się przez `swapgs` i `IA32_KERNEL_GS_BASE`,
      czyli jedyny mechanizm, jaki procesor daje na "to jest mój
      rdzeń": tablica bloków po 16 bajtów, po jednym na rdzeń.

      Trzecia: **wielki zamek jądra**. Reszta jądra - sterowniki,
      system plików, wymiana stron - pisana była dla jednego rdzenia
      i cicho na to liczy. Zamek jest zagnieżdżalny, bo musi: przerwanie
      trafia w środek wywołania systemowego, a rdzeń, który już go
      trzyma, nie może na siebie czekać. Bierze go każde wejście z
      pierścienia 3 i oddaje każde wyjście, a `switch_to` oddaje go **na
      czas przełączenia** - inaczej zadanie usypiające w środku
      syscalla zamykałoby jądro dla wszystkich aż do swojego
      przebudzenia. Głębokość leży na stosie tego zadania, więc każde
      odzyskuje dokładnie swoją.

      Czwarta okazała się prawdziwa i kosztowała dzień. **Rdzeń wtórny
      w trybie chronionym nie miał włączonego stronicowania.**
      Trampolina rozruchowa przechodziła w tryb chroniony i na tym
      kończyła; nikt nigdy nie ustawił mu CR3 ani bitu PG. Dla kodu
      jądra wychodziło to na jedno, bo dolna pamięć jest odwzorowana
      jeden do jednego - i dlatego wątki jądra na rdzeniach wtórnych
      działały bez zarzutu przez cały poprzedni krok. Ale adres programu
      użytkownika (0x40000000 w górę) bez tablic stron nie znaczy nic:
      rdzeń czytał stamtąd to, co leży pod tym adresem **fizycznie**,
      czyli zera.

      Objaw był myląco odległy od przyczyny. Proces ginął pod losowym,
      dzikim adresem, za każdym razem innym - bo `iret` wchodził w
      pierścień 3 na poprawnym adresie, a procesor wykonywał tam zera
      i skakał, gdzie popadnie. Wielki zamek wyglądał na winnego, więc
      go wyłączyłem - i nic to nie dało, co było pierwszą dobrą
      wiadomością. Rozstrzygnął dopiero zrzut, w którym te same dane
      czytane na dwa sposoby dały dwie różne odpowiedzi: przez adres
      wirtualny **zero**, a przez tę samą ramkę odczytaną wprost -
      poprawny kod. Katalog stron się zgadzał, wpis się zgadzał, CR3 się
      zgadzało, `invlpg` i przeładowanie CR3 nic nie zmieniały. Została
      jedna możliwość: tłumaczenie adresów w ogóle nie działa. Naprawa
      to sześć instrukcji w trampolinie i jedno pole na CR3 - dokładnie
      to samo, co rdzeń 64-bitowy dostał od początku, bo bez
      stronicowania tryb długi po prostu nie rusza.

      **Wątek a proces to teraz dwie różne zgody.** Zadanie jądra
      utworzone przez `task_create_anycore` jest *samodzielne*: nie
      bierze wielkiego zamka, bo ten, kto o nie poprosił, wziął na
      siebie, że jego wątek dotyka wyłącznie rzeczy znoszących dwa
      rdzenie naraz. Gdyby brało, cztery wątki liczące równolegle
      liczyłyby się po kolei - a po to ich nie ma. Proces uruchomiony
      przez `runw` ma odwrotnie: jego strona jądrowa chodzi pod zamkiem,
      równolegle idzie samo liczenie w pierścieniu 3, a tam program i
      tak spędza większość czasu.

      **Dowód znów jest porównaniem, nie pomiarem.** `run cores` i
      `runw cores` w tej samej maszynie: ten sam program, ta sama suma
      kontrolna co do bitu, ta sama liczba kroków. Różni je tylko to,
      ilu rdzeniom wolno go prowadzić. W trybie długim 86 tyknięć wobec
      **33** i maska wszystkich czterech rdzeni; w trybie chronionym 42
      wobec **26**. Na maszynie jednordzeniowej `runw` nie zmienia nic
      i tego też pilnuje asercja - obietnica "wolno wszędzie" nie jest
      obietnicą "musi gdzie indziej".
- [x] **Wywołania systemowe bez wielkiego zamka** - pierwszy kęs z tego,
      co poprzedni krok postawił na drodze.

      Zacząć trzeba było od przyznania się do błędu. Sprzątając kod
      diagnostyczny po poprzednim kroku cofnąłem `kernel/isr.c` do
      wersji z repozytorium - razem z opakowaniem, które brało wielki
      zamek przy wejściu z pierścienia 3. Jądro 32-bitowe wjechało więc
      do repozytorium z ring 3 na wszystkich rdzeniach i **bez żadnego
      zamka**, mimo że opis mówił co innego. Wszystkie testy przeszły,
      bo nic w nich nie naciskało na to miejsce dostatecznie mocno -
      i to jest właśnie ten rodzaj zieleni, który nic nie znaczy.
      Zamek wrócił na miejsce w tym kroku.

      **Najpierw pomiar, który pokazał coś zupełnie innego.** Program
      `syscalls` wola jądro milion razy i nic poza tym nie robi. Pod
      wielkim zamkiem dwa procesy na czterech rdzeniach kończyły w 1080
      tyknięciach, a te same dwa na jednym rdzeniu w 594. Cztery
      rdzenie były więc **dwa razy wolniejsze** niż jeden. Ale gdy
      zdjąłem zamek z tego wywołania, poprawa wyszła znikoma - czyli
      zamek nie był tu wcale największym kosztem.

      Był nim `smp_core()`. Całe jądro mówi słowem `current`, a to
      słowo znaczy "zadanie TEGO rdzenia" - więc każde jego użycie
      pytało procesor o numer rdzenia, a pytanie szło **odczytem
      z lokalnego APIC**. To rejestr sprzętowy poza pamięcią podręczną:
      setki cykli na realnej maszynie, a pod emulatorem jeszcze więcej,
      bo dostęp do sprzętu bierze jego własny zamek. Jądro płaciło to
      po kilka razy na każde wywołanie systemowe.

      Numer był jednak przez cały czas pod ręką i za darmo. Każdy rdzeń
      ma **własny segment stanu zadania** - dostał go w poprzednim
      kroku - a jego selektor siedzi w rejestrze zadania procesora.
      Instrukcja `str` czyta go bez ani jednego dostępu do pamięci.
      Zanim rdzeń załaduje swój segment, `str` daje zero, i to jest
      poprawna odpowiedź, bo do tej chwili biegnie tylko rdzeń
      rozruchowy. Sam rozruch segmentów pyta dalej APIC - inaczej
      byłoby to pytanie o numer po to, żeby go dopiero ustawić.

      Efekt: milion wywołań na jednym rdzeniu z 594 tyknięć do 286
      w trybie chronionym i z 288 do 159 w trybie długim. Dwa razy
      taniej, w każdym wywołaniu systemowym i w każdym przerwaniu.

      **Dopiero potem zamek.** Lista wywołań, które nie potrzebują
      wielkiego zamka, jest krótka i jawna: numer procesu, licznik
      zegara, numer rdzenia, uśpienie. Każde z nich dotyka wyłącznie
      stanu wołającego zadania albo rzeczy, które mają własne zamki.
      Domyślna odpowiedź brzmi "z zamkiem" i tak ma zostać - dopisanie
      się do tej listy jest obietnicą, że wywołanie przeczytało się do
      końca, razem z tym, co woła dalej.

      Doręczenie zabicia i sygnału zostaje pod zamkiem, bo jedno kończy
      proces, a drugie przestawia mu stos - ale zamek bierzemy dopiero
      wtedy, gdy naprawdę jest co doręczyć. Bez tego ta ścieżka nie
      różniłaby się od zwykłej niczym poza nazwą.

      **Liczby.** Milion wywołań w każdym z dwóch procesów: w trybie
      chronionym 287 tyknięć po kolei wobec **192** naraz, w trybie
      długim 147 wobec **62**. Przed całą zmianą było w tym miejscu
      wolniej na czterech rdzeniach niż na jednym.

      Test pilnuje jednak nie tylko czasu. Każde z miliona wywołań ma
      zwrócić numer TEGO procesu - a to jest dokładnie ta pomyłka,
      którą zamek do tej pory zasłaniał: dwa rdzenie, które mylą się co
      do tego, które zadanie prowadzą. Dwa miliony zgodnych odpowiedzi
      znaczą więcej niż każdy pomiar czasu.
- [x] **Nazwy po angielsku, testy rozdzielone** - zmiana konwencji, nie
      zachowania.

      Identyfikatory, nazwy plikow i nazwy programow sa teraz po
      angielsku; po polsku zostaje to, co system mowi do czlowieka:
      komunikaty jadra, opisy asercji w testach i ten dziennik. Kod
      pisze sie po angielsku, bo tak sie go czyta.

      Zamiane zrobilem narzedziem swiadomym skladni - dla C wlasnym
      skanerem pomijajacym napisy i komentarze, dla Pythona
      tokenizerem z biblioteki standardowej. Wynik porownalem
      maszynowo z czysta zamiana nazw na wersji z repozytorium: rozne
      mialy byc tylko te pliki, ktore poprawialem recznie, i tylko one
      byly.

      Recznych poprawek bylo cztery i wszystkie z jednego powodu: nowa
      nazwa zderzala sie z istniejaca. Jedna z nich zmieniala
      ZNACZENIE kodu i nie zglosil jej ani kompilator, ani zaden test.
      W sterowniku NVMe bufor DMA z zakresu pliku dostawal nazwe `buf`,
      ktora nosi juz parametr `nvme_write` - i przeslanialby go tak, ze
      zapis szedlby do bufora wolajacego zamiast do DMA. Napisalem
      wiec osobne narzedzie, ktore szuka NOWYCH przeslonien w calym
      drzewie: poza jednym miejscem w ush64 zaden plik zadnego nie
      zyskal.

      Po stronie Pythona byla jeszcze jedna pulapka. Tokenizer oddaje
      caly f-napis jako jeden znacznik, wiec nazwy w klamrach zostaly
      nietkniete i `run_tests.py` wywrocil sie na `sek`. Drugie
      przejscie podmienia je osobno, tylko w nawiasach klamrowych.

      **Testy rozdzielone na pliki po jednej rzeczy.** Trzy grupy byly
      workami: `rdzen` (174 asercje), `rdzen2` i `k64` (795 wierszy).
      Kazda z nich robila wszystko na jednej maszynie, wiec awaria
      wskazywala grupe pokrywajaca trzecia systemu, a nie zepsuta
      rzecz. Zostaly z nich osobne grupy: rozruch, shell, procesy,
      fork, pamiec na zadanie, sygnaly, ochrona, siec, HTTP, mysz,
      ush, system plikow - i po stronie 64-bitowej rdzen trybu
      dlugiego, procesy, shell i dysk. Sterowniki tez sie rozeszly na
      ACPI, klawiature USB, AHCI, konsole i wymiane stron.

      Cena jest uczciwa i widac ja w liczbach: grup jest 49 zamiast 32,
      a caly bieg trwa 361 zamiast 326 sekund, bo kazda grupa musi
      wystartowac wlasna maszyne. Za to najdluzsza grupa skrocila sie
      ze 174 do 87 sekund, a nieprzechodzaca asercja mowi teraz, co
      sie zepsulo.
- [x] **Komentarze po angielsku** - domkniecie poprzedniego kroku.

      Nazwy przeszly na angielski wczesniej, komentarze zostaly po
      polsku i przez chwile plik wygladal tak, jakby pisaly go dwie
      osoby. Teraz po polsku jest tylko to, co system mowi do
      czlowieka: komunikaty jadra, opisy asercji w testach i ten
      dziennik.

      Przeszlo przez to wszystko - kernel64/, user/, user64/, oba
      pliki .S w kernel/, ktore umknely poprzedniemu przejsciu, oraz
      komentarze i docstringi w scripts/. Robilem to narzedziem, ktore
      wycina z pliku same komentarze i wkleja z powrotem tlumaczenia
      pod tymi samymi numerami: dla C wlasnym skanerem, dla Pythona
      tokenizerem z biblioteki standardowej. Raz pomylilem numeracje o
      jeden i nadpisalem naglowek sekcji w t_e1000.py cudzym tekstem -
      widac to bylo od razu, bo w pliku zostal jeden komentarz po
      polsku, ktorego juz nie mialo byc.

      Przy okazji wyszly nazwy, ktore poprzedni pass ominal, bo nie
      byly identyfikatorami w kodzie, tylko nazwami plikow i
      programow: mkmapa, gleboko, shmpisz64, shmczytaj64. Do tego
      argumenty trybow mmapzap (pisz/spr/bez/spr2/widac) i garstka
      identyfikatorow z polskim rdzeniem, ktora przeszla przez sito:
      db_krok, FDK_WOLNY, gotowa, uzyte, uzyty, wszystkie, uz_open,
      uz_time.

      Automatyczna zamiana nazw ma swoja cene i tu tez ja zaplacilem:
      zamiana 'bez' na 'nosync' weszla w polskie napisy runtime i
      zrobila z "bajty bez msync" - "bajty nosync msync". Wylapalem to
      przegladajac diff, nie test. Tego rodzaju szkody zaden test nie
      zlapie, bo tekst komunikatu nie jest niczyja asercja.

      768 asercji, 49 grup, bez zmian w zachowaniu.

- [x] **Zmiana mapowania dochodzi do pozostalych rdzeni (TLB shootdown)**
      - i watki moga wreszcie wyjsc poza rdzen rozruchowy.

      Tablica stron jest pamiecia, a pamiec jest wspolna. Tlumaczenie
      WYLICZONE z tej tablicy juz nie: kazdy rdzen trzyma swoje, w
      podrecznej pamieci, do ktorej jadro nie ma zadnej instrukcji.
      Zmiana wpisu zmienia wiec to, co mowia tablice, i nic w tym, w
      co wierzy sasiad - a sasiad pisze dalej do ramki, ktora jadro
      zdazylo juz zwolnic albo oddac komus innemu.

      Dopoki proces bieglo na jednym rdzeniu, nikt na tym nie cierpial.
      I wlasnie dlatego watki byly trzymane na rdzeniu rozruchowym:
      dziela przestrzen adresowa, wiec to one wchodza w to pierwsze.

      Jedyna droga do cudzej pamieci podrecznej to zapytac jej
      wlasciciela - wyslac przerwanie i POCZEKAC na odpowiedz. To
      czekanie jest cala rzecza. Prosba wyslana i nieodczekana nie mowi
      nic o tym, kiedy zostanie spelniona, a "kiedy" jest dokladnie
      tym, co wolajacy musi wiedziec: za chwile zwalnia ramke.

      Placi za to tylko ten, kto jest winien. Kazdy rdzen oglasza, w
      ktorej przestrzeni stoi, wiec zmiana w przestrzeni, ktorej nikt
      inny nie trzyma, nie wysyla zadnego przerwania - czyli kazdy
      proces jednowatkowy, czyli prawie wszystkie.

      **Maska nie moze byc zla, i to jest cale rozumowanie.** Pytamy
      tylko te rdzenie, ktore mowia, ze stoja w danej przestrzeni, a
      rdzen moze zmienic zdanie w trakcie pytania. Oba kierunki sa
      bezpieczne z tego samego powodu: rdzen, ktory z przestrzeni
      WYSZEDL po policzeniu, uniewaznia adres w przestrzeni, ktorej juz
      nie prowadzi - jedno zmarnowane chybienie, a samo przejscie i tak
      wyrzucilo mu cala pamiec podreczna. Rdzen, ktory WSZEDL po
      policzeniu, musial zaladowac CR3, a to wyrzuca wszystko - nie ma
      jak wejsc, trzymajac cos starszego niz chwila wejscia.

      **Zakleszczenie, ktore to zatrzymalo.** Pierwsze uruchomienie
      zawieszalo maszyne. Rdzen czekajacy na wielki zamek jadra robi to
      z wylaczonymi przerwaniami - wchodzi bramka przerwania - wiec nie
      slyszy nic. Bylo to nieszkodliwe, dopoki jedyne, na co mozna bylo
      czekac, to sam zamek: trzymajacy dziala i predzej czy pozniej
      puszcza. Przestalo byc w chwili, gdy trzymajacy zaczal czekac na
      NAS. Petla czekania na zamek odpowiada teraz w trakcie: jedna
      proba, odpowiedz, kolejna proba.

      **Drugi blad, znaleziony przy okazji i starszy od calej zmiany.**
      Program testowy pokazal 7296 niezgodnosci pamieci - i nie mialy
      one nic wspolnego z TLB. Granica sterty i nastepny wolny adres
      mmap opisuja PRZESTRZEN adresowa, a watki dziela jedna; trzymane
      per zadanie byly kopiowane przy tworzeniu watku i przesuwane
      osobno, wiec dwa watki proszace o pamiec w tej samej chwili
      dostawaly TEN SAM adres, kazdy przekonany, ze jest pierwszy. Blad
      byl bledem od dnia, w ktorym pojawily sie watki - tylko trudno
      bylo go trafic, dopoki proces stal na jednym rdzeniu.

      **Liczby.** Grupa 'tlb' i program 'tlbtest': cztery watki jednego
      procesu, w kazdej rundzie swieza pamiec, czyli swieze zmiany
      mapowan w przestrzeni, w ktorej stoja trzy inne rdzenie.

      | proba | maska rdzeni | zdalnie | odpowiedzi | bledy pamieci |
      |---|---|---|---|---|
      | `run tlbtest`, 4 rdzenie | 1 | 0 | 0 | 0 |
      | `runw tlbtest`, 4 rdzenie | 15 | 124 | 203 | 0 |
      | `runw tlbtest`, 1 rdzen | 1 | 0 | 0 | 0 |

      Maska 15 to cztery rdzenie w JEDNEJ przestrzeni adresowej. Zera w
      wierszu pierwszym i trzecim sa tu rownie wazne, co 124 w drugim:
      mechanizm nie kosztuje nic tam, gdzie nie ma komu mowic.

      Jadro 64-bitowe dostalo ten sam mechanizm i te same liczniki, ale
      nie ma w nim jeszcze watkow - nie da sie wiec wsadzic dwoch
      rdzeni do jednej przestrzeni i test sprawdza tam tylko tyle, ze
      maszyneria stoi i milczy, kiedy ma milczec.

      796 asercji, 50 grup, 385 s.

- [x] **Watki w jadrze 64-bitowym** - i dopiero teraz widac, ze strzal
      w TLB dziala takze tam.

      Poprzedni krok wbudowal w oba jadra rozsylanie zmian mapowan, ale
      pokazac dzialanie umial tylko w 32-bitowym: w trybie dlugim nie
      bylo watkow, wiec nie dalo sie wsadzic dwoch rdzeni do JEDNEJ
      przestrzeni adresowej. Test sprawdzal tam tylko tyle, ze
      maszyneria stoi i milczy - a to zaden dowod.

      Watek to nowe zadanie z TA SAMA przestrzenia i wlasnym stosem.
      Cala trudnosc lezy poza samym utworzeniem: przestrzen musi
      przezyc smierc pierwszego watku (zwalnia ja ten, kto konczy
      ostatni), granica sterty i nastepny wolny adres musza byc jednym
      licznikiem dla wszystkich, a nie kopia u kazdego - dokladnie ten
      sam blad, ktory tydzien wczesniej wyszedl po stronie 32-bitowej.
      Naprawiony tu od razu, zanim zdazyl kogokolwiek ugryzc.

      Przy okazji doszla pamiec anonimowa (SYS64_MMAP): zarezerwowane
      adresy, strony pod palcami - bo z czegos ten stos musi byc.

      **Dwa bledy po drodze, oba starsze od tej zmiany.**

      Pierwszy: watki ginely na "Invalid Opcode" pod adresem, ktory
      okazal sie zwyklym `movdqa %xmm7`. CR0 i CR4 naleza do RDZENIA, a
      SSE wlaczal tylko rdzen rozruchowy - wiec kazdy program uzywajacy
      XMM, ktory trafil na rdzen wtorny, ginal na instrukcji zupelnie
      poprawnej. Blad byl tam od chwili, gdy programy zaczely biegac po
      wszystkich rdzeniach; nie trafil go dotad zaden test, bo zaden
      program 64-bitowy uruchamiany przez 'runw' nie siegal po XMM w
      widocznym miejscu. To jest ta kategoria bledow, ktora nie objawia
      sie wcale, dopoki nie zrobi sie czegos nowego.

      Drugi: watki ginely potem na "General Protection Fault" na
      instrukcji `movaps %xmm0, 0xe0(%rsp)`. Wierzcholek stosu byl
      wyrownany do szesnastu bajtow - i wlasnie o osiem bajtow za
      dobrze. Funkcja w C zaklada stos taki, jak PO instrukcji call,
      czyli z adresem powrotu na wierzchu; na tym opiera wyrownanie
      swoich zmiennych. Stos programu byl juz od dawna ukladany
      poprawnie, ja po prostu nie przepisalem tego minus osiem.

      **Liczby.** 'runw tlbtest64' na czterech rdzeniach: maska rdzeni
      15, 40 kawalkow pamieci sprawdzonych dwa razy, zero bledow, 140
      zmian mapowan, ktore musialy dojsc do innych rdzeni, i 221
      odpowiedzi. 'run tlbtest64' - maska 1 i ani jednego przerwania.
      Te same cztery sprawdzenia po obu stronach, i po obu stronach
      cos znacza.

      809 asercji, 50 grup, 492 s.

- [x] **Bledy stron bez wielkiego zamka** - i pierwszy podsystem, ktory
      dostal wlasny.

      Blad strony to najczestsze wejscie do jadra, jakie jest. Kazdy
      program dotykajacy pamieci przechodzi tedy tysiace razy, a
      wiekszosc tych bledow to nie sa bledy ani nawet praca z nazwy:
      ramka od alokatora, cztery kilobajty zer i jeden wpis w tablicy.
      Wszystkie trzy maja wlasne zamki.

      A mimo to kazde takie wejscie bralo wielki zamek jadra - wiec dwa
      procesy dotykajace WLASNEJ pamieci, w WLASNYCH przestrzeniach
      adresowych, bez niczego wspolnego, i tak szly po kolei.

      Lista przyczyn obslugiwanych bez zamka jest krotka i jawna, tak
      samo jak lista wywolan systemowych: sterta, stos, pamiec
      anonimowa w obszarze mapowan i kopiowanie przy zapisie. Reszta -
      strona z pliku wymiany, strona odwzorowanego pliku i kazdy
      prawdziwy blad - wchodzi jak dotad, bo siega po dysk albo po
      liste zadan.

      **Tablice stron dostaly wlasny zamek.** Byly wlasnoscia wielkiego
      zamka tak dlugo, jak dlugo bral go kazdy blad strony; skoro juz
      nie bierze, potrzebuja swojego. Jeden na cale jadro i z
      rozmyslem: sekcja pod nim to przejscie dwoch (albo czterech)
      poziomow i jeden zapis. Czego pod nim byc nie moze, to dysku -
      wiec wymiana stron bierze go wokol wpisu i puszcza na czas
      odczytu.

      **Dwa rdzenie na TEJ SAMEJ stronie.** To jest nowy wyscig i
      trzeba go bylo obsluzyc wprost: oba widza brak mapowania, oba
      przynosza ramke, i przegrany musi sie dowiedziec, ze przegral -
      inaczej strona zwyciezcy, juz zapisana, zostaje zastapiona pusta.
      Stad mapowanie "tylko jesli nic tam nie ma".

      **Wpis musi byc PUSTY, nie tylko nieobecny.** Strona wyrzucona na
      dysk tez jest nieobecna, a danie jej swiezej strony zer zgubiloby
      to, co lezy na dysku. Osobne pytanie i osobna odpowiedz.

      **Alokator ramek skanowal bitmape od zera przy kazdym
      przydziale.** Przydzielenie N ramek kosztowalo N do kwadratu, i
      to pod zamkiem trzymanym przez caly czas. Program dotykajacy
      dziewieciu tysiecy stron spedzal wiecej czasu na chodzeniu po
      bitmapie, po ktorej juz chodzil, niz na czymkolwiek innym. Kursor
      pamietajacy, gdzie skonczylo sie ostatnie szukanie, zalatwia to
      dla przypadku jednej ramki - czyli dla prawie kazdego wolajacego.
      To akurat pomaga takze na jednym rdzeniu.

      **Czego NIE twierdze.** Pomiar czasu jest tu niestabilny i mowie
      to wprost: w swiezej maszynie 'run faults' dawal 21, 45 i 51
      tykniec, a 'runw faults' 12, 12 i 25. Kierunek jest widoczny,
      liczba nie. Dlatego asercja nie stoi na stoperze, tylko na
      liczniku: jadro liczy, ile bledow obsluzylo ktora droga. Po
      'runw faults' rosnie o 18001 bez zamka i o ZERO z zamkiem - dwa
      procesy po dziewiec tysiecy stron. To mowi wprost to, co stoper
      moze najwyzej zasugerowac.

      Poprawnosc pilnowana jest rownie mocno i to ona nadaje licznikom
      wartosc: kazda strona ma wrocic z tym, co w nia wpisano, a swieza
      ma byc wyzerowana. Ramka wydana dwom procesom naraz wychodzi tu
      jako zla suma.

      830 asercji, 51 grup, 493 s.

      Uwaga na przyszlosc: przy 51 grupach i czterech maszynach naraz
      asercje czasowe w 'proccores' i 'lockless' zaczely raz na jakis
      czas przegrywac z obciazeniem gospodarza (35 wobec 44 tykniec
      przy progu 3/4). Osobno przechodza z zapasem. Prog nie jest zly -
      to pomiar czasu w maszynie wirtualnej stojacej obok trzech innych
      nie jest niczym pewnym.

- [x] **Ustawienia systemu** - maszyna zaczyna pamietac cos o sobie.

      Wszystko, co system o sobie wiedzial, bylo w niego wpisane w
      czasie budowania. Serwer nazw dalo sie zmienic komenda i zmiana
      zyla dokladnie tyle, co maszyna - wylaczenie wystarczylo, zeby
      zapomniec.

      Ustawienie to linia w pliku tekstowym na dysku: nazwa=wartosc, i
      nic wiecej. Zwykly tekst z rozmyslem - da sie go przeczytac przez
      'cat' i zmienic przez 'write', a gdy cos pojdzie nie tak, widac
      co. Jadro czyta ten plik raz, przy starcie, zaraz po systemie
      plikow.

      Zbior nazw jest staly i jawny, i to tez z rozmyslem: kazda z nich
      ma gdzies w systemie miejsce, ktore ja czyta i cos z nia robi.
      Nazwa, ktorej nikt nie czyta, nie jest ustawieniem - jest linia w
      pliku. Stad cztery, a nie czterdziesci: nazwa maszyny (zacheta),
      serwer nazw (siec), kolor zachety (konsola) i tlo pulpitu
      (kompozytor).

      **Okno na pulpicie.** Program 'gsettings' w pierscieniu 3 nie
      trzyma z tego niczego: pyta jadro o i-te ustawienie i prosi jadro
      o zmiane. Jest jedno miejsce, ktore decyduje, czym jest
      ustawienie, i nie jest nim program. Program ma za to te czesc,
      dla ktorej okno w ogole istnieje: ktory wiersz jest wybrany, co
      wpisano i czy zdazylo to trafic na dysk.

      Dwa sposoby zmiany, bo ustawienia sa dwojakie. Liczba z malego
      zbioru - tlo, kolor - przeklikuje sie w wierszu: nie ma czego
      pisac, gdy mozliwosci jest szesc. Tekst - nazwa maszyny, adres
      serwera - wpisuje sie, bo zgadnac go nie sposob.

      **Asercje nie dotycza pliku, tylko SKUTKOW.** Plik z linia
      'nazwa=maszyna' nie dowodzi niczego; zacheta mowiaca 'maszyna>'
      po restarcie dowodzi wszystkiego. Kazde ustawienie sprawdzane
      jest wiec tam, gdzie jest uzywane: nazwa w zachecie, kolor w jej
      barwie, tlo w pikselach pulpitu, a serwer nazw w tym, co mowi
      'ifconfig' - i kazde przez RESTART na tym samym obrazie dysku.

      Okno sprawdzane jest tak samo: ikona je otwiera, klik przeklikuje
      wartosc, Enter zapisuje, a po restarcie pulpit ma inny kolor.

      Po drodze jeden blad wart zapisania, bo pokazuje, jak latwo
      pomylic sie o jedna konwencje: fs_write oddaje zero przy
      powodzeniu, a nie liczbe bajtow. Sprawdzalem "wiecej niz zero",
      wiec kazdy udany zapis meldowal porazke - plik na dysku byl
      poprawny, a komunikat mowil, ze sie nie udalo. Test tego nie
      zlapal, bo testu jeszcze nie bylo; zlapalo oko na wydruku.

      849 asercji, 52 grupy, 483 s.

- [x] **Zamykanie i zmiana rozmiaru okien aplikacji** - pulpit zaczyna
      zachowywac sie jak pulpit.

      Okno, ktore da sie otworzyc i nie da zamknac, nie jest oknem. A
      dokladnie tak bylo: przycisk [x] kompozytora wyjmowal okno spod
      dzialajacego procesu, ktory rysowal dalej - w cos, czego juz nie
      bylo. Zmiana rozmiaru dzialala wylacznie dla okien jadra.

      **Zamkniecie jest PROSBA.** Kompozytor mowi aplikacji, ze jej
      okno jest zamykane, i daje jej czas skonczyc po swojemu. Program,
      ktory odpowiada, konczy sie wlasnym kodem wyjscia, posprzatawszy
      po sobie.

      **Program, ktory nie odpowiada, ginie.** Okno, ktorego nikt nie
      moze zamknac, jest gorsze niz aplikacja tracaca glos - wiec po
      okolo dwoch sekundach pulpit przestaje prosic. Do sprawdzenia
      tego jest osobny program 'gdeaf': otwiera okno i nie slucha
      niczego. Ginie z kodem -9 i o to chodzi.

      **Zmiana rozmiaru** robi oknu nowy bufor tresci i mowi aplikacji
      nowy rozmiar - raz, w chwili puszczenia rogu, a nie przy kazdej
      klatce ciagniecia. Starej tresci nie przenosimy: program zaraz i
      tak przerysuje calosc, a przeciagnieta kopia byloby widac tylko
      do tej chwili.

      Po drodze dwa bledy warte zapisania, oba tego samego rodzaju -
      wspolny plik kompilowany dwa razy:

      - desktop.c jest wspolny dla obu jader, wiec siegniecie po
        task_kill zerwalo linkowanie jadra 64-bitowego, gdzie ta nazwa
        nie istnieje. Slownik zgodnosci dostal ja jako sygnal nie do
        przechwycenia. Zobaczylem to dopiero po pelnym biegu, bo
        wczesniejsze budowanie puscilem z wyciszonym stderr - i to jest
        wlasciwa lekcja z tego bledu.
      - Programy graficzne tez kompiluja sie dwa razy, wiec nowe
        zdarzenia trzeba bylo dodac takze do user64/usys64.h.

      **Asercje czasowe dostaly powtorke.** Grupa 'lockless' i
      'proccores' przegrywaly czasem z obciazeniem gospodarza - przy 53
      grupach i czterech maszynach naraz pomiar czasu w maszynie
      wirtualnej stojacej obok trzech innych nie jest niczym pewnym.
      Prog sie NIE zmienil; zmienilo sie to, ile odczytow trzeba, zeby
      w jeden uwierzyc: gdy pierwszy przegrywa, mierzymy jeszcze raz.

      862 asercje, 53 grupy, 480 s.

- [x] **Dwanascie okien naraz i pasek zadan, ktory je miesci** - koniec
      z limitem, o ktory dalo sie uderzyc samym klikaniem w ikony.

      Kompozytor trzymal szesc okien, a dwa z nich sa jego wlasne. Czyli
      cztery aplikacje - przy siedmiu ikonach na pulpicie. Piata nie
      wstawala i nie mowila dlaczego: win_create oddawalo -1, program
      konczyl sie po cichu, na ekranie nie dzialo sie nic. Najgorszy
      rodzaj granicy: taki, ktorego nie widac, dopoki sie w niego nie
      wejdzie.

      Sama liczba to jedna zmiana, ale sensownie to trzy rzeczy:

      **Wiecej slotow** - dwanascie zamiast szesciu, czyli siedem ikon
      plus jeszcze troche z menu.

      **Kaskada, ktora nie wychodzi poza ekran.** Okna sa stawiane
      schodkiem w prawo i w dol; dosc daleko po tym schodku i okno
      trafia pod pasek zadan albo za prawa krawedz - tam, gdzie nie
      widac go i nie da sie go zlapac. Teraz schodek cofa sie o trzy
      pozycje, dopoki okno sie miesci, a na koniec i tak dociskamy je do
      krawedzi. Cofanie liczy sie z NUMERU SLOTU, a nie z licznika
      otwarc - dzieki temu slot 2 nadal daje (94,54) i wspolrzedne, na
      ktorych stoja starsze testy graficzne, sie nie ruszyly.

      **Pasek zadan, ktory dzieli sie szerokoscia.** Pasek to staly
      kawalek pikseli: 640 minus przycisk "Menu" to 592 px. Dwanascie
      przyciskow po 52 px to 624 - czyli ostatnie dwa rysowaly sie za
      ekranem. Szerokosc przycisku liczy sie wiec teraz z liczby
      otwartych okien i tego, ile miejsca zostalo, a dlugosc napisu z
      tej szerokosci (czcionka ma 8 px). Przy dziewieciu oknach wychodzi
      52 px jak dotad, przy dwunastu 49.

      I najwazniejsze w tym: szerokosc liczy JEDNA funkcja, a pyta o nia
      i rysowanie, i obsluga klikniecia. Dwa miejsca liczace to samo
      osobno rozjezdzaja sie po cichu - przycisk widac tam, gdzie nie
      dziala, i nikt nie umie powiedziec, co jest zepsute. Test celuje
      dokladnie w to: klika w OSTATNI przycisk (ten, ktory istnieje
      tylko dlatego, ze przyciski sie zwezily) i sprawdza, ze ognisko
      dostalo jego okno, a nie sasiad.

      Nowa grupa 'manywin': siedem ikon otwiera siedem okien, menu
      doklada trzy, pasek pokazuje dwanascie przyciskow, zadne dwa sie
      nie nachodza, wszystkie mieszcza sie w ekranie, i klikniecie w
      ostatni, w srodkowy oraz w zwezony ostatni trafia za kazdym razem
      we wlasciwe okno. Asercje sa o pikselach paska, bo to jedyne
      miejsce, gdzie widac jednoczesnie i to, co narysowane, i to, co
      klikalne.

- [x] **Okno na caly ekran** - ostatnia rzecz, ktorej okno nie umialo, a
      ktora robi sie z oknem najczesciej.

      Okno dalo sie przesunac, zlapac za rog, zwinac do paska i zamknac.
      Nie dalo sie go rozciagnac na caly ekran - a to jest to, co czlowiek
      robi z oknem, w ktorym naprawde pracuje. Ciagniecie rogu az do
      krawedzi to nie to samo, i te roznice warto wypisac, bo one sa
      cala trescia tej zmiany:

      **Do paska zadan, ani piksela dalej.** Pasek to jedyna droga do
      pozostalych okien. Okno narysowane na nim byloby oknem, z ktorego
      nie ma wyjscia.

      **Okno PAMIETA, skad przyszlo.** Ten sam przycisk odklada je
      dokladnie tam, gdzie stalo - nie w miejsce rozsadne, tylko w to
      samo. Stad cztery pola w strukturze okna, a nie jedna flaga.

      **Aplikacja dostaje nowy rozmiar.** Okno to nie tylko prostokat,
      ale i bufor tresci, wiec bufor rosnie i leci WEV_SIZE. Program,
      ktoremu by tego nie powiedziano, rysowalby dalej na 330 pikselach
      w oknie szerokim na 638, a reszta byla by pustym szarym polem -
      wygladajacym jak blad rysowania, a bedacym brakiem wiadomosci.

      I to jest to, na czym stoja asercje. 'gsettings' rysuje zaznaczony
      wiersz jako niebieski pasek przez CALA szerokosc swojej tresci -
      wiec ten pasek jest miarka: po maksymalizacji ma siegac prawej
      krawedzi ekranu, po powrocie konczyc sie tam, gdzie konczy sie
      mniejsze okno. Jedno i drugie widac w pikselach.

      Dwie rzeczy przy okazji, obie z gatunku "a co, jesli":

      - Nie ma pamieci na bufor 638x456 (a sterta jadra ma 1 MiB)? Wtedy
        nie dzieje sie NIC: okno zostaje takie, jakie bylo. Polowa
        maksymalizacji - okno wieksze niz jego bufor - rysowalaby cudza
        pamiec na ekranie. resize_content oddaje teraz -1 zamiast
        milczec.
      - Zlapanie zmaksymalizowanego okna za pasek tytulu przywraca mu
        wlasny rozmiar, jak wszedzie indziej - i okno wraca pod kursor w
        tym samym miejscu swojego paska, wiec nie ucieka spod reki.
        Pociagniecie za rog po prostu przestaje byc maksymalizacja: to,
        co wyjdzie z ciagniecia, jest nowym rozmiarem okna.

      Nowa grupa 'maxwin'.

- [x] **Zegar na pasku zadan** - maszyna miala zegar, ale trzeba go
      bylo pytac.

      'date' go drukowal, program mogl go dostac przez wywolanie
      systemowe. Nie bylo go tam, gdzie zegar jest potrzebny - na
      wierzchu, bez pytania. Teraz siedzi na prawym koncu paska i
      pokazuje czas co do sekundy.

      Odczyt zegara CMOS to odczekanie trwajacej aktualizacji i
      czytanie do skutku, az dwa odczyty sie zgodza - a pulpit
      przerysowuje sie ze trzydziesci razy na sekunde. Wiec czas jest
      czytany co szosta klatke (okolo pieciu razy na sekunde) i
      trzymany w buforze. Zadna sekunda przez to nie ginie, a CMOS nie
      jest odpytywany trzydziesci razy na sekunde o cyfre, ktora zmienia
      sie raz.

      Miejsce na zegar jest ODJETE od tego, co dziela miedzy siebie
      przyciski okien - to ta sama funkcja, ktora liczy ich szerokosc.
      Przy dwunastu oknach przycisk ma teraz 41 pikseli zamiast 47.
      Zegar narysowany na przycisku bylby zegarem, ktory jeden przycisk
      chowa.

      Test czyta cyfry Z EKRANU: wzorzec osmiu bajtow kazdego znaku
      szuka sie w tej samej czcionce, ktora rysowalo jadro (tak samo
      dzialaja testy konsoli w trybie graficznym). Maszyna wstaje z
      zegarem ustawionym na staly moment, wiec cyfry musza mowic ten
      moment - a po czterech sekundach o kilka sekund wiecej. Zegar
      narysowany raz i nigdy nieodswiezany przeszedlby pierwsza asercje
      i polegl na drugiej.

      Nowa grupa 'clock'.

- [x] **Terminal, ktory jest terminalem** - i jeden program zamiast
      dwoch.

      To, co bylo, uruchamialo jedna komende i rysowalo to, co
      wypisala. Kazda nastepna kasowala poprzednia, okno mialo 240x150
      niezaleznie od tego, co czlowiek z nim zrobil, nie bylo kursora,
      historii, powrotu do tego, co zjechalo, ani zmiany katalogu.
      Dowodzilo, ze program w pierscieniu trzecim potrafi uruchomic
      inny i przeczytac jego wyjscie. Nie bylo terminalem.

      Terminal to sesja. To, co bylo uruchomione, zostaje na ekranie nad
      tym, co sie wlasnie pisze; zacheta mowi, gdzie jestes; strzalki
      wracaja do tego, co sie pisalo; a ile z tego widac, decyduje
      rozmiar okna, a nie liczba wpisana w program.

      Wiec: pierscien linii (SCROLL, najstarsza wypada - przesuwanie
      czternastu kilobajtow przy kazdym nowym wierszu byloby tym
      drugim sposobem), odbicie zachety i komendy tak, jak sie stalo,
      dopisywanie wyjscia w miare czytania, PgUp/PgDn po tym, co
      zjechalo (z napisem, ze nie patrzymy na koniec - inaczej terminal
      wyglada na zawieszony), strzalki w bok, Home i End, pisanie W
      MIEJSCU KURSORA, cd zmieniajace zachete od razu, potok 'a | b'
      oraz komunikat zamiast ciszy, gdy programu nie ma.

      **I jeden plik zamiast dwoch.** Byly dwa terminale: user/gterm.c
      i user64/gterm64.c, kazdy z wlasnym bledem do poprawienia
      osobno. Zostal jeden, kompilowany dwa razy - dokladnie jak
      wspolne pliki jadra. Rozni sie w nim jedna rzecz, ta, ktora
      naprawde zalezy od trybu: jak uruchomic komende i zlapac jej
      wyjscie. W trybie chronionym jadro ma na to wywolanie (spawn z
      przekierowaniem, a caly potok jednym wywolaniem); w trybie dlugim
      nie ma - wiec terminal sklada to sam z fork, dup2, exec i pipe,
      tak jak robi to powloka. Reszta - pierscien, edycja, historia,
      rysowanie - to jeden kawalek kodu.

      Trzy rzeczy, ktore test zlapal, a oko nie:

      - Po uruchomieniu komendy linia wejscia byla "pusta" tylko przez
        wyzerowanie dlugosci; w buforze dalej stal poprzedni tekst i
        rysowal sie pod kursorem. Pusta linia to taka, ktorej pierwszy
        bajt jest zerem.
      - Napis "^ przewiniete" rysowal sie wprost na pierwszym wierszu i
        oba teksty dzielily te same piksele. Teraz najpierw czysci pod
        soba tlo. Widac to bylo dopiero przy czytaniu ekranu przez
        czcionke - czlowiek widzi "cos tam jest", test widzi
        "co^u????winiete".
      - Napis o przewinieciu ma pietnascie znakow, a czyszczone pod nim
        tlo mialo czternascie - wiec ostatnia litera rysowala sie poza
        wyczyszczonym polem i gubila sie przy krawedzi. Test czytajacy
        napis literka po literce widzi taka roznice; oko nie.

      **Ciemny motyw i angielski.** Terminal ma tlo w kolorze czerni z
      palety i jasny tekst - tak, jak terminal wyglada. Okna pulpitu sa
      szare, wiec widac od razu, ze to nie jest formularz, tylko ekran w
      oknie. Jego wlasne komunikaty sa po angielsku ('no such program',
      'cd: no such directory', '[code N]', '^ scrolled back') - w
      odroznieniu od komunikatow jadra, ktore zostaja po polsku.

      Testy czytaja CYFRY I LITERY Z EKRANU - wzorzec osmiu bajtow
      kazdej komorki szuka sie w tej samej czcionce, ktora rysowalo
      jadro. Wiec asercje mowia o tym, co widzi czlowiek, a nie o tym,
      w co wierzy program. Dwie grupy: 'gterm' (sesja, historia, ruch w
      linii, cd, potok, przewijanie, wiecej linii po rozciagnieciu
      okna) i 'gterm64' (ta sama sesja przez fork, dup2, exec i pipe w
      trybie dlugim).

- [x] **Alt+Tab** - klawiatura dostaje to, co dotad miala tylko mysz.

      Przy dwunastu oknach jedyna droga do okna byla mysz: klik w okno
      albo w jego przycisk na pasku. Klawisze do przelaczania byly w
      sterowniku od dawna - wiedzial, ze Alt jest wcisniety, i wiedzial,
      co to Tab. Nikt tych dwoch rzeczy nie zlozyl.

      Teraz Alt+Tab oddaje ognisko nastepnemu oknu i wynosi je na
      wierzch. Kolejnosc jest kolejnoscia slotow, czyli kolejnoscia
      przyciskow na pasku zadan - wiec dwa nacisniecia zabieraja tam,
      gdzie patrzenie na pasek mowi, ze zabiora. Okna zwiniete sa
      pomijane (ognisko, ktorego nie widac, to klawiatura pisaca w
      nicosc), tak samo dwa okna jadra - nie ma za nimi programu, ktory
      by klawisz przyjal.

      Kod Alt+Tab powstaje w kbdmap.c, wspolnym dla obu jader, jako
      KEY_NEXTWIN - kolejny kod sterujacy obok strzalek. Pulpit lapie go
      przed wszystkim innym; tam, gdzie pulpitu nie ma, nikt go nie
      czyta i na konsoli tekstowej nic sie nie dzieje.

      Nowa grupa 'alttab'. Dwie asercje sa tu wazniejsze od reszty: ze
      okno naprawde WYCHODZI NA WIERZCH (caly jego pasek tytulu widac, a
      nie polowa spod sasiada) i ze KLAWISZE ZA NIM IDA - po Alt+Tab
      wpisana komenda laduje w terminalu, ktory chwile wczesniej
      byl pod dwoma innymi oknami.

- [x] **Kto jest przy maszynie** - dwa poziomy, haslo i sudo.

      Wszystko, co dzialalo, moglo wszystko. Program w pierscieniu
      trzecim mogl nadpisac plik, z ktorego system czyta ustawienia przy
      starcie, skasowac plik wymiany albo zmienic nazwe maszyny - i tak
      samo kazdy, kto cos wpisal. Nie bylo kogo pytac, bo nie bylo
      nikogo.

      Teraz sa dwa poziomy: admin, ktory moze zmieniac system, i zwykly
      uzytkownik, ktory moze cala reszte. Bez grup, bez wlascicieli
      plikow, bez listy uprawnien - dwa poziomy wystarcza, zeby
      powiedziec to jedno, co warto: ze zmiana systemu to inna czynnosc
      niz uzywanie go.

      **Poziom nalezy do procesu i wedruje do dzieci.** Nowe pole w
      strukturze zadania w obu jadrach; fork i exec je dziedzicza.
      Shell jadra nie jest procesem, wiec ma swoj wlasny poziom obok -
      i program uruchomiony z shella dostaje jego poziom, bo watek
      stojacy w przestrzeni jadra jest z tego punktu widzenia shellem.

      **W gore przez haslo, w dol za darmo.** Trzy wywolania systemowe:
      auth, drop, level. Zejscie nizej nigdy nie jest sprawdzane -
      oddanie uprawnienia zawsze wolno.

      **Haslo lezy w pliku jako liczba**, nie jako tekst - i warto
      powiedziec wprost, czym to jest: skrotem na osmiu bajtach (FNV-1a
      z sola), a nie funkcja zrobiona do hasel. Chroni przed
      PRZECZYTANIEM hasla z pliku, ktory kazdy moze obejrzec. Nie
      chroni przed kims, kto zabierze dysk i weznie sie za te liczbe ze
      slownikiem, i nic tu tego nie udaje. Zgadywanie kosztuje czas:
      trzy zle proby i maszyna przez kilka sekund odpowiada "nie",
      nawet nie patrzac - inaczej szescioznakowe haslo jest dla
      programu w ringu 3 kwestia popoludnia.

      **Czego pilnujemy:** ustawien (SYS_CFG_SET i komenda 'set'),
      wlasnych plikow systemu (system.cfg, swap.sys, icons.pos - przy
      otwarciu do zapisu i przy kasowaniu), oraz sygnalow do cudzych
      procesow. Konsola pilnuje tego samego co wywolania systemowe -
      inaczej byloby obejsciem wszystkich regul, ktorych reszta maszyny
      przestrzega. Czytac te pliki moze kazdy: nie ma w nich nic
      tajnego, a zmiana to co innego niz zajrzenie.

      **Trzy miejsca, w ktorych sie o to prosi**, i wszystkie pytaja tak
      samo - gwiazdkami zamiast tekstu:

      - shell jadra: 'sudo <komenda>' (jedna komenda), 'sudo' (sesja do
        'wyloguj'), 'kim', 'haslo'. Zacheta konczy sie '#' zamiast '>',
        gdy jestes adminem - najstarszy sposob powiedzenia tego i taki,
        ktory sie widzi, nie czyta.
      - terminal w oknie: 'sudo <komenda>' zamienia linie w linie hasla.
        Program w ringu 3 nie moze podniesc sobie uprawnien sam - moze
        tylko dostac odmowe i cos z nia zrobic. Robi to, o co chodzi:
        pyta i probuje raz jeszcze, po czym od razu schodzi z powrotem.
      - okno ustawien: zapis wraca z odmowa, wiec okno mowi o tym i
        pyta o haslo pod zmienianym ustawieniem.

      Do tego 'rm' i 'cp' rozrozniaja teraz odmowe od "nie ma pliku" -
      to dwie rozne odpowiedzi i czlowiek robi po nich co innego.

      Dwie nowe grupy: 'users' (konsola: odmowa, ktora naprawde nie
      zmienia wartosci; zle haslo; jedna komenda, nie sesja; blokada po
      trzech probach; zmiana hasla przezywajaca restart; plik bez hasla
      otwartym tekstem) i 'usersgui' (terminal i okno ustawien). Grupa
      'k64shell' dostala te same asercje dla trybu dlugiego, bo to ten
      sam user.c po obu stronach.

- [x] **Terminal, ktory nie zamiera** - wyjscie na zywo i Ctrl+C.

      Terminal uruchamial program i CZEKAL: nic nie rysowal, na nic nie
      odpowiadal i nic nie pokazywal, dopoki program sie nie skonczyl.
      Przy programie, ktory konczy sie szybko, tego nie bylo widac.
      Przy takim, ktory sie nie konczy - a 'spin' jest wlasnie taki -
      zabieral ze soba okno: bez wyjscia, bez sposobu na zamkniecie,
      bez sposobu na zatrzymanie. To nie jest terminal uruchamiajacy
      program, to jest terminal, ktory stanal.

      **Wyjscie czytane w miare pisania.** Program pisze do pliku, a
      terminal na kazdym przebiegu swojej petli czyta to, co jest za
      punktem, na ktorym skonczyl ostatnio. Wiec linie pojawiaja sie,
      gdy program jeszcze dziala, i pojawiaja sie dalej.

      **Czekanie, ktore nie czeka.** Do tego potrzebne bylo nowe
      wywolanie systemowe w obu jadrach: SYS_TRYWAIT - "0, jesli sie
      skonczyl (i tu jest kod), -3, jesli jeszcze biegnie". Bez niego
      program pilnujacy innego musi wybrac miedzy blokowaniem a
      niewiedza. Jadro 64-bitowe dostalo przy okazji seek: bez niego
      czytanie "tego, co nowe" znaczyloby czytanie calosci za kazdym
      razem.

      **Ctrl+C znaczy to samo, co wszedzie.** Klawisz przychodzi jako
      0x03 jak kazdy inny, terminal wysyla SIGINT do programu, ktory
      uruchomil, a program bez wlasnej procedury na tym ginie. Potem
      terminal bierze nastepna komende.

      Jeden blad wart zapisania: plik z wyjsciem nie byl czyszczony
      przed startem, wiec pierwszy odczyt nowej komendy pokazywal
      resztki poprzedniej. Przy czytaniu dopiero na koncu tego nie bylo
      widac - stary plik i tak byl juz nadpisany, zanim ktokolwiek do
      niego zajrzal. Czytanie na biezaco to wywleklo.

      Nowa grupa 'gtermlive': wyjscie widac przed koncem programu, jest
      go coraz wiecej, okno przyjmuje klawisze przy dzialajacym
      programie, Ctrl+C pokazuje sie w sesji i konczy program, po nim
      nic juz nie dopisuje, a nastepna komenda pokazuje swoje wyjscie,
      nie resztki poprzedniego.

- [x] **Rozmowa z programem** - shell w oknie terminala.

      Terminal umial uruchomic program i pokazac, co ten wypisal. Nie
      umial powiedziec mu nic z powrotem: wejscie programu bralo sie
      znikad, wiec wszystko, co czyta, bylo poza jego zasiegiem - zaden
      shell, zaden program pytajacy o cokolwiek, nic interaktywnego.

      Teraz to, co sie pisze przy dzialajacym programie, idzie rura do
      jego standardowego wejscia, a Ctrl+D te rure zamyka - czyli mowi
      czytajacemu, ze nic wiecej nie bedzie. Trzeba bylo do tego trzech
      rzeczy:

      **Sposobu na uruchomienie programu z rura do srodka.** W trybie
      chronionym robi to jadro: nowe wywolanie SYS_SPAWN_TALK tworzy
      potok, daje dziecku jego koniec do czytania jako deskryptor zero,
      a wolajacemu oddaje koniec do pisania jako zwykly deskryptor. W
      trybie dlugim terminal sklada to sam z pipe, fork i dup2 - tam
      wszystko to juz bylo.

      **Wspolnego stdout.** Dzieci shella musza pisac tam, gdzie pisze
      shell. Kazdy deskryptor niesie wlasne polozenie, wiec jedynym
      miejscem, co do ktorego wszyscy sie zgadzaja, jest KONIEC pliku.
      Bez tego kolejna zacheta shella ladowala na tym, co wlasnie
      napisalo jego dziecko, i wyjscie znikalo pod nia. To samo w obu
      jadrach: w 32-bitowym flaga na deskryptorze, w 64-bitowym regula
      "plik pod deskryptorem numer jeden pisze sie na koncu".

      Do tego w jadrze 32-bitowym dziecko dziedziczy teraz przekierowany
      stdout rodzica (exec przekazuje i-wezel), bo inaczej programy
      uruchamiane przez shella pisalyby na konsole tekstowa, ktorej w
      trybie graficznym nikt nie oglada.

      **Konca wejscia, ktory cos znaczy.** Oba shelle uzytkownika
      krecily sie w kolko, gdy czytanie zwracalo zero: 'if (n != 1)
      continue'. Shell czytajacy strumien, ktory sie skonczyl, tez sie
      skonczyl - teraz obydwa konczy zero z read.

      **Przy okazji dwa zrodla falszywych alarmow w testach**, oba
      naprawione bez ruszania tego, co asercje twierdza:

      - Czekanie na zachete przy starcie maszyny mialo pietnascie
        sekund. Pelny bieg startuje cztery maszyny naraz i cztery jadra
        wstajace na jednym hoscie potrzebuja wiecej - a to jest
        czekanie NA ZDARZENIE, wiec dluzszy limit nie oslabia niczego.
        Trzydziesci sekund.
      - Grupa 'lockless' mierzy, czy dwa procesy potrafia byc w jadrze
        naraz. To twierdzenie o MOZLIWOSCI, nie o sredniej: obala je
        dopiero to, co robil wielki zamek - dwa rdzenie wolniejsze niz
        jeden. Wiec pomiar, ktory przegrywa, jest powtarzany (do
        czterech razy) i liczy sie najlepszy, a margines to teraz 0.85
        zamiast 0.75. Jadro 64-bitowe wychodzi na 0.42, 32-bitowe na
        0.68-0.80 - i to jest uczciwy obraz rzeczy: w trybie
        chronionym pod wielkim zamkiem siedzi jeszcze wiekszosc
        wywolan, a bez zamka sa na razie bledy stron.

      I to jest dowod, ze to dziala: shell uzytkownika (ring 3) chodzi
      w oknie terminala, jego komendy sie wykonuja, wyjscie ich dzieci
      jest POD komenda i NAD nastepna zacheta, potok wewnatrz shella
      wewnatrz terminala pisze do tego samego okna, a Ctrl+D konczy
      calosc. Nowa grupa 'gtermsh'; ta sama droga w trybie dlugim
      dopisana do 'gterm64'.

- [x] **Schowek** - tekst, ktory przechodzi z okna do okna.

      Kopiowanie i wklejanie to najprostsza rzecz, jaka dwa programy
      robia razem - i jedyna, ktorej nie zrobia same: zaden nie moze
      dotknac pamieci drugiego i zaden nie wie, ze ten drugi istnieje.
      Wiec tekst musi czekac w miejscu nalezacym do obu, czyli w jadrze.

      Piecset dwanascie bajtow tekstu i dwa wywolania systemowe, w obu
      jadrach. Bez formatow, bez wlascicieli, bez negocjowania, co
      znaczy zaznaczenie - tyle wystarcza, zeby nazwe wpisana w
      terminalu wkleic do okna ustawien, a wszystko ponad to byloby
      protokolem, ktorego tu jeszcze nikt nie potrzebuje.

      Klawisze najstarsze z mozliwych: Ctrl+Insert kopiuje, Shift+Insert
      wkleja. Nie Ctrl+C - w terminalu Ctrl+C znaczy "zatrzymaj ten
      program" i nic innego. Insert jest jedynym klawiszem rozszerzonym,
      ktorego znaczenie zalezy od tego, co jest wcisniete razem z nim,
      wiec rozstrzyga sie to tam, gdzie ten stan jest znany - w
      kbdmap.c, wspolnym dla obu jader.

      Siega po niego terminal, okno ustawien i OBA shelle jadra: schowek
      nalezy do systemu, a nie do jednego okna. Widac to najlepiej na
      konsolach tekstowych - linia skopiowana na pierwszej wkleja sie na
      drugiej, ktora ma wlasny bufor i wlasny shell.

      Nowa grupa 'clip'. Najwazniejsza asercja jest ta ostatnia: okno, z
      ktorego tekst pochodzil, zostaje ZAMKNIETE, a tekst dalej sie
      wkleja - bo nigdy nie byl tego okna.

- [x] **Zaznaczanie mysza** - schowek siega wreszcie po to, co napisal
      program.

      Schowek umial wziac linie, ktora sie WPISUJE. Nie umial wziac
      niczego, co napisal program - czyli wiekszosci tego, co jest w
      terminalu, i akurat tej czesci, ktora czlowiek chce skopiowac:
      adresu, liczby, nazwy pliku z listingu.

      Zaznaczanie potrzebuje czegos, czego kompozytor nie mowil. Klik
      mowi, GDZIE cos sie stalo; zaznaczenie jest tym, co sie stalo
      MIEDZY dwoma miejscami. Wiec pulpit wysyla teraz trzy zdarzenia
      dla przeciagniecia wewnatrz tresci okna - przycisk w dol, ruch z
      trzymanym przyciskiem, przycisk w gore - we wspolrzednych tresci,
      tak samo jak klik. Co one znacza, jest sprawa aplikacji:
      kompozytor nie wie nic o zaznaczeniach i nie musi.

      Terminal zamienia je na zakres w SWOIM PIERSCIENIU LINII, a nie na
      ekranie - dzieki temu zaznaczenie zostaje na swoim tekscie, gdy
      widok przewija sie pod nim. A puszczenie przycisku kopiuje: w
      terminalu zaznaczenie JEST kopiowaniem od czterdziestu lat i nie
      ma zadnego drugiego klawisza do nacisniecia.

      Nowa grupa 'gtermsel': zaznaczony kawalek ma pod soba pole (a
      sasiedni nie ma), wklejone jest dokladnie to, co bylo pod mysza,
      zaznaczenie przez dwie linie bierze obie, a samo klikniecie
      zaznaczenie kasuje.

- [x] **Wyglad pulpitu** - glebia zamiast plaskich prostokatow.

      Wszystko na ekranie bylo rysowane jednym wywolaniem: wypelnij ten
      prostokat tym kolorem. Dzialalo i tak wygladalo - plaskie tlo,
      plaskie okna z czarna obwodka, kafelki z napisem pod spodem. Nie
      bylo czym powiedziec, ktore okno slucha, ani ze okno jest NAD
      pulpitem, a nie w nim, ani co jest na ikonie poza jej podpisem.

      Ekran ma osiem bitow na piksel i wlasna palete, wiec odcien
      kosztuje jeden indeks i nic wiecej. Z tego zrobiona jest cala ta
      glebia:

      **Swiatlo pada z gory z lewej** - i pada tak wszedzie. Krawedz
      jasna u gory i po lewej, ciemna u dolu i po prawej, to okno
      wypukle; odwrocenie tych dwoch linii to okno wglebione. Stad
      przycisk, ktory wystaje, i pole zegara, ktore jest wpuszczone w
      pasek - i stad przycisk okna z ogniskiem, ktory jest WCISNIETY,
      a nie tylko innego koloru.

      **Cien pod oknem**: nie czarny, tylko przyciemnione tlo - czern na
      ciemnym pulpicie jest dziura, a nie cieniem.

      **Tlo to szesnascie pasm**, ciemniejacych ku dolowi. Szesnascie, a
      nie szesc, bo szesc daje sie policzyc okiem: stopnie musza byc
      dosc male, zeby to bylo zbocze, a nie schody. Zakres jest waski
      celowo - maszyna ustawiona na morskie ma wygladac morsko z kazdego
      miejsca ekranu.

      **Pasek tytulu w dwoch kolorach, i w innej parze, gdy okno nie ma
      ogniska.** To jest zmiana funkcjonalna, nie ozdoba: przedtem nie
      dalo sie zobaczyc, ktore okno przyjmie klawisz. Teraz widac to z
      drugiego konca ekranu.

      **Ikony maja rysunki.** Kolorowy prostokat z wyrazem pod spodem to
      etykieta, a nie ikona - nic nie mowi, dopoki nie zostanie
      przeczytana. Obrazek rozpoznaje sie przed przeczytaniem i po to
      wlasnie sa ikony. Dwanascie na dwanascie pikseli to nie jest duzo
      miejsca, wiec kazdy z nich to jedna mysl bez szczegolow: okno,
      tarcza zegara, teczka, slupki wykresu, znak zachety, kula ziemska,
      kolo zebate.

      Nowa grupa 'look'. Wygladu nie da sie zapisac jako "ladny", wiec
      asercje mowia o STRUKTURZE, ktora oko czyta: tlo ciemnieje ku
      dolowi stopniami, ktorych nie widac; okno rzuca cien ciemniejszy
      od tla obok; krawedzie sa jasne u gory i ciemne u dolu; przycisk z
      ogniskiem ma to odwrocone; niebieski pasek tytulu jest dokladnie
      jeden i przenosi sie klikiem; ikona ma na sobie ciemne piksele, a
      dwie ikony maja rozne.

      Poprawek w starych testach bylo sporo i wszystkie tego samego
      rodzaju: sondy trafialy w piksel, ktory zmienil znaczenie -
      pasek tytulu okna bez ogniska nie jest juz niebieski, przyciski
      paska maja teraz krawedzie, a napisy przesunely sie o piksel, zeby
      krawedz nie przecinala ostatniego rzedu liter. Zadna z tych
      asercji nie zostala oslabiona: kazda mowi to samo co przedtem, w
      miejscu, w ktorym to teraz widac.

- [x] **Plasko i czysto** - to samo powiedziane kolorem zamiast reliefem.

      Poprzedni krok dodal glebie: swiatlo z gory z lewej, cienie pod
      oknami, wypukle przyciski, wglebione pola. Dzialalo, ale wygladalo
      jak system sprzed trzydziestu lat - a chcemy nowoczesnie i czysto.
      Wiec relief poszedl, a zostalo to, co mowil:

      **Powierzchnia to jeden kolor.** Zadnych gradientow na przyciskach,
      zadnych krawedzi jasnych u gory i ciemnych u dolu. To, co dzieli
      dwie powierzchnie, to jedna linia o odcien ciemniejsza.

      **Cieni nie ma.** Ani pod oknem, ani pod ikona, ani pod menu.

      **Akcent jest jeden i mowi jedna rzecz**: ktore okno slucha. Pasek
      tytulu i przycisk na pasku zadan tego okna sa niebieskie, reszta
      jest szara. Skoro to jedyna rzecz, ktora krzyczy, to nic z nia nie
      konkuruje.

      **Przyciski w pasku tytulu to same znaki** - bez ramek dookola. Co
      robia, mowi znak; ramka dokladalaby tylko tuszu. Wyjatkiem jest
      zamkniecie, ktore zostaje czerwone, bo akurat ono ma byc nie do
      pomylenia.

      **Ikony to plaskie kafelki z bialym rysunkiem.** Bez cienia, bez
      krawedzi - ksztalt w kolorze akcentu i tak rozni sie od pulpitu
      dosc, zeby byc rzecza.

      Zostalo jedno z poprzedniego kroku, bo nie jest reliefem tylko
      przestrzenia: tlo w szesnastu pasmach, ciemniejacych ku dolowi.

      Grupa 'look' mowi teraz to, co widac naprawde: wnetrze okna jest
      JEDNYM kolorem (cztery odczyty, wszystkie rowne - inaczej byloby
      to gradientem albo krawedzia), dzieli je od pulpitu jedna linia,
      pod oknem nie ma nic, przycisk z ogniskiem ma kolor akcentu i jest
      plaski (gora i dol tego samego koloru), a kafelek ikony jest
      plaski i ma na sobie biale piksele rysunku.

- [x] **Obrazki na pasku zadan i pasek do ustawienia.**

      Pasek nosil nazwy, przyciete do szesciu liter, wiec "Ustawienia"
      czytalo sie "Ustawi", a dwa terminale czytaly sie tak samo. Nosil
      je dlatego, ze okno zna swoj tytul i nic wiecej - nie wie, w co
      kliknieto, zeby je uruchomic.

      Teraz pulpit pamieta. Uruchamiajac cos z ikony zapisuje, z ktorej
      ikony to bylo; okno, ktore ten proces otworzy chwile pozniej,
      dostaje ten sam obrazek. Nic innego tych dwoch rzeczy nie polaczy:
      kompozytor widzi pid, a ikony sa jedynym miejscem, gdzie program
      ma twarz. Okna jadra i wszystko uruchomione z shella dostaja
      domyslny znak okna.

      Pasek urosl z dwunastu pikseli na osiemnascie, bo to, co na nim
      teraz jest, to obrazki, a nie slowa: dwunastopikselowy rysunek
      potrzebuje miejsca dookola. Przy okazji przycisk zmiescil sie w
      jednej trzeciej tego, co zajmowala przycieta nazwa - dwanascie
      okien miesci sie teraz BEZ zwezania.

      **I pasek stal sie sprawa gustu, a nie losu.** Dwa ustawienia,
      trzymane na dysku jak reszta:

      - 'pasek' - 100, 75 albo 50 procent szerokosci ekranu. Wezszy niz
        ekran jest wysrodkowany, i to wlasnie robi z niego dok.
      - 'ikony' - 0 od lewej, 1 na srodku, 2 dosuniete do zegara.

      Czyta je rysowanie i czyta je obsluga klikniecia - te same dwie
      funkcje, wiec przycisk klika sie dokladnie tam, gdzie go widac,
      w kazdym z tych ustawien. To jest ta asercja, ktora w nowej
      grupie 'bar' znaczy najwiecej; reszta mowi, ze pasek naprawde ma
      te szerokosc, ze obrazki naprawde sie przesuwaja i ze dwa okna
      roznych programow maja rozne obrazki, a dwa okna jadra ten sam.

      Okno ustawien umie te dwa ustawienia przelaczac klikiem: 'ikony'
      jak kazde inne cykliczne, a 'pasek' po swojemu - bo to nie jest
      ciag liczb, tylko trzy z nich. Samo okno uroslo, bo szesc ustawien
      z opisem i wierszem o tym, co sie stalo, przestalo sie miescic w
      stu szescdziesieciu pikselach.

      Do tego jeszcze jedna asercja czasowa - "cztery porcje naraz
      szybciej niz po kolei" w grupie 'kthreads' - dostala to samo, co
      dostal wczesniej 'lockless': przegrany pomiar jest powtarzany i
      liczy sie lepszy stosunek. Prog stoi tam, gdzie stal (polowa
      drogi miedzy jednym a czterema); zmienilo sie tylko to, ile
      odczytow trzeba, zeby w jeden uwierzyc na hoscie, na ktorym stoja
      cztery maszyny naraz.

      Przy okazji dwie asercje w starych testach przestaly liczyc
      polozenie wiersza i zaczely go SZUKAC w pasie, w ktorym moze byc:
      to, gdzie program pisze "zapisane na dysku", zalezy od liczby
      ustawien i dlugosci opisu jednego z nich - czyli od ukladu, ktory
      jest sprawa programu. Komunikat jest sprawa testu.

- [x] **Menu z obrazkami, nazwa okna pod kursorem, tapeta z pliku.**

      Trzy rzeczy w tym samym duchu: pulpit ma mowic bez czytania, a
      tam, gdzie czytanie jest potrzebne, ma je podac na zawolanie.

      **Menu ma obrazki.** Te same, co ikony i co przyciski na pasku -
      program, ktory wyglada na trzy rozne rzeczy, jest trzema rzeczami
      do zapamietania. Wpisy nosza teraz nazwe spod ikony ("Zegar"), a
      nie nazwe programu ("gclock"), bo nazwa programu jest dla shella.
      Wpis pod kursorem dostaje kolor akcentu: bez tego menu jest lista
      slow, ktora akurat da sie kliknac.

      **Nazwa okna wraca pod kursorem.** Obrazek mowi, KTORY program;
      nie mowi, ktory z dwoch terminali. Wiec nad przyciskiem, na
      ktorym stoi kursor, pojawia sie nazwa okna - i tylko wtedy.
      Rysowana jest na samym koncu klatki, po menu, bo to, co objasnia,
      musi byc nad tym, co objasnia.

      **Tapeta jest plikiem.** Tlo bylo kolorem, potem szesnastoma
      pasmami koloru. Obrazkiem byc nie moglo, bo obrazek musi skads
      pochodzic - a maszyna bez aparatu i bez edytora ma tylko
      arytmetyke. Wiec obrazek liczy program 'mkwall' w pierscieniu
      trzecim, zapisuje na dysk, a kompozytor czyta go przy starcie.

      Format to cala mysl: "PWAL", rozmiar, 192 kolory po trzy bajty i
      jeden bajt na piksel. Maly z rozmyslem - 160 na 120, rozciagane
      na ekran. Pelnoekranowy obraz to trzysta kilobajtow, a sterta
      jadra ma megabajt; to, co tu lezy, to jedna pietnasta tego, i
      kazdy jego piksel przykrywa kawalek ekranu. Wyglada na to, czym
      jest - na obrazek, miekko. Kolory tapety siedza NAD kolorami
      pulpitu (indeksy od 64), wiec tapeta nie ma jak przemalowac okna.

      Cztery wzory, bo tyle jest warte jedno popoludnie arytmetyki:
      zbocze, pierscienie, plecionka i krata - kazdy w szesciu barwach.

      Nowa grupa 'wall'. Najlepsza asercja jest ta, ktora odroznia
      obrazek od gradientu nie wiedzac, co jest narysowane: gradient ma
      w poprzek wiersza JEDEN kolor, a obrazek ma ich wiele. Do tego:
      plik ma rozmiar wynikajacy z formatu, jadro mowi przy starcie, co
      wczytalo, okno na tapecie ma nadal swoj kolor, a tapeta, ktorej
      nie ma, nie psuje niczego - wraca gradient.

- [x] **Ikony pulpitu z pliku** - i koniec z lista wpisana w jadro.

      Siedem programow, ustalonych w chwili budowania jadra. Dodanie
      osmego znaczylo edycje kernel/desktop.c i zbudowanie jadra - czyli
      tyle samo, co "nie da sie dodac".

      Teraz lista to plik na dysku, jeden wiersz na ikone:

          program  napis  numer-obrazka

      i nic wiecej. Zwykly tekst, jak wszystko, co ten system pamieta -
      da sie przeczytac przez 'cat' i, co wazniejsze, DA SIE ZROBIC NA
      TEJ MASZYNIE: shell w terminalu ma przekierowania, wiec

          echo gterm Terminal 4 > pulpit.ikony
          echo gweb Internet 5 >> pulpit.ikony

      wystarczy. Format, ktorego nikt na maszynie nie umie wyprodukowac,
      jest formatem dla czyjejs innej maszyny.

      Wiersz, ktory sie nie parsuje, jest pomijany; plik pusty albo bez
      sensownych wierszy zostawia wbudowana siodemke. Pulpit bez ikon to
      maszyna, w ktora nie ma w co kliknac, i zaden zepsuty wiersz nie
      powinien miec takiej wladzy.

      Nowa grupa 'icons' buduje plik dokladnie tak, jak zrobilby to
      czlowiek - przez 'run sh' i echo z '>' oraz '>>' - a potem czyta z
      EKRANU, ze kafelki maja napisy i obrazki z pliku, ze jest ich tyle,
      ile wierszy, i ze klik w taka ikone uruchamia jej program.

      Przy okazji dwie dziury w samym harnessie testowym, obie tego
      samego rodzaju: QEMU nie zna klawisza 'T' ani '#'. Wielka litera to
      shift i litera, a '#' to shift i trojka - bez tego wpisywany tekst
      po cichu gubil znaki, i widac to bylo dopiero jako "erminal"
      zamiast "Terminal" w pliku, ktory test sam napisal.

- [x] **Alt+F4 i menu pod prawym przyciskiem** - dwa gesty, ktore kazdy
      zna, a ktorych ten pulpit nie mial.

      Klawisze byly na miejscu od dawna: Alt z klawiszem funkcyjnym
      przelaczal konsole tekstowe. Tyle ze na pulpicie nie ma czego
      przelaczac - ekran jest jeden - wiec te nacisniecia szly donikad.
      Teraz, gdy ktos przejal klawiature (a na pulpicie przejmuje ja
      kompozytor), te same klawisze ida do niego, i F4 znaczy to, co
      znaczy wszedzie od trzydziestu lat.

      Zamyka DOKLADNIE tak, jak [x] w rogu okna: program dostaje
      pytanie i konczy sie sam, ze swoim kodem wyjscia. Wyrwanie okna
      spod dzialajacego procesu byloby czyms innym niz to, co robi
      mysz, a jeden gest i drugi maja robic to samo - inaczej sa dwie
      rzeczy do zapamietania zamiast jednej.

      Prawy przycisk na pustym pulpicie otwiera menu tam, gdzie stoi
      kursor. To ta sama lista, co pod przyciskiem "Menu" w rogu, bo
      druga lista bylaby druga rzecza do nauczenia - tylko przychodzi
      do reki, zamiast kazac rece isc w rog. Menu nie wychodzi poza
      krawedz ekranu i nie wchodzi na pasek zadan: przy kliknieciu w
      rogu zjezdza w lewo i w gore. Na oknie prawy przycisk nie otwiera
      niczego (wlasne menu okna to sprawa okna, a zadne go nie ma) i
      zamyka to, ktore bylo otwarte.

      Droga klawiszy nie jest wspolna - long mode ma wlasny sterownik
      klawiatury - wiec Alt+F4 trzeba bylo przepuscic przez niego
      osobno. Nowa grupa 'winkeys' sprawdza oba jadra: ze program
      zamkniety Alt+F4 konczy sie SAM z kodem zero (a wiec byl pytany),
      ze okno znika z ekranu i z paska zadan, ze menu stoi pod kursorem
      a nie nad przyciskiem "Menu", ze pozycja pod kursorem sie
      podswietla, ze wybrany z niego program naprawde chodzi (zegar
      rysuje cyfry, nie samo okno sie pojawia) i ze przy krawedzi menu
      sie miesci.

- [x] **Okno przyciagane do krawedzi** - czyli dwa okna obok siebie
      jednym ruchem.

      Dwa okna obok siebie znaczylo dotad dwa przeciagniecia i dwa
      ciagniecia za rog, na oko - a oko jest kiepskie w polowach. Teraz
      okno dociagniete do lewej krawedzi bierze lewa polowe, do prawej
      prawa, a do gory caly ekran. Nie ma tu czego sie uczyc: okno idzie
      tam, gdzie sie je popchnelo.

      Dopoki przycisk jest wcisniety, miejsce, ktore okno ZAJMIE, jest
      rysowane jako obrys w kolorze akcentu. Okno, ktore po puszczeniu
      skacze gdzie indziej bez uprzedzenia, to okno, ktore uciekło -
      obrys jest pytaniem, a puszczenie przycisku odpowiedzia.

      Decyzja zapada z polozenia OKNA, nie kursora. Kursor moze byc juz
      przy samej krawedzi, gdy okno jeszcze do niej nie doszlo (ciagniete
      okno nigdy nie wychodzi poza ekran), a czlowiek celuje oknem.
      Kursor mowi tylko, o ktora krawedz chodzilo - inaczej okno na tyle
      szerokie, ze dotyka obu naraz, przyciagaloby sie gdziekolwiek je
      puscic. Rogi daja polowe, nie caly ekran: dwa okna obok siebie to
      to, po co sie tam celuje, a caly ekran jest i tak jeden przycisk
      dalej.

      Rozmiar sprzed przyciagniecia jest pamietany tak samo, jak przy
      maksymalizacji, wiec przycisk [] oddaje oknu jego wlasny rozmiar -
      okno trzymane na polowie ekranu nie stracilo swojego rozmiaru
      bardziej niz zmaksymalizowane.

      Nowa grupa 'snap' sprawdza, ze w srodku ekranu obrysu nie ma, ze
      przy krawedzi jest i konczy sie nad paskiem zadan, ze okno naprawde
      zajmuje polowe (szare z jednej strony, pulpit z drugiej), ze
      program przerysowuje sie w nowym rozmiarze, ze [] oddaje wlasna
      szerokosc (360, a nie 320 czy 640) i - na koniec - to, po co to
      wszystko: terminal na lewej polowie, ustawienia na prawej, granica
      dokladnie w polowie.

- [x] **Cztery pulpity wirtualne** - najstarsza odpowiedz na ekran,
      ktoremu skonczylo sie miejsce.

      Zamiast tuzina okien na jednej kupie - cztery male kupki, miedzy
      ktorymi sie przelacza. Przelaczenie nie kosztuje nic: nic sie nie
      rysuje, nie przesuwa i nie zmienia rozmiaru. Cala rzecz to jedno
      pytanie - here(i) - ktore zadaje wszystko, co rysuje okno, liczy
      okna albo szuka okna pod kursorem. Okno na innym pulpicie nie jest
      SCHOWANE, tylko go tam nie ma; dziala i rysuje do swojego bufora
      tak samo jak przedtem.

      Ctrl+Alt i strzalka zmienia pulpit - strzalka pokazuje, dokad sie
      idzie. Z Shiftem okno z ogniskiem idzie razem, bo okno zostawione
      za soba to okno szukane. Idziemy za nim, zamiast wypchnac je poza
      wzrok i zostac.

      Ktory pulpit widac, pisze sie na prawym koncu gornej belki: jedna
      komorka na pulpit, ta widoczna w kolorze akcentu, pusta - szarym
      przygaszonym. "Gdzie jestem" i "czy tam cos jest" to jedno
      spojrzenie, a nie dwa. Komorka jest tez przyciskiem.

      Co sie konczy przy przelaczeniu: ciagniecie okna, ciagniecie ikony,
      zmiana rozmiaru i otwarte menu. Ciagniecie, ktore przezywa
      przelaczenie, upuszcza okno na pulpicie, na ktory nikt nie patrzy.

      Przy okazji jedna rzecz, ktora zlapala dopiero pelna suita:
      komorka na gornej belce lezy dokladnie tam, gdzie ma przyciski
      okno rozwiniete na caly ekran. Belka jest rysowana POD oknami,
      wiec i klikniecia musi brac pod nimi - inaczej [] okna
      zmaksymalizowanego przelaczalo pulpit. Jedno pytanie -
      window_at(x,y), zadawane od wierzchu stosu w dol - odpowiada teraz
      i belce, i prawemu przyciskowi myszy.

      Nowa grupa 'desks' (19 asercji) sprawdza komorki na belce, to ze
      okno znika z ekranu I z paska po przejsciu obok, ze nowe okno
      otwiera sie tam, gdzie sie patrzy, ze klik w komorke przenosi, ze
      Shift zabiera okno - i najwazniejsze: ze zegar zostawiony na
      niewidocznym pulpicie po powrocie pokazuje czas pozniejszy o tyle,
      ile nas nie bylo. Program, na ktorego nikt nie patrzy, dalej
      chodzi.

- [x] **Edytor tekstu w oknie** - bo system, ktory umie zrobic plik, ale
      nie umie go ZMIENIC, to maszyna do przechowywania tekstu, a nie do
      pracy.

      Wszystko, co dotad robilo plik - 'write' z shella, 'echo' z '>' -
      bylo jednym wierszem wpisanym przy zachecie, a wiersz wpisany przy
      zachecie nie moze wrocic do tego, co mowil chwile temu.

      Wiec: biala kartka, czarny tekst, kursor tam, gdzie kaza strzalki
      albo mysz, Enter dzielacy wiersz i Backspace laczacy dwa (jedyna
      droga powrotna po Enterze wcisnietym przez pomylke), Ctrl+S i
      Ctrl+O. Schowek ten sam, co reszta pulpitu: Ctrl+Insert kopiuje
      wiersz z kursorem, Shift+Insert wstawia w kursor - wiec tekst
      chodzi miedzy edytorem a terminalem, choc zaden z nich nie wie o
      drugim.

      Pasek stanu na dole jest calym interfejsem: nazwa pliku, gwiazdka
      gdy sie zmienil, wiersz i kolumna, a gdy edytor potrzebuje nazwy -
      pytanie, zadane w tym samym pasku. Okienko dialogowe byloby drugim
      oknem do przesuwania, ogniskowania i zamykania, dla jednego
      wiersza tekstu.

      Dwa nowe kody klawiszy: Ctrl+S i Ctrl+O. Pozostale Ctrl+litera
      przychodza jako sama litera (tego chce shell), ale program, ktory
      pisze pliki, musi umiec odroznic "zapisz" od "s". Jadro nie wie nic
      wiecej niz to, ze klawisze padly - co z nimi zrobic, to sprawa
      programu.

      Kursor to wlos MIEDZY znakami, nie blok i nie belka w komorce:
      jedno i drugie zaslania wlasnie to, na co czlowiek patrzy, gdy
      pisze. Test zlapal to od razu - czytal z ekranu 'Ala ma kota?',
      bo kursor zjadal kolumne litery.

      Nowa grupa 'gedit' (19 asercji, oba jadra) czyta z ekranu tekst i
      pasek stanu, a potem wychodzi poza edytor: shell jadra robi 'cat'
      na zapisanym pliku, a w trybie 64-bitowym - gdzie shella jadra z
      pulpitu nie da sie pisac - robi to terminal przez 'ucat64'. Jeden
      program zapisal, drugi przeczytal.

- [x] **Edytor dostaje plik, a menedzer plikow ma czym go podac** - i
      dwa jadra znowu robia to samo.

      'gedit notatka' otwiera plik od razu. Kazanie czlowiekowi wpisac
      nazwe drugi raz, w programie, ktoremu wlasnie powiedzial, o ktory
      plik chodzi, to jest dokladnie to, od czego edytor robi sie
      obowiazkiem.

      W menedzerze plikow przybyl przycisk "Edytuj" - bo do czego SLUZY
      menedzer plikow, jesli nie do dostania sie do pliku, a dostanie
      sie do pliku tekstowego znaczy otworzenie go w edytorze. W trybie
      chronionym to jedno wywolanie sys_spawn z gotowa linia polecenia,
      w long mode fork i exec, bo tam nie ma spawnu - dwa zrodla, bo
      menedzer plikow ma osobne, ale ten sam przycisk w tym samym rogu.

      Przy okazji dziura, ktora znalazl test edytora: w trybie
      64-bitowym pulpit nie mial komu oddac klawisza, ktorego nie chcialo
      zadne okno. keyboard_console_handler() zwracalo NULL, wiec ekran
      pelen okien byl ekranem, na ktorym nie dalo sie nic wpisac do
      shella jadra - podczas gdy w trybie chronionym dzialalo to od
      zawsze. Teraz klawisz idzie do kolejki ogladanej konsoli, czyli
      tam, skad shell czyta. Grupa 'desktop64' sprawdza to jednym
      'uptime' wpisanym przy oknach na ekranie.

      Grupa 'gedit' urosla do 24 asercji i konczy sie tym samym po obu
      stronach: menedzer plikow widzi plik, ktory zapisal edytor, i
      oddaje go z powrotem edytorowi.

- [x] **Program moze odmowic zamkniecia** - bo zamkniecie zawsze bylo
      PROSBA, tyle ze nie bylo jak odpowiedziec "jeszcze nie".

      Odliczanie po prosbie bylo od poczatku - dla programow, ktore nie
      odpowiadaja. Brakowalo drugiej odpowiedzi: programu, ktory
      USLYSZAL i ma powod zostac. Edytor z niezapisanym plikiem jest
      calym tego przykladem.

      Nowe wywolanie systemowe win_keep(okno): "slyszalem, jeszcze nie
      zamykam". Pulpit przestaje odliczac, a co program zrobi z
      wyproszona chwila - zapyta we wlasnym oknie, zapisze plik - to
      juz jego sprawa. Milczenie dalej kosztuje okno: program, ktory nie
      odpowiada, po dwoch sekundach je traci.

      Przy okazji ognisko: prosba o zamkniecie zabierala je oknu, ktore
      wlasnie pytano. Skoro program moze odmowic, drugie Alt+F4 musi
      miec jak do niego trafic - wiec ognisko zostaje przy oknie tak
      dlugo, jak okno, a puszcza sie je przy sprzataniu.

      Edytor odmawia raz: pierwsze Alt+F4 zamienia sie w pytanie w pasku
      stanu, drugie jest na nie odpowiedzia. Zapisanie pliku kasuje i
      gwiazdke, i pamiec o pytaniu.

      Grupa 'gedit' ma teraz 30 asercji i sprawdza obie strony: ze okno
      po odmowie stoi jeszcze piec sekund pozniej (odliczanie naprawde
      stanelo, a nie tylko zwolnilo), ze drugie Alt+F4 zdejmuje przycisk
      z paska zadan i ze program konczy sie SAM z kodem zero, a nie
      zostaje zabity. To samo w long mode, gdzie wywolanie ma wlasny
      numer i wlasna obsluge.

      I jedna nauka o samym tescie: kursor myszy jest bialy jak kartka
      edytora. Sonda zostawiona pod kursorem mowila "okno dalej jest" o
      oknie, ktorego juz nie bylo.

- [x] **Kazdy rdzen dostaje wlasna linie cache planisty** - i przy okazji
      lekcja o tym, czego test naprawde pilnuje.

      Grupa 'kthreads' zaczela padac po dodaniu SZESCIU WIERSZY do
      kompozytora - kodu, ktory w tym tescie nawet sie nie uruchamia
      (test chodzi w trybie tekstowym). Cztery watki jadra, ktore
      wczesniej konczyly w 45 tyknieciach, potrzebowaly 508. Bisekcja po
      wlasnych zmianach pokazala winowajce, ktory winowajca byc nie mogl
      - i to bylo wlasnie wskazowka.

      Wskazniki 'running' i 'idle' kazdego rdzenia leza w tablicy
      cores[]. Osiem bajtow na rdzen, cztery rdzenie - wszystkie w
      JEDNEJ linii cache. Kazde przelaczenie zadania to zapis do tej
      linii, wiec przy czterech rdzeniach linia jezdzi miedzy nimi
      zamiast pracy. Ile to kosztuje, zalezy od tego, gdzie akurat
      wypadnie granica linii - a to zmienia sie, gdy cokolwiek w jadrze
      urosnie. Stad test, ktory pada od zmiany w zupelnie innym pliku.

      Szescdziesiat cztery bajty na rdzen to najtansza rzecz, jaka to
      jadro kupuje. Zamek planisty dostaje wlasna linie z tego samego
      powodu. Po poprawce: 45 tykniec, niezaleznie od tego, co gdzie
      indziej urosnie.

      Test nie byl zbyt czuly. Mierzyl dokladnie to, co mial mierzyc -
      czy praca idzie rownolegle - i powiedzial prawde o maszynie, ktora
      przestala isc rownolegle. Prog zostal, gdzie byl.

- [x] **Menedzer zadan w oknie** - bo 'ps' drukuje, a drukowania program
      nie przeczyta.

      Lista, po ktora trzeba napisac komende, to lista, w ktora nikt nie
      patrzy. Maszyna z tuzinem okien potrzebuje jednego miejsca, ktore
      mowi, co dziala - a gdy program przestal odpowiadac, sposobu na
      zakonczenie go innego niz restart maszyny.

      Nowe wywolanie sys_ps(numer, bufor): jeden wiersz listy zadan na
      wywolanie, indeks od zera, -1 gdy nic tam nie ma - dokladnie tak,
      jak czyta sie katalog. Kolejnosc to kolejnosc listy, wiec lista,
      ktora program pokazuje, jest o jedno odswiezenie z tylu - jak w
      kazdym menedzerze zadan na swiecie.

      Okno: wiersz na zadanie, odswiezane dwa razy na sekunde, klik
      wybiera, przycisk konczy. Dwie rzeczy, ktorych nie zrobi: nie
      zakonczy watku jadra (te dziela pamiec jadra, wiec zabranie
      jednego zabiera kawalek maszyny) i nie zakonczy SIEBIE - menedzer
      zadan, ktory znika po kliknieciu, jest zartem, a nie narzedziem.
      Oba przypadki mowia dlaczego, zamiast nie zrobic nic.

      Co odroznia program od watku jadra: wlasna przestrzen adresowa, a
      nie sterta. Sterte maja jedne i drugie - pierwsza wersja pokazywala
      wszystko jako "program" i test to zlapal od razu.

      Nowa grupa 'gtasks' (22 asercje, oba jadra) sprawdza, ze lista
      ZYJE: program uruchomiony przy otwartym oknie sam sie na niej
      pojawia, a zakonczony przyciskiem znika i z listy, i z paska
      zadan. Oraz obie odmowy.

      Test musial przy okazji odsunac okno menedzera z drogi kaskady -
      bo kazde nowe okno otwiera sie o krok dalej i stanelo nad
      czytana lista. Czlowiek by je przesunal; test tez.

- [x] **Miernik obciazenia na gornej belce** - ile maszyny jest zajete,
      jednym spojrzeniem.

      Liczba mowi "4128 KiB", czyli jest do PRZECZYTANIA. Pasek
      wypelniony w jednej trzeciej jest do ZOBACZENIA - a dokladne
      liczby (ile pamieci, z ilu, i ile zadan za tym stoi) sa jedno
      najechanie kursorem dalej, wiec belka milczy, dopoki sie jej nie
      spyta.

      Miejsce: gorna belka, obok przelacznika pulpitow. Tam mieszka to,
      co jest ZAWSZE prawda - ktory pulpit jest widoczny i ile maszyny
      jest zajete. Pasek zadan na dole jest dla okien; miernik na nim
      bylby jeszcze jedna rzecza sciskajaca przyciski (i, jak sie
      okazalo przy pierwszej wersji, jedna rzecza, ktora testy licza
      jako przycisk).

      Wypelnienie rysuje sie WEWNATRZ ramki. Narysowane po niej, odczyt
      kilku procent ma jedna kolumne szerokosci - dokladnie te, ktora
      zakrywa ramka - wiec miernik nie pokazywalby nic, dopoki maszyna
      nie bylaby w polowie pelna.

      Podpowiedz umie sie teraz pojawic tam, gdzie stoi to, co
      tlumaczy: dotad zawsze wychodzila nad paskiem zadan, a
      podpowiedz na drugim koncu ekranu niz rzecz, ktorej dotyczy, jest
      zagadka.

      Liczba zadan: task_count() w obu jadrach - 64-bitowe mialo ja od
      dawna, 32-bitowe dostalo teraz.

- [x] **Okno mowi, co ma otwarte** - bo trzy okna nazwane "Edytor" to
      trzy okna do sprawdzenia po kolei.

      Nowe wywolanie win_title(okno, napis): program zmienia nazwe
      swojego okna. Pasek tytulu i podpowiedz na pasku zadan czytaja to
      samo pole, wiec jedno wywolanie zalatwia oba miejsca.

      Edytor mowi teraz "Edytor: notatka" - po wczytaniu i po zapisie,
      bo dopiero wtedy plik ma nazwe. Terminal mowi "Terminal: /kat",
      i robi to tam, gdzie i tak buduje zachete: katalog zmienia sie
      jednym 'cd', a zacheta i tytul maja mowic to samo.

      Dwa jadra, dwa numery wywolania, jedna funkcja w kompozytorze.

- [x] **Podwojny klik i "pokaz pulpit"** - dwa odruchy, ktore czlowiek
      sprawdza w pierwszej minucie.

      Dwa klikniecia w ten sam pasek tytulu, blisko siebie w czasie, to
      jeden gest: na caly ekran albo z powrotem. "Blisko siebie" to
      dwadziescia klatek, czyli okolo szesc dziesiatych sekundy przy
      trzydziestu trzech klatkach na sekunde - wartosc, przy ktorej
      osiada kazdy system. Krocej i reka nie trafia na zajetej maszynie,
      dluzej i dwa osobne klikniecia (bo tak sie podnosi okno) zaczynaja
      liczyc sie jako jeden gest. Pierwsza wersja miala dwanascie klatek
      i nie dalo sie w nia trafic.

      Przycisk "pokaz pulpit" na gornej belce chowa wszystkie okna, a
      drugim kliknieciem przywraca DOKLADNIE te, ktore schowal. To jest
      cala rzecz: przycisk, ktory tylko chowa, kosztuje uklad okien i
      nikt go nie nacisnie drugi raz. Okno zwiniete recznie zostaje
      zwiniete, bo to byla czyjas decyzja.

- [x] **Menedzer plikow napisany od nowa** - i jedno zrodlo zamiast
      dwoch.

      To, co bylo, listowalo osiem nazw w oknie wielkosci znaczka
      pocztowego i pokazywalo pierwsze sto bajtow jednej z nich. To
      dowodzilo drogi "dysk -> klik -> odczyt -> rysunek"; menedzerem
      plikow nie bylo. Menedzer plikow to miejsce, do ktorego czlowiek
      IDZIE po plik: mowi, ile co waży, wchodzi do katalogow i z nich
      wychodzi, podaje plik programowi, ktory go otwiera, i wyrzuca go,
      gdy poprosic dwa razy.

      Wiec: lista z rozmiarami, katalogi oznaczone i chodliwe (Enter
      albo "Otworz" wchodzi, Backspace wychodzi wyzej), nazwa katalogu w
      tytule okna, PgUp/PgDn przewija, a licznik w rogu mowi, ktory
      kawalek listy widac - lista, ktora przewija sie bez slowa, wyglada
      na taka, ktora coz zgubila.

      "Usun" pyta. Menedzer plikow, ktory wyrzuca plik po jednym
      kliknieciu, to menedzer, ktorego czlowiek sie boi - a drugie
      klikniecie jest odpowiedzia, nie powtorzeniem. Katalogu nie usuwa
      wcale.

      Podglad tresci znikl, bo od pokazywania tresci jest edytor, a
      "Otworz" mu ten plik podaje. Dwie asercje w starych testach
      mowily o podgladzie - zostaly przepisane na to, co program robi
      teraz: klik wybiera wiersz, a wybor widac kolorem akcentu.

      I najwazniejsze: JEDNO zrodlo dla obu jader. Osobne byly przez
      jedna rzecz - czytanie katalogu, ktore w trybie chronionym oddaje
      rozmiar w wyniku, a w long mode wypelnia strukture. Ta jedna
      roznica to teraz dziesiec wierszy pod #ifdef, a reszta - lista,
      klikanie, rysowanie - jest wspolna. user64/gfiles64.c usuniety.

- [x] **Jeden klik, jedno dzialanie** - i przyciski trzymajace krawedz.

      Screeny zlapaly blad, ktorego zaden test nie widzial: klikniecie
      "Otworz" w menedzerze plikow uruchamialo edytor DWA RAZY. Okno
      dostaje i WEV_PRESS, i WEV_CLICK - te same palce, dwa zdarzenia -
      a programy sluchaly obu. Teraz slucha sie klikniecia, a nacisniecie
      i puszczenie zostaja tam, gdzie sa potrzebne: przy ciagnieciu.

      Drugi screen (okno rozwiniete na caly ekran) pokazal przyciski
      przybite do liczby: "Zakoncz" stalo w polowie okna szerokiego na
      caly ekran i wygladalo jak pomylka. Przyciski liczy sie teraz od
      prawej krawedzi.

      Obie rzeczy widac na obrazku od razu, a w tescie - dopiero gdyby
      ktos sprawdzil, ile okien sie otworzylo.

- [x] **Ctrl+F w edytorze i Tab w terminalu** - dwa klawisze, bez ktorych
      pisze sie wolno.

      Ctrl+F pyta w pasku stanu, szuka od miejsca ZA kursorem i okraza
      koniec pliku, wracajac tam, skad zaczal - szukanie, ktore staje na
      dole, trzeba obmyslic, a od myslenia o szukaniu nie jest szukanie.
      Enter przy pustym pytaniu powtarza ostatnie, bo pierwsze trafienie
      rzadko jest tym, o ktore chodzilo. Mowi, w ktorym wierszu znalazl,
      albo ze nie znalazl.

      Tab w terminalu konczy slowo spod kursora tym, co lezy na dysku.
      Jedno dopasowanie konczy slowo, kilka - popycha je tak daleko, jak
      sie zgadzaja, i wypisuje kandydatow (dalej byloby zgadywaniem,
      ktore z nich); zadne nie robi nic, bo to jedyna uczciwa
      odpowiedz. Nazwy bierze z katalogu roboczego, czyli stamtad, skad
      bierze je shell - wiec dopelnia sie to, co i tak by sie
      uruchomilo. Gdy program dziala, wiersz nalezy do niego i tabulator
      jest tabulatorem.

      Trzeci kod klawisza dla jadra: Ctrl+F. Jadro dalej wie tylko tyle,
      ze klawisze padly.

- [x] **Ctrl+Z w edytorze** - bo edytor bez cofania to edytor, w ktorym
      pisze sie ostroznie, a ostrozne pisanie jest wolniejsze niz pisanie
      i poprawianie.

      Pamieta sie nie PLIK, tylko ZMIANY: znak wstawiony, znak usuniety,
      wiersz rozdzielony, dwa wiersze zlaczone. Kazda z tych czterech ma
      oczywiste przeciwienstwo, a robienie przeciwienstw w odwrotnej
      kolejnosci jest calym cofaniem. Sto dwadziescia krokow wstecz
      kosztuje dwa kilobajty; dwiescie kopii pliku kosztowaloby cztery
      megabajty.

      Najstarszy krok wypada, gdy log sie zapelni. Kto napisal od tamtej
      pomylki sto dwadziescia rzeczy, nie szuka juz tamtej pomylki.

      Przy okazji dwie operacje na wierszach - rozdziel i zlacz -
      zostaly wypisane raz, jako split_at() i join_at(): uzywaja ich i
      klawisze, i cofanie, tylko w druga strone. Wczesniej ta sama
      robota byla wpisana w Enter i w Backspace osobno.

- [x] **Struktura zadania na wlasnej linii cache - i utajony blad, ktory
      to odkrylo.**

      Grupa 'kthreads' znowu zaczela padac, tym razem po stronie
      64-bitowej: cztery watki jadra 320 tykniec zamiast 42. Bisekcja po
      commitach pokazala winowajce nie do uwierzenia - CZTERY WIERSZE
      DODANE DO MAPY KLAWIATURY (Ctrl+F i Ctrl+Z). Kod, ktory w tym
      tescie nawet nie jest wolany.

      Struktury zadan leza na stercie, a sterta wyrownuje do osmiu
      bajtow. Pierwsze slowa struktury - wskaznik stosu i stan - sa
      zapisywane przy KAZDYM przelaczeniu zadania, przez ten rdzen,
      ktory to zadanie prowadzi. Dwie struktury w jednej linii cache to
      dwa rdzenie piszace do jednej linii, a wtedy linia jezdzi miedzy
      nimi zamiast pracy. Czy tak wypadnie, zalezy od tego, gdzie
      poukladaly sie bloki sterty - czyli od rozmiaru kodu, ktory nie ma
      z tym nic wspolnego.

      Zadanie dostaje teraz linie dla siebie: prosimy o linie wiecej i
      sami przesuwamy sie do granicy, pamietajac adres do oddania.

      Po drodze pierwsza proba - "kazdy blok sterty na wlasnej linii" -
      wywrocila sterownik xHCI. I dobrze, bo pokazala blad starszy od
      wszystkiego: tworzac zadanie zerowano w tablicy mapowan plikow
      tylko POCZATEK i KONIEC, a nie wskaznik 'blocks'. Zwolnienie
      mapowania oddawalo wiec sterte wskaznik, ktory byl smieciem z
      poprzedniego zycia tej pamieci. Czekalo to na sterte, ktora poda
      blok z odpowiednim smieciem w srodku - i doczekalo sie.

      Sterta ma teraz straznika: wskaznik, przed ktorym nie ma
      sensownego naglowka, nie jest zwalniany, tylko zglaszany. Awaria
      byla w kfree, a blad calkiem gdzie indziej - i to jest dokladnie
      ten rodzaj awarii, o ktorym warto powiedziec glosno.

- [x] **Nadrobione testy: osiem funkcji dostaje siatke** - bo to ona
      zlapala blad sprzed miesiecy w mapowaniach plikow.

      Dwie nowe grupy i trzy rozszerzone:

      'sysbar' (15 asercji) - gorna belka. Miernik pokazuje, ze cos jest
      zajete i ze nie wszystko; podpowiedz podaje liczby, ktore trzymaja
      sie kupy (uzyte <= calosc, procent <= 100); a najwazniejsze - te
      liczby ZYJA: program uruchomiony miedzy dwoma spojrzeniami musi
      pojawic sie w liczniku zadan. "Pokaz pulpit" chowa okna, nie
      zamykajac ich (przyciski zostaja na pasku), a drugi klik przywraca
      dokladnie te, ktore schowal - okno zwiniete recznie zostaje
      zwiniete.

      'files' (18 asercji) - menedzer plikow. Rozmiar sprawdzany przy
      pliku, ktorego dlugosc test zna, bo kolumna liczb znikad niczego
      nie dowodzi. Usuwanie sprawdzone w obu polowach: pierwszy klik
      tylko pyta (i plik dalej jest), drugi usuwa - co potwierdza shell
      jadra, nie umiejac juz tego pliku przeczytac. Katalogu nie usuwa
      wcale. Nazwa okna mowi, w ktorym katalogu stoimy, i zmienia sie
      przy wejsciu.

      'winkeys' (+4) - podwojne klikniecie w pasek tytulu na caly ekran i
      z powrotem. 'gedit' (+9) - Ctrl+F znajduje slowo LEZACE WYZEJ niz
      kursor, czyli naprawde okraza koniec pliku; Enter przy pustym
      pytaniu powtarza; Ctrl+Z cofa po jednym znaku; okno nosi nazwe
      pliku. 'gterm' (+4) - Tab konczy nazwe, przy kilku pasujacych
      wypisuje kandydatow i dopisuje tylko to, co wspolne; okno mowi, w
      ktorym katalogu stoi.

      Dwie rzeczy poprawione po drodze, obie znalezione przez pisanie
      testu: podpowiedz miernika chowala sie POD KURSOREM (strzalka jest
      rysowana na niej), wiec wychodzi teraz w lewo od tego, co
      tlumaczy; a napis "katalog" w menedzerze byl wyrownany inaczej niz
      kolumna rozmiarow - dwie kolumny konczace sie w roznych miejscach
      czytaja sie jak dwie kolumny.

- [x] **Zmiana nazwy w systemie plikow** - bo dotad przeniesienie
      czterech megabajtow znaczylo przeczytanie i zapisanie czterech
      megabajtow, zeby zmienic szescdziesiat cztery bajty wpisu w
      katalogu.

      fs_rename(): bloki pliku nie ruszaja sie z miejsca, zmienia sie
      tylko wpis, ktory je nazywa - a gdy sciezki wskazuja rozne
      katalogi, takze rodzic. Reguly ma te, co kazdy system plikow:
      zrodlo musi istniec, katalog celu musi istniec, nazwa celu musi
      byc wolna (zmiana nazwy nie jest sposobem na skasowanie czegos
      przez przypadek), a katalogu nie da sie przeniesc do samego siebie
      ani pod siebie - to odcielo by kawalek drzewa od korzenia i
      zostawilo go wskazujacego w kolko.

      Do tego wywolanie systemowe po obu stronach (pliki systemowe
      chronione z OBU stron: przemianowanie systemowego pliku jest
      rownie dobre jak jego skasowanie, a przemianowanie czegos NA jego
      nazwe podklada obcy plik tam, gdzie system szuka swojego),
      komenda 'mv' w shellu jadra, i program 'mv', ktory probuje teraz
      najpierw taniej drogi, a kopiowanie zostawia jako druga probe -
      bo "mv na istniejacy plik" jest rzecza, ktora ludzie robia.

      W menedzerze plikow przybyl przycisk "Nazwa": pyta o nowa nazwe w
      tym samym pasku, w ktorym mowi wszystko inne. Okienko dialogowe
      byloby drugim oknem do przesuwania, ogniskowania i zamykania - dla
      jednego slowa.

      Testy: 'pfs' dostal zmiane nazwy i obie odmowy (nazwa zajeta,
      zrodla nie ma), 'files' pyta i zmienia, a 'gedit' robi to samo na
      jadrze 64-bitowym, gdzie wywolanie ma wlasny numer i wlasna
      obsluge.

- [x] **Zrzut ekranu i podglad obrazu** - maszyna, ktora umie sie
      sfotografowac i sama na to zdjecie popatrzec.

      Print Screen zapisuje ekran w wlasnych kategoriach maszyny: cztery
      bajty "PSCR", szerokosc, wysokosc, cala paleta (256 kolorow po
      trzy bajty, zeby plik mowil, co znacza jego liczby) i po jednym
      bajcie na piksel - numer w palecie. To dokladnie to, co narysowal
      kompozytor, wiec podglad nie ma czego interpretowac: wklada bajty
      z powrotem do okna i to znowu sa te same kolory.

      Odczyt ekranu wstecz: w trybach osmiobitowych bufor JUZ trzyma
      numery i to jest kopia; w 24- i 32-bitowych trzyma kolory, a droga
      powrotna to szukanie w palecie - dokladne, bo wszystko na tym
      ekranie zostalo z tej palety narysowane. Szukanie byloby 256
      porownan na piksel, gdyby robic je uczciwie za kazdym razem; ekran
      sklada sie z ciagow jednego koloru, wiec ostatnia odpowiedz jest
      pamietana i prawie zawsze jest znowu ta wlasciwa.

      Nowe wywolanie win_blit(): blok pikseli prosto do tresci okna.
      Wszystko, co program mogl dotad narysowac, to prostokat jednego
      koloru albo wiersz tekstu - a obraz nie jest ani jednym, ani
      drugim, i rysowanie go po prostokacie na piksel byloby trzysta
      tysiecy wywolan systemowych na jeden zrzut.

      Podglad pokazuje co drugi (albo co trzeci) piksel, bo caly ekran
      nie miesci sie w oknie na tym samym ekranie. Najblizszy sasiad, bo
      chodzi o to, zeby ZOBACZYC, co bylo na ekranie, a rozmyta polowka
      ekranu pelnego osmiopikselowych liter pokazalaby mniej, nie
      wiecej. Nic nie jest trzymane po wczytaniu: tresc okna JEST kopia.

      Menedzer plikow podaje ".psc" podgladowi, a wszystko inne
      edytorowi - po nazwie, bo zagladanie do srodka znaczyloby czytanie
      kazdego pliku w katalogu, zeby narysowac liste.

      Nowa grupa 'zrzut' (9 asercji) sprawdza, ze plik ma dokladnie tyle
      bajtow, ile ma ekran plus paleta, i - co wazniejsze - ze obraz w
      oknie NAPRAWDE jest ekranem: pomaranczowy ikon znajduje sie w
      oknie w tym samym miejscu i w ilosci odpowiadajacej pokazywaniu co
      drugiego piksela. Takiej liczby nie produkuje zaden przypadek.

- [x] **Obraz z zewnatrz: BMP i kolory, ktorych ekran nie ma** - bo
      pytanie "a co z JPG, PNG i BMP" ma trudnosc nie w formatach, tylko
      w kolorze.

      Ten ekran jest osmiobitowy z paleta: tresc okna to numery, nie
      kolory. Plik .psc dziala bez wysilku, bo to SA bajty kompozytora.
      Kazdy obraz z zewnatrz jest RGB i trzeba go skwantyzowac do tego,
      co maszyna ma: trzydziesci szesc kolorow pulpitu plus sto
      dziewiecdziesiat dwa miejsca po tapecie. Rysunek wychodzi dobrze,
      zdjecie wychodzi jak zdjecie ogladane przez trzydziesci szesc
      kredek - i to wlasnie jest osmiobitowy ekran, a nie usterka.

      Wiec najpierw fundament: sys_palette() oddaje programowi cala
      palete, bo bez wiedzy, co znacza numery, zaden format nie ma sensu.
      Szukanie najblizszego koloru bylo by 256 porownan na piksel, wiec
      odpowiedz jest pamietana dla koloru zaokraglonego do pieciu bitow
      na kanal: najwyzej trzydziesci dwa tysiace szukan, a obraz rzadko
      uzywa dziesiatej czesci tego.

      BMP: bez kompresji, 24 bity albo 8 z wlasna paleta, wiersze
      dopelniane do czterech bajtow i - o ile wysokosc nie jest ujemna -
      liczone od DOLU.

      Jak taki plik ma tu trafic? Nie ma dyskietki ani wymiany z hostem,
      ale jest siec: 'httpget' sciaga plik na dysk, a podglad go
      pokazuje. Tak tez sprawdza to test - serwer harnessu podaje BMP z
      czterema cwiartkami w czerwieni, zieleni, blekicie i bieli, bo te
      cztery kolory paleta ma DOKLADNIE: dekoder, ktory dziala, pokazuje
      dokladnie je, a dekoder, ktory cos rysuje, pokazuje cos innego.

      Nastepne w kolejce: PNG (inflate to jakies trzysta wierszy i
      przyda sie jeszcze nie raz), potem JPEG baseline.

- [x] **PNG: inflate wlasnej roboty, chunki i piec filtrow** - drugi z
      trzech formatow z pytania "a co z JPG, PNG i BMP".

      PNG to trzy rzeczy w jednym plaszczu: chunki, strumien deflate i
      jeden bajt filtra na wiersz. Chunki sa latwe (dlugosc, nazwa,
      bajty, suma kontrolna, na ktora nie trzeba patrzec, bo strumien
      deflate i tak sie nie zlozy, jesli bajty sa zle).

      Deflate wyladowal w osobnym pliku (user/uinflate.h), bo przyda sie
      jeszcze nie raz: trzy rodzaje blokow (skladowany, ze stalymi
      drzewami, z drzewami zapisanymi w samym bloku - opisanymi trzecim
      drzewem), a potem strumien symboli, gdzie symbol ponizej 256 to
      bajt, 256 konczy blok, a powyzej znaczy "przepisz N bajtow sprzed
      D" - i to jest cale kompresowanie. Kopiowanie bajt po bajcie jest
      celowe: kopia moze zachodzic na siebie, bo "trzy bajty sprzed
      jednego" znaczy ten sam bajt trzy razy.

      Dekodowanie Huffmana zwyklym chodzeniem po bitach, bez tablicy
      podgladu: trzydziesci wierszy zamiast stu piecdziesieciu, a obraz
      i tak wczytuje sie chwile. Sprawdzone na hoscie przeciw prawdziwej
      zlib, na czterech poziomach kompresji - w tym zerowym, ktory daje
      bloki skladowane.

      Filtry to miejsce, gdzie dekodery zwykle padaja, wiec obraz w
      tescie ma KAZDY z piatki po kolei (none, sub, up, average, paeth).
      Obraz z samymi zerowymi filtrami nie mowi nic o pozostalych
      czterech.

      Przeplatanego PNG (Adam7) nie umiem i mowie to wprost, zamiast
      rysowac cwierc obrazu: obraz pokazany zle jest gorszy niz obraz
      niepokazany.

      W 'proccores' wiecej cierpliwosci (cztery odczyty zamiast dwoch,
      liczy sie najlepszy) przy tym samym progu - suita ma coraz
      ciezszych sasiadow, a pomiar rownoleglosci na hoscie bez wolnego
      rdzenia mierzy hosta.

- [x] **JPEG baseline: Huffman, kosinus i kolor z polowy rozdzielczosci**
      - trzeci i ostatni z formatow z pytania "a co z JPG, PNG i BMP".

      BMP to bylo czytanie bajtow, PNG - rozpakowanie strumienia. JPEG
      jest czyms innym: obrazu w tym pliku NIE MA. Sa wspolczynniki
      przeksztalcenia kosinusowego, spakowane Huffmanem i podzielone
      przez tablice kwantyzacji, a obraz trzeba z nich policzyc.

      Kolejno: bity (z odkrecaniem wypelniacza - 0xFF w danych jest
      zapisane jako 0xFF 0x00, zeby nie udawalo znacznika), Huffman po
      kanonicznych tablicach z naglowka (mincode/maxcode/valptr, bez
      drzewa - kody tej samej dlugosci leza obok siebie, wiec wystarczy
      porownanie), zygzak z powrotem do kwadratu 8x8, mnozenie przez
      kwantyzacje i odwrotny kosinus.

      Kosinus w stalym przecinku, dwanascie bitow po przecinku, i
      rozdzielony: osiem kolumn, potem osiem wierszy. Rozdzielenie to
      jedyny powod, dla ktorego to sie liczy w rozsadnym czasie -
      wprost byloby 64 mnozenia na piksel zamiast szesnastu.

      A na koncu kolor: JPEG trzyma jasnosc w pelnej rozdzielczosci, a
      barwe zwykle w polowie w obu osiach, bo oko tego nie widzi. Wiec
      skladowe leza we wlasnych rozmiarach, a pytanie o piksel przelicza
      wspolrzedne - i dopiero wtedy YCbCr wraca na RGB.

      Sprawdzone przeciw dekoderowi Go (image/jpeg) na calym jego
      zestawie testowym: trzynascie plikow baseline - 4:4:4, 4:4:0,
      4:2:2, 4:2:0, 4:1:1, 4:1:0, ze znacznikami restartu, w skali
      szarosci, z probkowaniem 2x2 na jedynej skladowej i z barwami
      zapisanymi wprost jako R, G, B. Najwieksza roznica na kanal: 4
      poziomy z 256, i to tylko na zaokragleniu kosinusa. To jest ten
      rodzaj sprawdzenia, ktorego wlasny test nie zastapi - obcy plik
      od obcego kodera.

      Po drodze trzy prawdziwe bledy, wszystkie znalezione przez
      psucie pliku losowo (900 uszkodzonych wersji, pod ASan i UBSan):
      numery tablic z naglowka nie byly sprawdzane (tq=66 siegalo poza
      tablice kwantyzacji), przesuwanie liczby ujemnej w lewo jest
      niezdefiniowane, a wspolczynniki z popsutego pliku wychodzily poza
      zakres na tyle, ze mnozenia w kosinusie przekraczaly int. Stad
      dwa ograniczniki - na wspolczynnik i na wynik pierwszego przebiegu
      - dobrane z pomiaru na prawdziwych obrazach (najwiekszy
      wspolczynnik ~1000, najwiekszy wynik posredni ~4200) i ustawione
      dziesieciokrotnie wyzej: poprawnego obrazu nie dotykaja, a
      popsutemu daja bezsensowne piksele zamiast niezdefiniowanego
      zachowania.

      Przy okazji oba dekodery obrazow - inflate i JPEG - weszly na
      stale do 'make fuzz', obok certyfikatow, HTML-a i ramek. Obraz
      przychodzi z sieci dokladnie tak samo jak certyfikat, a w dodatku
      to wejscie decyduje, ILE sie zapisze; jednorazowe sprawdzenie w
      trakcie pisania nie pilnuje niczego pozniej.

      Progresywnego JPEG nie umiem i mowie to wprost - to jest drugi
      dekoder, nie wariant tego samego. Tak samo CMYK i arytmetycznego.

      Test bierze obrazek od obcego kodera (Go, jakosc 90, 4:2:0),
      wpisany do harnessu jako bajty - bo wlasny koder pisalby pliki
      pod wlasny dekoder, a to nie jest sprawdzanie. Te same cztery
      cwiartki co przy BMP i PNG, czytane w srodkach cwiartek, bo
      format stratny ma prawo rozmazac granice. Sprawdzane na obu
      jadrach - dekoder jest jednym zrodlem kompilowanym dwa razy, a w
      trybie dlugim wskaznik ma inny rozmiar i indeksowanie plaszczyzny
      idzie przez long.

- [x] **Kolory: koniec z paleta 256 tam, gdzie ekran ma wiecej** -
      odpowiedz na pytanie "co zrobic, zeby miec tyle kolorow, co
      wspolczesne komputery".

      Najpierw to, czego nie trzeba bylo robic: bufor ekranu JUZ byl
      32-bitowy. video.c od dawna umie 24 i 32 bity na piksel, a
      video_adopt_lfb bierze bufor od GRUB-a albo od UEFI, wiec na
      prawdziwym sprzecie to dziala. Sufitem nie byl sprzet, tylko my
      sami: wszystko powyzej video_putpixel mowilo indeksami palety.

      **Sterta.** Pierwszy prawdziwy blocker nie mial nic wspolnego z
      kolorem. Bufor okna to jeden bajt na piksel; w kolorach cztery.
      Okno 420x300 rosnie ze 123 KiB do 492 KiB, a sterta jadra miala
      MEGABAJT - dwa okna i po stercie. Wiec sterta liczy sie teraz z
      wykrytej pamieci: szesnasta czesc, miedzy 1 a 8 MiB. Maszyna ze
      128 MiB dostaje 8 MiB, a te z 16 MiB, na ktorych chodza testy
      wymiany, dostaja dokladnie ten megabajt, ktory mialy - nie
      zabieramy im polowy pamieci przy okazji.

      **Paleta 8-bitowa.** video_set_palette brala skladowe 6-bitowe
      (0..63) - spadek po DAC-u VGA, ktory tyle bierze. Teraz bierze
      0..255, a zwezenie zostalo tam, gdzie jego miejsce: w jednej
      linijce piszacej do DAC-u. Stale w desktop.c przeliczone dokladna
      formula (v<<2)|(v>>4), czyli tak, jak przeliczala je sama paleta
      - wyglad nie drgnal ani o poziom, co potwierdzily grupy 'look',
      'wall' i 'icons'. To jest zmiana, ktora zdejmuje sufit i niczego
      nie rusza.

      **Druga sciezka rysowania.** video_putpixel_rgb, video_fill_rect_rgb
      i video_blit_rgb biora 0x00RRGGBB. W trybie 8-bitowym (tryb 13h,
      awaryjny) kolor jest wymieniany na najblizszy indeks - z pamiecia
      ostatniej odpowiedzi, bo obraz sklada sie z ciagow jednego koloru.

      **Okno z formatem.** Okno rodzi sie z indeksami, bo tak rysowal
      kazdy program tej maszyny i zaden nie musial sie zmieniac. Program,
      ktory ma co pokazac, prosi raz (win_truecolour) i od tej pory
      pisze kolory przez win_blit32. Jego wlasna oprawa - tlo, pasek
      stanu - dalej idzie przez win_rect i win_text w indeksach, bo te
      wywolania TLUMACZA indeks na kolor dla takiego okna. Dzieki temu
      gimg zmienil trzy petle, a nie caly plik.

      Podmiana bufora pod oknem jest bezpieczna nie przez nowy zamek,
      tylko przez ten, ktory juz jest: syscall bierze duzy zamek jadra,
      a kompozytor trzyma go przez cale rysowanie. Ta sama zasada od
      dawna chroni resize_content.

      Ciekawe uboczne: okno w kolorach sklada sie SZYBCIEJ niz w
      indeksach - jego piksele sa juz tym, czego chce bufor ekranu, wiec
      idzie wierszami zamiast piksel po pikselu.

      **gimg.** Znikla funkcja nearest() i tablica 32768 zapamietanych
      odpowiedzi. Zdjecie jest zdjeciem, a nie zdjeciem widzianym przez
      trzydziesci szesc kredek. Pliki .psc zostaja indeksami - ich bajty
      SA numerami tej palety, wiec win_blit odklada je dokladnie.

      **Tapeta.** Format dostal drugi ksztalt: "PWL2", rozmiar, trzy
      bajty na piksel, bez palety. Stary "PWAL" dalej sie czyta. I tu
      trzeba byc uczciwym: dla czterech z pieciu wzorow mkwalla to nie
      zmienia NIC - zbocze przez 120 wierszy ma 120 kolorow, cokolwiek
      by sie robilo. Zmiana zarabia na siebie piatym wzorem:
      przenikaniem po obu osiach, gdzie kazdy piksel ma swoj kolor -
      dziewietnascie tysiecy, czego paleta 192 nie udzwignie.

      **gweb nie dostal nic** i mowie to wprost: jego parser HTML w
      ogole nie czyta kolorow, rysuje szescioma stalymi. Przepisanie
      tych szesciu stalych na RGB nie zmienilo by ani jednego piksela.
      Prawdziwe kolory w przegladarce to parser CSS - inna robota, nie
      ta.

      **Prezentacja tylko tego, co sie zmienilo.** Kompozytor przerysowuje
      calą klatke trzydziesci razy na sekunde, czy cos sie ruszylo, czy
      nie, i dotad cala klatka szla na karte - przy 640x480 to 1,2 MB,
      przy 1024x768 juz 3 MB. Pisanie do bufora karty jest DUZO drozsze
      niz do pamieci, wiec doszedl trzeci bufor: kopia w RAM tego, co
      ostatnio poszlo na ekran. Porownanie RAM z RAM-em mowi, ktore
      wiersze naprawde sie zmienily, i tylko one przechodza przez
      magistrale. Czytanie bufora karty z powrotem kosztowaloby wiecej
      niz zapisy, ktore by oszczedzilo - stad kopia, a nie porownanie z
      ekranem.

      Zmierzone, nie oszacowane: komenda 'ekran' pokazuje ile wierszy
      poszlo w ostatniej klatce. Bezczynny pulpit: ZERO. Z tykajacym
      zegarem na pasku: szesc wierszy. Test tego pilnuje.

      I blad, ktory ta optymalizacja przyniosla - bo przyniosla.
      Konsola na buforze obrazu (fbcon) trzyma adres bufora WIDOCZNEGO
      i pisze prosto na niego, nie wiedzac nic o buforze roboczym
      kompozytora. Dopoki cala klatka szla na ekran, nie mialo to
      znaczenia: nastepna klatka i tak zakrywala napis. Z porownaniem
      wierszy - ma. Kompozytor rysowal wlasciwa rzecz, porownywal ja z
      kopia, ktora juz sie zgadzala, i nie wysylal nic; napis z jadra
      zostawal na ekranie na zawsze. Wysypala sie na tym grupa
      'gterm64', bo jej OCR czytal te resztki jako tresc okna.

      Naprawa jest tam, gdzie problem: kazdy, kto pisze po widocznym
      buforze za plecami video.c, mowi to przez video_front_touched(),
      a nastepna klatka idzie caloscia. Pierwsze podejscie wlozylem w
      video_console_cell() i bylo bezuzyteczne - vga.c wola fbcon_cell
      bezposrednio, z pominieciem tamtej funkcji. Wiec haczyk siedzi w
      samym fbcon.c.

      Grupa 'iso' pyta o to wprost: pulpit stoi na buforze od loadera,
      z shella jadra leci komenda 'mem', i po niej tla ma byc tyle
      samo, co przed. To jest asercja, ktora bez tego bledu przechodzi,
      a z nim nie.

      Czego to nadal nie daje: to jest bufor ramki bez GPU. Zadnego
      przyspieszania, tryb ustawia za nas GRUB albo UEFI, jeden monitor.
      Sterownik karty to projekt wielkosci calego dotychczasowego
      systemu i nie tam bym szedl.

      Nowa grupa 'kolory' liczy to, czego nie da sie podrobic: pulpit
      rysowany indeksami ma 29 roznych kolorow na calym ekranie, a
      wpuszczony w okno gradient JPEG - 3031, i rosnie w prawo czerwienia
      i w dol zielenia, wiec to nie jest "duzo kolorow" tylko "wlasciwe
      kolory".

- [x] **Laptop: touchpad, kolko i rozdzielczosc wlasna panelu** - bo
      "najpierw na laptopie bede system testowac".

      **Blad, ktory na laptopie bylby widoczny od pierwszej sekundy.**
      mouse.c rysowal "M x,y bN" w rogu ekranu przy KAZDYM pakiecie
      myszy, a pit.c "uptime: Ns" co sekunde. W trybie tekstowym to byla
      diagnostyka. Pod UEFI trybu tekstowego nie ma - vga_write_at idzie
      przez fbcon prosto na WIDOCZNY bufor - wiec sterownik myszy malowal
      po pulpicie, z przerwania, kilkaset razy na sekunde. Widac to bylo
      na zrzucie jako napisy wiszace nad tapeta bez zadnego okna.

      Naprawa: kompozytor mowi vga_screen_taken(1), gdy bierze ekran, i
      rog tekstowego ekranu przestaje byc miejscem. Trzy wywolania
      naraz - mysz, uptime, watki demonstracyjne z kmain.c - i wszystkie
      i tak pisza do dziennika szeregowego, gdzie diagnostyka nalezy.

      Przy okazji: bez tego kazdy ruch palcem po touchpadzie unieważnial
      kopie ekranu (bo napis szedl na bufor widoczny), wiec optymalizacja
      z poprzedniego wpisu przestawala dzialac dokladnie wtedy, gdy jest
      najbardziej potrzebna.

      **Kolko, czyli dwa palce.** Touchpad na porcie PS/2 to mysz: melduje
      ruch wzgledny i przyciski dokladnie tak samo, a stukniecie w tafle
      zamienia na klikniecie jego wlasny firmware. Czego mysz z 1987
      roku nie miala, a touchpad ma, to kolko - i tak wlasnie zglasza
      dwa palce sunace po tafli. To jest rozszerzenie IntelliMouse:
      czwarty bajt, o ktory trzeba poprosic pukaniem, jakiego nikt nie
      wysle przypadkiem - trzy czestotliwosci probkowania, 200, 100 i
      80 - po czym urzadzenie mowi, kim jest. Odpowiedz 3 znaczy, ze
      czwarty bajt jest.

      Znak jest odwrotny, niz sie wydaje: protokol liczy zab KU
      CZLOWIEKOWI jako dodatni, a cala reszta systemu nazywa dodatnim
      kierunek OD czlowieka, bo w te strone jedzie dokument. Odwrocenie
      siedzi w sterowniku, zeby nikt wyzej nie musial o tym pamietac -
      wyszlo to na tescie, ktory pokazal liste jadaca w zla strone.

      Przy okazji parser pakietow: byl switch z etykieta i skokiem, jest
      tablica i licznik.

      **WEV_SCROLL idzie do okna POD WSKAZNIKIEM**, nie do tego z fokusem.
      Tak robi kazdy inny system i to jest wlasciwa strona: patrzy sie na
      liste i przewija ja, nie robiac jej wpierw oknem od klawiatury.
      Obsluzyly to gweb, gfiles i gedit.

      W edytorze wyszla rzecz do przemyslenia: draw() wolal
      scroll_to_caret() co klatke, wiec przewiniecie natychmiast by sie
      cofalo. Stad follow_caret - kolko puszcza kursor, pierwszy klawisz
      lapie go z powrotem. Przewijanie dokumentu to patrzenie gdzie
      indziej, a nie przenoszenie sie tam.

      **Rozdzielczosc: jadro samo sobie ja psulo.** boot/boot.S prosil
      loader o 1024x768. Prosba w naglowku Multiboot2 WYGRYWA z
      gfxpayload, wiec panel 1920x1080 dostawal cwiartke siebie. Naglowek
      64-bitowy mial tam zero ("dowolna") juz od dawna, z komentarzem
      tlumaczacym dlaczego; 32-bitowy nie mial, i to bylo cale zrodlo
      malego pulpitu. Zmierzone, nie zgadniete: z prosba - 1024x768, bez
      prosby - to, co da loader. Tak samo grub.cfg dostal 'insmod
      all_video' i 'gfxpayload=keep', ktore plyta 64-bitowa miala, a
      32-bitowa nie.

      **Bufory okien wyszly ze sterty do PMM.** Bufor okna to najwieksza
      pojedyncza rzecz, jaka ten system alokuje: przy wlasnej
      rozdzielczosci laptopa zmaksymalizowane okno w kolorach to osiem
      megabajtow, czyli CALA sterta. Trzymanie najwiekszych i
      najmniejszych obiektow na jednej liscie pierwszego dopasowania to
      przepis na fragmentacje. Teraz ida calymi stronami prosto z
      alokatora ramek, i rozmiar jest kwestia panelu, a nie liczby
      wpisanej w kod.

      **I rzecz, ktorej nie bylo w planie.** 1920x1080 zadzialalo od
      razu - i bylo NIECZYTELNE. Cale UI stoi w pikselach bezwzglednych,
      font ma osiem pikseli, a osiem pikseli na 15-calowym panelu 1080p
      to poltora milimetra. System poprawny i bezuzyteczny.

      Odpowiedz: kompozytor rysuje we WLASNYM ekranie, logicznym, a na
      panel idzie ten ekran z kazdym pikselem polozonym jako kwadrat
      czterech. video_w() i video_h() odpowiadaja w pikselach logicznych
      - i ani jedna linia rozkladu nigdzie nie musiala sie zmienic.
      Maszyna, w ktorej kazde okno, ikona i pasek stanu stoja w
      wspolrzednych bezwzglednych, inaczej wymagalaby przepisania ich
      wszystkich, a z nimi geometrii w kilkudziesieciu grupach testowych.

      Trzeba powiedziec wprost, co to kosztuje: to jest podwajanie
      pikseli, nie skalowanie elementow. Litery sa tymi samymi literami
      8x8, narysowanymi dwa razy wiekszymi - nie ostrzejszymi. Zdjecie
      na takim ekranie ma polowe szczegolu, ktory panel mogl by uniesc.
      W zamian: zero zmian wyzej i zero ryzyka.

      Skala jest ustawieniem ('set skala'), a zero znaczy "niech
      zdecyduje ekran": panel wysoki na 900 pikseli i wiecej dostaje 2,
      wszystko mniejsze 1 - bo podwojenie 640x480 dalo by pulpit 320
      pikseli w poprzek, a to nie jest pulpit. Dzieki temu wszystkie
      dotychczasowe testy zostaja przy skali 1 i nie zmienia sie w nich
      nic.

      Grupa 'skala' pyta o mechanizm przy skali wymuszonej recznie, i
      pyta uczciwie: nie tylko czy sasiednie piksele sa rowne w parach
      (11100 na 11100), ale takze czy ta sama siatka przesunieta o
      piksel juz sie NIE zgadza (8590 na 11100). Pierwsza liczba bez
      drugiej nie znaczylaby nic.

- [x] **Mysz USB - i maszyna, ktora nie ma PS/2 w ogole** - druga polowa
      odpowiedzi na "najpierw na laptopie".

      Wskaznik przychodzil przez kontroler 8042 od pierwszego dnia i na
      komputerze stacjonarnym dalej tak przychodzi. Laptop z ostatnich
      lat moze nie miec 8042 wcale: ani klawiatury PS/2, ani touchpada
      PS/2, bo nie ma tych portow. Jego wskaznik to urzadzenie HID na
      magistrali USB, i klawiatura tak samo.

      Wiec sterownik myszy obok sterownika klawiatury, ta sama droga -
      porty, ktore melduje warstwa kontrolera, deskryptor konfiguracji,
      interfejs klasy HID w wersji rozruchowej - i rozstaja sie na
      JEDNYM bajcie: protokol interfejsu to 1 dla klawiatury i 2 dla
      myszy. Raport rozruchowy ma trzy bajty, czasem cztery: przyciski,
      ruch w X, ruch w Y i - na urzadzeniu z kolkiem - liczba zebow.
      Bajty ruchu sa ZE ZNAKIEM i trzeba je tak czytac: ruch o minus
      jeden przychodzi jako 0xFF, a wziety jako 255 rzucilby wskaznik
      przez caly ekran.

      Wspolne kawalki - budowanie osmiu bajtow zadania i dwa rodzaje
      transferu sterujacego - wyszly z usbkbd.c do usbhc.c. Siedzialy
      tam, dopoki klawiatura byla jedynym urzadzeniem; drugie by je
      skopiowalo, a dwie kopie formatu na drucie to sposob, w jaki
      formaty sie rozjezdzaja.

      **Pulapka, ktora sam bym zastawil.** Oba sterowniki chodza po tych
      samych portach i kazdy zatrzymuje sie na pierwszym urzadzeniu,
      ktore rozpozna - przygladajac sie po drodze wszystkim wczesniejszym.
      A przygladanie sie znaczy RESET portu. Drugi przebieg wywrocilby
      klawiature, ktora pierwszy wlasnie uruchomil. Wiec port jest witany
      raz, a jego uchwyt zapamietany: drugi pytajacy dostaje to samo
      urzadzenie, a nie urzadzenie wlasnie zresetowane pod wlascicielem.

      Ruch z obu drog konczy sie w mouse_inject(), bo wskaznik na ekranie
      jest jeden i kompozytor nie ma miedzy czym wybierac. Znak kolka w
      HID jest juz taki, jakiego uzywa reszta systemu - w odroznieniu od
      pakietu PS/2, ktory trzeba odwrocic.

      **Dwa bledy po drodze, oba moje, oba zlapane przez testy.**

      Pierwszy: usbhc_init() bezwarunkowo startowal kontroler, a start
      znaczy RESET. Drugi sterownik, pytajac o kontroler, kasowal slot i
      punkt koncowy, ktorych pierwszy juz uzywal - i klawiatura USB byla
      znajdowana, meldowana i cicha. Teraz raz znaleziony kontroler
      odpowiada od razu.

      Drugi, subtelniejszy: nawet nie resetujac portu, drugi sterownik
      CZYTAL deskryptory urzadzenia, z ktorym pierwszy juz rozmawial -
      dwie rozmowy jednym drutem. Stad usbhc_claim(): sterownik, ktory
      wzial urzadzenie, mowi to, i od tej pory port odpowiada "nic tu
      nie ma" wszystkim innym.

      Zadnego z nich nie widac w kodzie, ktory sie kompiluje; oba wyszly
      dopiero z uruchomionej grupy 'usbkbd'. To jest dokladnie ten
      rodzaj bledu, dla ktorego te testy istnieja.

      Grupa 'usbmysz' pyta o dwie rzeczy, i druga jest ta, ktora sie
      liczy: ze maszyna znajduje mysz na kontrolerze xHCI i mowi gdzie,
      oraz ze przy WYLACZONYM 8042 pulpit wstaje, a jego kursor idzie za
      mysza. Pytanie musi byc zadane w pikselach, nie slowami - bo
      maszyna bez 8042 nie ma tez klawiatury PS/2, wiec nie ma jak
      niczego WPISAC. Co poruszylo tym kursorem, przyszlo przez USB, bo
      innej drogi tam nie ma.

- [x] **Klawiatura milkla, gdy dolaczala mysz - i raport sprzetowy dla
      pierwszego rozruchu na obcej maszynie.**

      Laptop okazal sie Dellem. Dobra wiadomosc: Dell trzyma wbudowana
      klawiature na i8042 nawet w nowych modelach. Touchpad to loteria
      zalezna od rocznika - starsze sa PS/2, nowsze od okolo 2016 siedza
      na I2C-HID, ktorego nie umiemy. Wiec zamiast zgadywac, ktora to
      maszyna, trzeba bylo sprawic, zeby SAMA powiedziala.

      **Pierwsze klamstwo do usuniecia.** mouse_init() nigdy nie
      sprawdzalo, czy cokolwiek jest na porcie pomocniczym: mouse_ack()
      czytalo bajt i wyrzucalo. Maszyna bez myszy PS/2 wygladala
      dokladnie jak maszyna z mysza - kazde polecenie "dzialalo",
      handler przerwania wchodzil, a wskaznik sie nie ruszal i nikt nie
      mowil dlaczego. Teraz brak potwierdzenia 0xFA konczy sprawe i
      rozruch mowi to wprost. Tak samo wait_write/wait_read: na maszynie
      bez 8042 kazdy odczyt portu daje 0xFF, a 0xFF ma ustawiony ten
      bit - bez sprawdzenia czekalismy pelna liczbe obrotow na kazdy
      bajt kazdej komendy i nazywali to sukcesem.

      Drugie: raport mowil "klawiatura tak" bez zadnej podstawy. To
      dokladnie ten sam blad, ktory zarzucilem myszy. Teraz pyta o sam
      kontroler - maszyna, ktora go nie ma, odpowiada 0xFF na wszystko,
      i to jest klasyczny i uczciwy test.

      **Komenda 'sprzet'** - napisana pod pierwszy rozruch na sprzecie,
      ktorego nikt nie probowal. Ekran (skad bufor, jaka skala), porty
      PS/2, co znalazlo sie na USB, i spis urzadzen PCI. Ten ostatni
      jest tam po cos: touchpad, ktorego nie ma na PS/2, siedzi zwykle
      na kontrolerze I2C, a kontroler I2C jest urzadzeniem klasy
      magistrali szeregowej widocznym w tym spisie. Nie umiemy tej
      magistrali - ale moc POWIEDZIEC, ze to ona, to roznica miedzy
      zagadka a znana luka.

      **I blad, ktory ta praca odslonila - w sterowniku xHCI.** Byl
      napisany dla dokladnie jednego urzadzenia i nikt tego nie
      zauwazyl, bo do dzis bylo jedno. Jeden bufor na caly kontroler:
      dwa urzadzenia maja dwa odczyty w locie naraz, wiec raport jednego
      lada w miejscu, z ktorego drugi wlasnie czyta. I jeden pierscien
      zdarzen: kto pierwszy zajrzy, widzi CUDZE zakonczenia, a wziecie
      nie swojego gubi tamten raport na zawsze.
      Objaw: klawiatura milkla w chwili, gdy do magistrali dolaczala
      mysz - czyli dokladnie na laptopie z klawiatura i touchpadem po
      USB.

      Naprawa: bufor na slot, a zdarzenie odkladane pod swoim slotem
      (numer slotu jest w najstarszym bajcie slowa sterujacego - i tylko
      dlatego dwa urzadzenia moga w ogole dzielic jeden pierscien).

      Po drodze zepsulem to dwa razy i za kazdym razem powiedzialy o tym
      testy, nie kompilator. Najpierw sort_events pozeralo zdarzenia
      ZAKONCZENIA POLECEN, na ktore czekala enumeracja - stad osobna
      kolejka na to, co nie jest transferem. Potem okazalo sie, ze na
      slot potrzeba KOLEJKI, a nie jednego miejsca: transfer sterujacy
      konczy sie dwa razy, raz dla danych i raz dla statusu, a przy
      jednym miejscu drugie nadpisywalo pierwsze i wywolujacy czekal w
      nieskonczonosc na cos, co juz mu powiedziano. Za trzecim razem
      cofnalem sie do stanu z commita, potwierdzilem, ze dziala, i
      dopiero wtedy zrobilem to raz a porzadnie.

- [x] **Dysk, ktory nie jest nasz, nie jest nasz do wziecia** - i to jest
      wpis, ktory powinien byl powstac dawno temu.

      Pytanie brzmialo "a jakis instalator do tego systemu". Zaczalem od
      sprawdzenia, czym instalator w ogole moglby tu byc, i po drodze
      trafilem na to:

          if (memcmp(sb->magic, MAGIC, 4) != 0)
              format_disk();

      fs_init() przy KAZDYM starcie sprawdzal, czy pierwszy blok mowi
      PFS3 - a jesli nie, formatowal dysk. Bez pytania, bez spojrzenia,
      co tam jest.

      Bylo to nieszkodliwe tak dlugo, jak kazdy dysk, jaki ten system
      widzial, byl pustym plikiem zrobionym przez test. Przestaje byc w
      chwili, gdy trafia na prawdziwa maszyne: dysk laptopa tez nie mowi
      PFS3. Uruchomienie tego z pendrive'a na Dellu skasowaloby tablice
      partycji, Windowsa i wszystko inne w pierwszej sekundzie
      pierwszego rozruchu.

      Zobaczylem to nie z lektury, tylko dlatego, ze moj obraz testowy z
      GPT wrocil jako "bez tablicy partycji" - jadro zdazylo go
      wyczyscic, zanim zdazylem go odczytac.

      Teraz formatowany jest wylacznie dysk NAPRAWDE pusty. "Pusty"
      odpowiedziane ostroznie, bo koszt pomylki w jedna strone to nowy
      dysk, ktory czlowiek formatuje recznie, a w druga - czyjes dane:
      sygnatura tablicy partycji w sektorze zerowym dyskwalifikuje od
      razu, a poza tym pierwsze szescdziesiat cztery kilobajty musza byc
      w calosci zerami. Wszystko inne zostaje nietkniete i powiedziane
      wprost, a kto naprawde chce ten dysk, mowi to komenda 'formatuj'.

      Potwierdzenie jest slowem wpisanym w calosci - TAK, duzymi
      literami - bo klawisz do wcisniecia to rzecz, ktora czlowiek robi
      odruchowo, a slowo trzeba postanowic napisac.

      **Czytanie tablicy partycji** (kernel/part.c) powstalo przy tej
      okazji i jest fundamentem calej reszty: MBR i GPT, przy czym
      wykrycie jest jednym bajtem - typ 0xEE w sektorze zerowym znaczy
      "patrz do sektora pierwszego". GPT trzyma tam wpis ochronny
      obejmujacy caly dysk wlasnie po to, zeby program znajacy tylko
      stary format zobaczyl dysk pelny zamiast pustego i nie zaproponowal
      uprzejmie spartycjonowania go. Nic w tym pliku nie pisze.

      Grupa 'dysk' pyta o powsciagliwosc i o to, na czym ona polega:
      obraz z prawdziwa tablica GPT wchodzi w rozruch i wychodzi z niego
      z ta sama tablica - odczytana Z POWROTEM PO STRONIE HOSTA, bajt po
      bajcie, a nie wzieta na slowo systemu. Pusty dysk dalej jest
      formatowany. Zla odpowiedz na 'formatuj' nie kasuje niczego, a
      wpisane TAK kasuje - i wtedy tablicy juz nie ma, bo o to sie
      prosilo.

      **PFS w partycji** - drugi krok, zrobiony od razu, bo bez niego
      poprzedni jest tylko odmowa. Przesuniecie siedzi w DWOCH
      funkcjach dotykajacych dysku i nigdzie indziej, wiec zaden kod
      wyzej nie musi wiedziec, czy stoi na calym dysku, czy na jego
      kawalku. 'formatuj N' zajmuje jedna partycje - tablica i sasiedzi
      zostaja tacy, jacy byli - a przy nastepnym rozruchu jadro samo
      znajduje swoj PFS, szukajac partycji, ktorej pierwszy blok mowi
      PFS3. Bez rejestru GUID-ow, bez nowych umow: system rozpoznaje
      sie po tym, co juz zapisuje.

      Test tego sprawdza rzecz, ktora latwo udawac. Nie tylko czy
      tablica przezyla - to 34 sektory - ale czy SASIEDNIE PARTYCJE
      dalej maja swoje dane, bo to sa setki megabajtow i to o nie
      naprawde chodzi. Znacznik wpisany przed, odczytany po, po stronie
      hosta.

      Czego to NIE jest: instalatora. Zeby maszyna wystartowala z tej
      partycji, brakuje jeszcze zapisu do partycji EFI - czyli FAT-u,
      ktorego nie mamy - i programu rozruchowego EFI. To sa nastepne
      kroki, nie ten.

- [x] **protoboot: wlasny program rozruchowy UEFI** - etapy 1-3.

      Do dzis ten system uruchamial GRUB. GRUB jest znakomity i jest
      CUDZY; wszystko inne tutaj jest napisane od zera, a rzecz, ktora
      kladzie jadro w pamieci, byla ostatnim kawalkiem, ktory nie byl.

      **Etap 1 zdejmowal najwieksze ryzyko** i tylko o to chodzilo: nie
      wiedzialem, czy ten toolchain w ogole zbuduje aplikacje EFI.
      Aplikacja EFI to plik PE32+ z konwencja wywolan Microsoftu, czyli
      dwie rzeczy, ktorych GCC na Linuksie nie robi. Robi je clang
      celujacy w x86_64-unknown-windows plus lld-link; 'file' mowi "PE32+
      executable (EFI application)", firmware to zaladowalo i wykonalo.
      Dwa kilobajty, bez gnu-efi i bez EDK2. Jedyne, co cudze, to format
      pliku.

      **Deklaracje UEFI sa nasze** (boot/efi/efi.h) i dwie rzeczy trzeba
      przy nich zrobic dobrze. KOLEJNOSC skladnikow: firmware podaje
      tablice wskaznikow do funkcji, a my indeksujemy po pozycji -
      pominiety wpis przesuwa wszystko po nim i maszyna skacze w zla
      funkcje. Dlatego kazdy wpis tablicy uslug rozruchowych jest tam, w
      kolejnosci, nawet te, ktorych nigdy nie wolamy. I KONWENCJA
      WYWOLAN, ktora zalatwia wybor kompilatora, wiec nie musi jej
      powtarzac ani jedna linijka.

      **Etapy 2 i 3**: czytanie jadra i initrd, tryb graficzny, ACPI.
      Wlasnego FAT-u do rozruchu NIE POTRZEBUJEMY - firmware ma juz
      zamontowana partycje, z ktorej nas uruchomiono, i podaje protokol
      plikowy. FAT bedzie potrzebny do ZAPISANIA takiej partycji, czyli
      przy instalacji, a nie tutaj. Rozmiar pliku brany przez pojscie na
      koniec i zapytanie, gdzie sie jest - co oszczedza EFI_FILE_INFO,
      GUID-a i bufora, dla odpowiedzi bedacej jedna liczba.

      Tryb graficzny brany taki, jaki firmware juz pokazuje: na laptopie
      jest to rozdzielczosc wlasna panelu, czyli dokladnie to, czego
      chcemy, a proszenie o konkretny tryb wlasnie by to wyrzucilo. To
      ta sama lekcja, co przy naglowku Multiboot2 - prosba o konkret
      wygrywa i psuje.

      RSDP z tablicy konfiguracyjnej firmware'u, bo na maszynie UEFI nie
      ma innej drogi: stare szukanie po niskiej pamieci nie znajdzie tam
      nic.

      Zmierzone pod OVMF: jadro 825028 B, initrd 448132 B - co do bajtu
      tyle, ile maja pliki - ekran 1280x800, RSDP w wersji 2.0.

      **Loader laduje jadro 64-bitowe i tylko je.** Poczatkowo siegal po
      protonos.bin, co bylo bezmyslne: maszyna z UEFI jest maszyna
      64-bitowa. Jadro 32-bitowe istnieje w tym projekcie po to, zeby
      jeden zestaw zrodel byl kompilowany dwa razy i drugie kompilowanie
      powiedzialo, co ukrylo pierwsze - to jest dobry powod, zeby je
      trzymac, i nie jest powodem, zeby je uruchamiac na laptopie.
      Wpuszczanie go tutaj znaczyloby loader ELF32 obok ELF64 dla
      przypadku, ktorego nikt nie ma; ogolnosc, o ktora nikt nie prosil,
      to sposob, w jaki loader przestaje byc godny zaufania. GRUB dalej
      uruchamia tamto jadro komu trzeba.

      Przy okazji dwie rzeczy, ktore to ustalilo: jadro 64-bitowe jest
      ELF64 (nie ELF32, jak zakladalem, planujac wczytywanie), a modul
      initrd jadro bierze PO POZYCJI, nie po nazwie - wiec loader ma
      podac jeden modul i tyle.

      **Czego jeszcze nie ma**: zbudowania tablicy tagow Multiboot 2,
      wczytania ELF-a, ExitBootServices i skoku. Ten ostatni jest
      niezrecznosc calego przedsiewziecia: aplikacja UEFI biegnie w
      trybie dlugim, a jadro Multiboot 2 wchodzi w 32-bitowym trybie
      chronionym, wiec po pozegnaniu firmware'u trzeba z trybu dlugiego
      wyjsc samemu - na wlasnych tablicach, z wylaczonym stronicowaniem.

      Ksztalt "powiedzenia jadru" jest juz ustalony i nie jest nasz do
      zmieniania: jadro to obraz Multiboot 2 i chce magicznej liczby w
      EAX oraz tablicy tagow w EBX. Wiec loader te tablice ZBUDUJE,
      dokladnie tak jak robi to GRUB, a jadro sie nie dowie, ze cokolwiek
      sie zmienilo. Ani jedna jego linijka nie musiala.

- [x] **protoboot: etap 4 - jadro startuje z naszego loadera** (grupa
      testow `protoboot`).

      Etapy 1-3 czytaly pliki i pytaly firmware o ekran. Etap 4 jest
      tym, po co to wszystko bylo: zbudowac tablice tagow Multiboot 2,
      wczytac ELF-a, pozegnac firmware i skoczyc. Od chwili, gdy
      ExitBootServices wroci, nie ma juz nikogo do pomocy - konsola
      firmware'u nie istnieje, wiec kazdy blad w tej sekwencji to
      maszyna, ktora gasnie bez slowa.

      **Tablica tagow jest zbudowana pod to, co jadro faktycznie
      czyta**, a nie pod cala specyfikacje. kernel/mb2.c rozpoznaje
      siedem typow: koniec, wiersz polecen, modul, mape pamieci, bufor
      obrazu i dwie postacie RSDP. Loader wystawia dokladnie te.
      Wystawianie reszty byloby kodem, ktorego nikt nie czyta - a wiec
      kodem, ktory nikt nie sprawdzi.

      **Mapa pamieci UEFI ma pulapke i jest to najwazniejsza rzecz w
      tym etapie.** Firmware oddaje mape swoich regionow; przelozenie
      jej na Multiboot 2 jest zamiana typow. Kuszace jest uznac
      EfiLoaderCode i EfiLoaderData za wolne - przeciez loader zaraz
      przestanie istniec. Tyle ze W TEJ WLASNIE PAMIECI lezy initrd,
      ktory loader wczytal z dysku. Oddane jako wolne, trafilyby do
      alokatora jadra i zostalyby rozdane pierwszemu, kto poprosi -
      a initrd zamienilby sie w smieci, zanim ktokolwiek by go otworzyl.
      Wiec ida do mapy jako ZAJETE. Test pilnuje tego nie przez
      ogladanie mapy, tylko przez uruchomienie programu z initrd:
      jesli te strony ktos rozdal, program nie wystartuje.

      **Zejscie z trybu dlugiego.** Aplikacja UEFI biegnie w trybie
      dlugim, jadro Multiboot 2 wchodzi w 32-bitowym trybie chronionym.
      Kolejnosc jest przymusowa i nie ma w niej miejsca na wlasne
      pomysly: wlasna GDT (firmware'owa znika razem z firmware'em),
      daleki skok do segmentu 32-bitowego, wylaczenie stronicowania w
      CR0, wyczyszczenie EFER.LME, wylaczenie PAE w CR4 - i dopiero
      wtedy skok pod punkt wejscia z EAX i EBX ustawionymi tak, jak
      ustawia je GRUB. Adresy w tej sekwencji sa juz fizyczne, bo
      UEFI mapuje pamiec tozsamosciowo; gdyby nie mapowalo, tego kroku
      nie daloby sie zrobic w miejscu.

      **Zadziolalo za pierwszym razem i nie jest to powod do dumy, tylko
      skutek tego, ze ksztalt byl ustalony wczesniej.** Jadro nie
      dowiedzialo sie o niczym: ta sama liczba magiczna, ta sama tablica,
      ten sam modul. Ani jedna linijka jadra nie zmienila sie na potrzeby
      tego loadera - i to jest miara, czy interfejs byl dobry.

      **Grupa testow `protoboot`** startuje maszyne, na ktorej NIE MA
      GRUB-a: partycja EFI z trzema plikami i firmware OVMF. Rozmiary
      jadra i initrd sprawdzane sa co do bajtu wzgledem plikow na dysku
      (loader ma je przeczytac, a nie powiedziec, ze przeczytal),
      rozdzielczosc z GOP porownana z ta, ktora zobaczylo jadro, i z ta,
      ktora ma zrzut ekranu. Sam plik BOOTX64.EFI czytany jest bajtowo:
      PE32+, maszyna 0x8664, podsystem 10 - bo tylko taki firmware w
      ogole uruchomi.

      **Czego dalej nie ma**: zapisu do FAT-u i instalatora. Loader umie
      juz uruchomic system z pendrive'a przygotowanego na hoscie; zeby
      ProtonOS umial przygotowac go sam - obok Windowsa, na dysku
      laptopa - musi umiec ZAPISAC partycje EFI. To jest nastepny krok.

- [x] **FAT12/16/32 - jedyny cudzy system plikow w tym projekcie**
      (kernel/fat.c, kernel/fatcmd.c, grupa testow `fat`).

      Ten system trzyma swoje pliki w PFS3 i nie ma powodu chciec
      czegokolwiek innego - poza jednym miejscem. Partycja EFI jest
      FAT-em z urzedu: firmware kazdej maszyny UEFI umie czytac FAT i
      nie umie czytac niczego innego, wiec program rozruchowy lezacy
      gdzie indziej nigdy nie zostanie uruchomiony. Zainstalowanie tego
      systemu obok innego znaczy wiec ZAPISAC do partycji, ktora juz
      istnieje, ma juz w sobie czyjs loader i ma dzialac dalej.

      Stad ksztalt sterownika. Czytanie jest kompletne. Zapis robi to,
      czego potrzebuje instalator - katalog, plik, jego tresc - i nic
      ponadto: nic tu nie formatuje, nic nie przenosi cudzych plikow,
      nic nie przepisuje struktury, ktorej nie musialo dotknac. Kazdy
      zapis idzie do WSZYSTKICH kopii tablicy alokacji, bo system,
      ktory uruchomi te maszyne jutro, moze czytac dowolna z nich.

      **Ktora to z trzech odmian, nie mowi nic w sektorze rozruchowym.**
      Mowi to LICZBA KLASTROW, policzona z rozmiarow. To nie jest
      zlosliwosc specyfikacji, tylko jedyna rzecz, ktora jest o FAT-cie
      naprawde prawdziwa - a odczytanie nazwy z napisu w sektorze
      rozruchowym, czyli miejsce, w ktorym mieszka pierwszy blad
      kazdego, myli sie na woluminach, ktore glosza "FAT16   " i nim
      nie sa.

      **FAT12 nie jest ciekawostka.** Jego wpis ma dwanascie bitow,
      wiec co drugi lezy okrakiem na granicy sektora tablicy. Sterownik
      napisany na cale wpisy lamie sie wlasnie tam. Dlatego caly dostep
      do tablicy - chodzenie po lancuchu, przydzielanie, zwalnianie -
      idzie przez POJEDYNCZE BAJTY: bajt nie lezy okrakiem na niczym.

      **Dlugie nazwy sa czytane i zapisywane**, i nie jest to
      uprzejmosc. Loader szuka pliku 'protonos64.bin', ktore nie miesci
      sie w osmiu znakach i kropce; nagiecie nazwy do ograniczen
      systemu plikow byloby pozwoleniem, zeby to system plikow
      decydowal, jak nazywaja sie pliki tego systemu. Nazwa, ktora
      MIESCI sie w 8.3, dostaje jeden zwykly wpis - dokladnie taki, jaki
      zapisalby kazdy inny - z dwoma bitami w bajcie na pozycji
      dwunastej, ktore pamietaja, ze byla pisana malymi literami.

      **Co jest po drugiej stronie kazdej asercji.** Obrazy FAT w
      testach robi mtools i to mtools czyta z powrotem wszystko, co
      system do nich zapisal: katalog, dluga nazwe wraz z jej nazwa
      krotka (PROTON~1.BIN), tresc plikow, zniknięcie usunietego pliku.
      Sterownik, ktory zgadza sie tylko sam ze soba, nie udowodnil
      niczego - a juz najmniej na partycji nalezacej do systemu, ktory
      uruchamia te maszyne. Osobno sprawdzane jest to, czego robic nie
      wolno: cudzy bootmgfw.efi na tej samej partycji, tablica GPT i
      sasiednia partycja Windows sa po wszystkim czytane z hosta i
      musza byc nietkniete.

      **fatcopy jest instalatorem w miniaturze.** Przepisuje plik z
      initrd na partycje EFI - a instalowanie tego systemu to dokladnie
      to: loader, jadro i initrd sa plikami, ktore dzialajacy system juz
      ma. Komenda powstala przed instalatorem, bo jest ta czescia, ktora
      da sie sprawdzic osobno, co do bajtu, programem, ktory nie jest
      nasz. Test przepisuje libproton.so (83 272 B) i porownuje z
      zawartoscia initrd.

      **Komendy sa w osobnym pliku** (kernel/fatcmd.c), bo obie powloki
      - 32- i 64-bitowa - maja je mieć, a zadna nie powinna miec
      wlasnej kopii.

- [x] **Instalator: ProtonOS na dysk maszyny** (kernel/install.c, grupa
      testow `instaluj`).

      Na maszynie z UEFI instalowanie jest mniejsze, niz slowo
      sugeruje: trzy pliki zapisane na partycji EFI, ktora juz tam
      jest. Firmware szuka na niej programu rozruchowego i uruchamia
      go - to caly mechanizm. Wiec nic tutaj nie tworzy partycji, nic
      nie rusza tablicy i nic nie usuwa ani nie przenosi zadnego pliku
      nalezacego do systemu, ktory na tym dysku juz mieszka.

      **Skad biora sie te trzy pliki - to jest sedno.** Jadro dzialajace
      w tej chwili to zaladowane segmenty, a nie plik, z ktorego
      przyszlo; a zapisac trzeba plik. Wiec loader podaje wszystkie trzy
      jako moduly Multiboot 2: initrd tak, jak podawal zawsze, a obraz
      jadra i SIEBIE SAMEGO - po nazwie. System uruchomiony inaczej
      (QEMU z -kernel, GRUB z plyty, ktora nie byla o tym robiona) nie
      ma czego instalowac i mowi to wprost, zamiast zapisac cos
      przyblizonego.

      Moduly poza initrd bierze sie PO NAZWIE, nie po pozycji.
      Alternatywa jest umowa loadera z jadrem co do kolejnosci, czyli
      rodzaj umowy, ktora psuje sie po cichu.

      **Dwie sciezki, i roznica miedzy nimi jest cala etyka tej
      komendy.** /EFI/ProtonOS jest nasz i zapisujemy go zawsze.
      /EFI/BOOT/BOOTX64.EFI - nazwa, do ktorej firmware sie cofa, gdy
      nic innego nie jest ustawione - zapisujemy TYLKO WTEDY, GDY GO
      TAM NIE MA. Na maszynie, ktora go ma, nalezy on do tego, kto te
      maszyne dzisiaj uruchamia; zabranie go byloby instalacja NA
      MIEJSCE kogos, a nie OBOK niego. Wtedy droga wejscia jest wpis w
      menu firmware'u i powiedzenie tego jest lepsze niz maszyna, ktora
      po cichu przestaje uruchamiac to, co uruchamiala.

      Potwierdzenie jest slowem wpisanym z klawiatury, tak samo jak przy
      'formatuj' i z tego samego powodu: klawisz do nacisniecia to
      odruch, a slowo to decyzja - a po drugiej stronie jest czyjs
      laptop.

      **Test zamyka petle.** Maszyna wstaje z pendrive'a (partycja EFI
      podana QEMU jako katalog), dysk 'tej drugiej maszyny' wisi na
      kontrolerze AHCI - a wiec jest tym, ktory system wybiera - i ma na
      sobie tablice GPT, FAT-a zrobionego przez mtools i cudzego
      bootmgfw.efi. Po instalacji kazdy z trzech plikow jest czytany z
      obrazu przez mtools i porownywany co do bajtu, a cudzy plik,
      tablica i sasiednia partycja musza byc nietkniete. Potem TEN DYSK
      JEST URUCHAMIANY SAM, bez pendrive'a - i to jedyny dowod, ze
      instalacja cokolwiek zainstalowala. Osobno sprawdzane jest, ze bez
      wpisania TAK nie dzieje sie nic, i osobno drugi dysk - taki, ktory
      cudzy /EFI/BOOT/BOOTX64.EFI juz ma - gdzie ten plik po wszystkim
      musi byc bajt w bajt ten sam.

      Przy okazji dwie poprawki w loaderze: pliki wczytuje teraz przez
      AllocateMaxAddress ponizej czterech gigabajtow (adres modulu
      Multiboot 2 ma trzydziesci dwa bity i nie ma gdzie zapisac
      wiekszego - dotad bylo to ciche zalozenie), a szukajac jadra i
      initrd zaglada takze do \EFI\ProtonOS - czyli tam, gdzie kladzie
      je instalator.

- [x] **Instalator ma wlasny ekran, a nie okno na pulpicie**
      (user/ginstal.c, tryb kiosku w kernel/desktop.c, grupa testow
      `ginstal`).

      Najpierw powstal jako zwykle okno wsrod innych okien i tak bylo
      zle. Instalowanie nie jest jednym z zadan: kiedy trwa zapis na
      czyims dysku, pasek zadan i cudze okna w zasiegu reki nie sa
      wygoda, tylko sposobem, zeby w polowie zapisu kliknac cos innego.
      Wiec 'instaluj' bierze CALY ekran.

      **Tryb kiosku** to ten sam kompozytor z odjeta cala reszta: bez
      gornego paska, bez paska zadan, bez ikon, bez menu, bez pulpitow
      i bez drugiego okna. Jeden program, na srodku pustego ekranu, i
      jedyna rzecz, ktora oferuje samo srodowisko - restart maszyny w
      prawym gornym rogu. Okno nie ma nawet przyciskow zamkniecia,
      minimalizacji i maksymalizacji: nie ma dokad go zminimalizowac,
      nie ma w co je powiekszyc i nie ma nic za nim, wiec byly to trzy
      klamstwa.

      Kiosk uruchamia program ponownie, gdy ten sie skonczy - kiosk z
      pustym ekranem wyglada na zepsuta maszyne. Ale jesli program
      pada od razu, po trzech probach kiosk przestaje i pisze o tym;
      migotanie w nieskonczonosc bylo latwiejsze do napisania i gorsze
      do ogladania.

      **Instalatora nie ma juz w menu pulpitu** i to jest cala roznica
      miedzy tym, co bylo, a tym, co jest.

      **Program nie wie nic o instalowaniu.** Pyta jadro, jak wyglada
      dysk (sys_install_look - czyta i nic nie zmienia, wiec wolno to
      kazdemu), i prosi jadro o jeden krok naraz (sys_install_step -
      kazdy krok wymaga admina, bo po drugiej stronie jest dysk maszyny,
      ktora uruchamia sie bez nas). Decydowanie, co znaczy instalacja,
      jest w jednym miejscu i nie jest nim okno.

      Krok na obrot petli, a nie pieciu naraz: jadro zapisaloby wszystko
      w jednym wywolaniu i okno zamarloby na czas zapisu. Tak pasek
      idzie, a pod nim stoi nazwa pliku, ktory wlasnie leci na dysk - a
      czlowiek patrzacy, jak zapisuje sie jego dysk, ma prawo widziec,
      gdzie to jest.

      **Slowo, nie przycisk.** Przycisk to odruch, a odruchami zapisuje
      sie cudze dyski. Okno pyta o wpisane TAK, tak samo jak wersja
      konsolowa. Potem jadro odmawia programowi, ktory nie jest adminem,
      i wtedy - dopiero wtedy - okno pyta o haslo; ten sam ksztalt, co w
      gsettings, i z tego samego powodu: odmowa to nie to samo co blad.

      **Kazda linia lezy na siatce dwunastu pikseli.** Nie dla porzadku:
      czcionka ma osiem pikseli wysokosci, a tekst rozstawiony na oko -
      jedna linia na dwudziestu, druga na piecdziesieciu - to tekst,
      ktory umie przeczytac tylko czlowiek. Siatka to tekst, ktory umie
      przeczytac takze program. Kolumna z rozmiarami stoi na x=6+36*8 z
      tego samego powodu, a nie na 296, bo 296 nie dzieli sie przez
      osiem.

      Test czyta ekran ta sama czcionka, ktora jadro go narysowalo
      (nowe `Qemu.read_text` w ptest.py - screen_fbcon jest teraz jego
      szczegolnym przypadkiem, bo konsola nie jest jedyna rzecza pisana
      ta czcionka). Sprawdza obie polowy rzeczy: ze na tym ekranie NIE
      MA paska zadan, gornego paska ani ikon i ze klik w rog, gdzie na
      pulpicie jest 'Menu', niczego nie otwiera - oraz ze wszystko, co
      program mowi, da sie z ekranu przeczytac. Na koniec, jak w grupie
      `instaluj`, pliki sa czytane z obrazu przez mtools co do bajtu, a
      zainstalowany dysk jest uruchamiany sam.

      Dwie drobne rzeczy, ktore test odslonil: kursor myszy narysowany
      na literze robi z niej znak, ktorego nie ma w czcionce (test
      odsuwa wskaznik, tak jak zrobilby czlowiek), a pasek tytulu okna
      zaczyna sie piksel od ramki - wiec tresc jest pod win->y +
      TITLE_H + 1, a nie tam, gdzie zgadywalem.

      Wersja konsolowa zostaje jako 'instaluj tekstem'. Nie z
      sentymentu: kiedy nie ma na czym rysowac, albo kiedy odpowiedz ma
      trafic do dziennika, jest to jedyna wersja, ktora dziala.

- [x] **Kontrast: ciemny tekst na jasnym, jasny na ciemnym** - w calym
      GUI, nie tylko w instalatorze.

      Okna tego systemu maja jasnoszare tlo (227,227,227), a wszystkie
      programy pisaly na nim BIALYM. Wygladalo to jak dzialajacy
      program - wszystko bylo narysowane - i nie dalo sie tego czytac.
      To najgorszy rodzaj bledu w interfejsie: nic nie jest zepsute,
      tylko nikt nic nie widzi.

      Zasada jest jedna i jest calym rozwiazaniem: **na jasnym tle
      ciemny tekst, na ciemnym jasny**. Wiec czarny na szarym oknie,
      bialy na akcentowym niebieskim (przyciski, zaznaczony wiersz,
      pasek tytulu), bialy na czarnych polach do wpisywania, bialy na
      ciemnym tle pulpitu. Ciemny szary (73,73,73) dla tego, co
      powiedziane ciszej - rozmiar pliku obok jego nazwy - i niebieski
      dla naglowkow, bo to ten sam akcent, ktorego uzywa pulpit.

      Zielony (0,255,0) zniknal z jasnych powierzchni, bo na szarym 227
      jest gorszy niz bialy; tam, gdzie sluzyl do odroznienia (katalog
      od pliku, watek jadra od programu), zastapil go akcent albo ciemny
      szary. Zolte odnosniki w gweb sa teraz niebieskie - odnosniki sa
      niebieskie, odkad istnieja odnosniki, a zolty na jasnym tle to
      nie jest kolor, to jest brak koloru.

      Poprawione: ginstal, gtasks, gfiles, gweb, guiapp, gclock, gmon,
      gsettings (dwa miejsca - reszta juz to wiedziala, jest tam nawet
      komentarz "Black on grey reads; white on grey does not") i
      licznik przy przycisku 'Klik' w samym kompozytorze. gedit, gterm
      i gimg byly poprawne od poczatku: gedit ma biala strone i czarny
      tekst, gterm czarne tlo i bialy, gimg pasek stanu w akcencie.

      Kilka linii, ktore mialy byc kreskami oddzielajacymi, tez bylo
      bialych na szarym - czyli niewidocznych. Sa teraz w kolorze linii
      (150,150,150), tym samym, ktorego uzywa kompozytor.

- [x] **Caly dysk: wlasna tablica partycji i wlasny FAT32**
      (kernel/gpt.c, fat_mkfs w kernel/fat.c, grupa testow `caly`).

      Dotad instalacja znaczyla: trzy pliki na cudzej partycji EFI.
      Teraz jest druga droga - dysk zrobiony od nowa - i roznica miedzy
      nimi jest calym sensem tego kroku. Obok: tablica zostaje, cudze
      partycje zostaja, kazdy plik na partycji EFI zostaje. Caly dysk:
      nowa tablica, nowa partycja EFI, partycja dla ProtonOS-a - i
      wszystko, co tam bylo, znika.

      Trzeciej drogi nie ma i nie bedzie takiej, ktora zmniejsza cudza
      partycje, zeby zrobic miejsce. Przesuwanie danych innego systemu
      na podstawie domyslu, czym one sa, to nie jest rzecz, ktora sie
      robi.

      **Tablica GPT jest napisana pod to, co sprawdza firmware, a nie
      pod to, co wystarczy nam.** Ochronny MBR w sektorze zerowym,
      naglowek w pierwszym, 128 wpisow za nim - i te same wpisy oraz
      drugi naglowek na samym koncu dysku, od tylu. Ostatnie 34 sektory
      dysku GPT to nie jest wolne miejsce, tylko kopia, dzieki ktorej
      urwany poczatek da sie naprawic. Oba naglowki nosza CRC siebie i
      CRC calej tablicy wpisow. Pomylka w tych sumach nie objawia sie
      glosno: firmware po cichu uznaje tablice za uszkodzona i maszyna
      nie startuje, nie mowiac dlaczego. Dlatego test czyta tablice po
      swojemu, w Pythonie, tak jak czyta ja firmware - z obiema sumami
      i porownaniem kopii z oryginalem.

      Wpisy ida na dysk PRZED naglowkami, ktore na nie wskazuja, a
      ochronny MBR na samym koncu: dopoki go nie ma, sektor zerowy
      dalej mowi "pusto" kazdemu, kto go czyta. To jest kolejnosc na
      wypadek, gdyby prad zniknal w polowie.

      **GUID naszej partycji jest nasz.** Bajty ukladaja sie w
      PROTONOS-PFS3, wiec czlowiek ogladajacy tablice edytorem
      szesnastkowym widzi, czyja ona jest, a zaden inny system nie
      wezmie jej za swoja. Pozyczanie GUID-a Linuksa byloby klamstwem,
      ktore kiedys ktos by odczytal.

      **fat_mkfs robi FAT32, i FAT32 nie jest flaga, ktora sie
      ustawia.** To jest liczba klastrow powyzej 65524 - dokladnie ta
      sama regula, ktorej uzywa montowanie. Wiec rozmiar klastra jest
      dobierany tak, zeby liczba wypadla tam, gdzie trzeba, a partycja
      za mala na to jest odrzucana, a nie formatowana na cos innego i
      nazywana FAT32. Partycja EFI ma 128 MiB: znacznie wiecej, niz
      trzy pliki potrzebuja, i najmniej, przy czym FAT32 jest wygodnie
      FAT32.

      Przy okazji: **"32" w FAT32 nie ma nic wspolnego z 32-bitowym
      procesorem.** To szerokosc numeru klastra we wpisie tablicy
      alokacji - FAT12 ma wpisy dwunastobitowe, FAT16 szesnastobitowe,
      FAT32 trzydziestodwubitowe (uzywane 28). Decyduje to o tym, jak
      duzy moze byc wolumin, a nie o tym, na czym sie go czyta.
      Partycja EFI 64-bitowego laptopa z Windowsem tez jest FAT32;
      FAT64 nie istnieje.

      **Slowo jest inne.** Przy instalacji obok pyta o TAK. Tutaj o
      KASUJ - bo TAK mowi sie na pytanie, a na to mowi sie co innego.

      Test sprawdza cala droge: stary dysk z wlasna, poprawna tablica i
      cudzym bootmgfw.efi; plan, ktory mowi wprost, co zniknie; slowo
      inne niz TAK; brak zmian, gdy sie go nie wpisze; po wpisaniu -
      tablica czytana w Pythonie z obiema sumami, dwie partycje o
      wlasciwych GUID-ach, nazwach i granicach, FAT32 czytany przez
      mtools, trzy pliki co do bajtu i na koniec URUCHOMIENIE TEGO
      DYSKU. Osobno to, czego ten tryb NIE robi: nie zeruje dysku
      bajt po bajcie, bo nie robi tego zaden instalator - znika ksztalt
      (tablica i systemy plikow), a nie kazdy bajt, i test mowi to
      wprost zamiast twierdzic cos, co nie byloby prawda.

- [x] **Kilka dyskow i wybor miedzy nimi** (kernel/blk.c jako lista,
      ata.c master i slave, ahci.c wszystkie porty, grupa testow
      `dyski`).

      Warstwa blokowa znajdowala jeden dysk i to byl dysk. Dla maszyny
      z jednym jest to sluszne i przestaje byc sluszne w chwili, gdy
      ktos ma wybrac, na ktory zainstalowac.

      Wiec jest lista. Kolejnosc dalej idzie od nowszego do starszego -
      NVMe, potem kazdy port AHCI z dyskiem, potem oba napedy starej
      magistrali ATA - a pierwszy z listy jest tym uzywanym, dzieki
      czemu maszyna z jednym dyskiem zachowuje sie dokladnie tak, jak
      zachowywala sie wczesniej. To nie jest ostroznosc na wyrost: pod
      ta warstwa lezy caly system plikow, wymiana i tablica partycji.

      Ponad warstwa nic sie nie zmienilo: system plikow, part.c i fat.c
      czytaja i pisza DYSK UZYWANY. Przewlekanie numeru urzadzenia
      przez kazdego z nich byloby zmiana w kazdym z nich - dla pytania,
      ktore zadaje sie raz, czlowiekowi, w instalatorze.

      **OGLADANIE NIE MOZE NICZEGO ZMIENIAC** i to jest jedyna
      subtelna rzecz w tym kroku. Przeczytanie tablicy innego dysku
      wymaga wskazania go warstwie blokowej, bo part.c czyta dysk
      uzywany. Ale maszyna zostawiona wskazujaca na dysk, o ktory ktos
      tylko zapytal, to system plikow czytajacy cudze sektory. Wiec
      install_look() zapamietuje, co bylo uzywane, i wraca tam przed
      powrotem - razem z odmontowaniem FAT-u, ktory po drodze
      podmontowal. Wskazanie wybranego dysku jest rzecza install_step(),
      czyli chwili, w ktorej naprawde sie pisze.

      **Test znalazl przy okazji prawdziwy blad, i to stary.** Komenda
      'disk' pisala wzorzec do OSTATNIEGO SEKTORA dysku i czytala go z
      powrotem - co bylo bezpieczne, dopoki nasze dyski nie mialy na
      sobie nic. Na dysku z GPT ostatni sektor JEST kopia tablicy
      partycji. Zapis tam niszczy ja bez slowa: dysk dalej startuje, a
      pewnego dnia narzedzie naprawcze mowi, ze tablica jest
      uszkodzona. Teraz dysk z tablica jest tylko czytany, i jest
      asercja, ktora to trzyma.

      Wersja tekstowa przyjmuje numer dysku ('instaluj tekstem caly 2')
      i wypisuje liste, gdy dyskow jest wiecej niz jeden. W oknie linia
      z dyskiem jest klikalna i mowi o tym wprost - wybor, ktorego
      trzeba sie domyslic, nie jest wyborem.

- [x] **Domkniecie drogi do sprzetu: `make stick`, `sprzet` i `dyski`
      w jadrze 64-bitowym** (scripts/mkstick.py, kernel/hwinfo.c, grupa
      testow `stick`).

      Caly ten lancuch - loader, FAT, instalator, partycje, wybor dysku
      - istnieje po to, zeby polozyc ProtonOS-a na prawdziwym laptopie.
      I ani jeden bajt z tego nie dzialal na prawdziwym sprzecie:
      wszystko bylo sprawdzane pod QEMU i OVMF. Miedzy "dziala w
      emulatorze" a "startuje z pendrive'a" brakowalo trzech rzeczy i
      wszystkie trzy byly banalne, co jest najgorszym rodzajem
      brakujacych rzeczy.

      **Nie bylo polecenia, ktore robi pendrive'a.** `make iso64` robi
      plyte z GRUB-em, czyli dokladnie to, czego nasz loader nie
      uzywa. Zeby przygotowac stick, trzeba bylo recznie zlozyc GPT,
      sformatowac ESP i skopiowac trzy pliki - czyli powtorzyc z
      pamieci to, co system umie sam. Teraz `make stick` daje surowy
      obraz do dd, a `make run-stick` uruchamia go pod OVMF, zanim
      pojdzie na pendrive.

      Tablica w obrazie jest pisana bajt po bajcie w Pythonie, z tego
      samego powodu, dla ktorego istnieje kernel/gpt.c: firmware
      sprawdza sumy i kopie na koncu dysku i po cichu uznaje tablice
      za uszkodzona. FAT robi mformat - czyli ktos inny. To, ze stick
      jest robiony przez jedna implementacje, a czytany przez druga,
      jest warte wiecej niz to, ze obie byly nasze.

      Obraz ma 192 MiB i jedna partycje. Trzy pliki zajmuja poltora
      megabajta; reszta jest po to, zeby FAT32 byl FAT32 (liczba
      klastrow) i zeby bylo gdzie cos dopisac.

      **`sprzet` nie istnial w jadrze 64-bitowym.** Napisalem te
      komende wprost pod pierwszy rozruch na sprzecie - co jest na
      PS/2, co na USB, co widzi PCI, jaki ekran dal loader - i siedziala
      w powloce 32-bitowej, czyli w jadrze, ktorego na laptopie nikt nie
      uruchomi. To jest ta komenda, ktora wpisuje sie jako pierwsza na
      maszynie, ktorej nikt jeszcze nie probowal, i nie bylo jej tam,
      gdzie jest potrzebna.

      Wyladowala w kernel/hwinfo.c, wspolnym dla obu powlok, razem z
      nowa komenda **`dyski`**: co maszyna ma za dyski, ktorego uzywa i
      co jest na kazdym z nich. Bez wchodzenia w instalator. Ogladanie
      cudzego dysku wskazuje go warstwie blokowej, wiec i tu jest
      wskazywany z powrotem - ta sama zasada, co w install_look().

      Test `stick` czyta obraz tak, jak przeczyta go firmware: tablica
      w Pythonie z obiema sumami, FAT przez mtools, trzy pliki co do
      bajtu - a potem go URUCHAMIA i wpisuje obie nowe komendy.

- [x] **Dzwiek: Intel HD Audio** (kernel/hda.c, komenda `beep`, grupa
      testow `dzwiek`).

      Jedyna klasa urzadzen, ktorej w tym systemie nie bylo. Maszyna ze
      stosem TCP, TLS 1.3 i przegladarka, ktora nie umie wydac zadnego
      dzwieku, jest dziwnie niekompletna.

      **HDA nie jest urzadzeniem, do ktorego sie pisze probki**, i to
      jest cala trudnosc. W maszynie sa DWIE rzeczy. Kontroler siedzi
      na PCI i sam wyciaga bajty z pamieci - dostaje liste buforow
      (BDL) i chodzi po niej w kolko; od tej chwili to on czyta, a
      sterownik ma tylko nadazac z wypelnianiem tej polowy, w ktorej go
      akurat nie ma. Kodek siedzi za kontrolerem na szeregowym laczu i
      jest GRAFEM: ponumerowane widzety - przetworniki, gniazda,
      miksery - po ktorych trzeba CHODZIC, zeby dowiedziec sie, ktory
      przetwornik dochodzi do ktorego gniazda. Dwie maszyny z tym samym
      kontrolerem moga miec zupelnie inne grafy.

      **Trzy bledy po drodze, wszystkie takie, ze urzadzenie po prostu
      milczy.**

      Pierwszy i najdrozszy: **RINTCNT ustawione na jeden**. To licznik
      odpowiedzi, po ktorym kontroler uznaje, ze nie nadazamy - i
      PRZESTAJE wykonywac polecenia, dopoki nie skasuje sie flagi,
      ktora przy tym podniosl. Przy jedynce odpowiedz dostaje dokladnie
      jedno pytanie, a kazde nastepne czeka na przerwanie, ktorego nie
      bierzemy. Objaw byl idealnie mylacy: pierwsze pytanie (o
      producenta kodeka) dawalo prawidlowa odpowiedz, wiec pierscienie
      "dzialaly", a drugie nie dawalo nic. Widac to bylo dopiero po
      wypisaniu CORBRP: wskaznik odczytu stal na jedynce, podczas gdy
      wskaznik zapisu szedl dalej. Licznik jest teraz odsuniety, a
      status kasowany po kazdej odpowiedzi.

      Drugi: **werby maja dwie dlugosci i dlugosc decyduje o miejscu**.
      Czterobitowy zostawia szesnascie bitow na argument, dwunastobitowy
      osiem. Zapisanie krotkiego juz przesunietego - 0x200 zamiast 0x2 -
      wyglada niewinnie i wysyla polecenie, na ktore kodek nie
      odpowiada niczym. Sa teraz zapisane jako liczby, ktorymi sa, a
      klasyfikuje je jeden warunek.

      Trzeci wyszedl dopiero z pliku dzwiekowego: **ton trwal 243 ms
      zamiast 200**. Kontroler chodzi po buforze w kolko, wiec polowa,
      ktorej sie nie nadpisze, zostaje zagrana drugi raz. Po skonczeniu
      sie probek trzeba dalej wypelniac - cisza - przez caly bufor, i
      dopiero potem zatrzymac strumien. To jest roznica miedzy tonem o
      zadanej dlugosci a tonem z zajakniem na koncu.

      **Test nie pyta systemu, czy zagral.** Sterownik, ktory ustawi
      wszystkie rejestry i wyprodukuje cisze, wyglada dokladnie tak samo
      jak dzialajacy. Wiec QEMU zapisuje swoje wyjscie dzwiekowe do
      pliku, a plik jest czytany tutaj: szczyt, dlugosc glosnego
      fragmentu i odleglosc miedzy przejsciami przez zero. 440 Hz przez
      200 ms musi wyjsc jako 440 Hz przez 200 ms, a 880 przez 100 jako
      to - jedna liczba bylaby zbiegiem okolicznosci, dwie sa
      sterownikiem. Wychodzi 441 Hz / 200 ms i 882 Hz / 100 ms.
      Czestotliwosc liczona jest jako MEDIANA odstepow miedzy
      przejsciami, a nie ich liczba na calosci: jedno przejscie
      przeoczone na krawedzi przesuwa liczbe i nie przesuwa mediany.

      Osobno maszyna BEZ karty dzwiekowej - musi powiedziec wprost,
      czego nie ma, a 'beep' na niej ma odmowic, zamiast udawac. Stad
      tez sterownik pamieta, GDZIE sie zatrzymal: "nie ma kontrolera
      HDA na PCI" i "kodek nie ma wyjscia, ktore umiem znalezc" to dwie
      rozne rzeczy i na prawdziwym sprzecie robi to cala roznice.
