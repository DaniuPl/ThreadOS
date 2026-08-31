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
