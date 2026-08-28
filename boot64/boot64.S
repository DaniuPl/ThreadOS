/* ProtonOS 64 - wejscie w tryb dlugi
 *
 * GRUB zostawia nas w trybie chronionym 32-bit, bo tak stanowi
 * Multiboot 2. Przejscie w tryb dlugi wymaga kilku krokow w scisle
 * okreslonej kolejnosci:
 *
 *   1. tablice stron - w trybie dlugim stronicowanie jest OBOWIAZKOWE,
 *      wiec musi byc gotowe, zanim go wlaczymy,
 *   2. PAE w CR4 - bez niego wpisy sa 32-bitowe i tryb dlugi nie ruszy,
 *   3. bit LME w rejestrze EFER - zgoda na tryb dlugi,
 *   4. PG w CR0 - stronicowanie; procesor wchodzi wtedy w tryb
 *      zgodnosci, nie jeszcze w pelny tryb dlugi,
 *   5. skok daleki do segmentu z bitem L - dopiero on przelacza
 *      procesor naprawde.
 *
 * Mapujemy identycznosciowo pierwsze cztery gigabajty stronami po
 * 2 MiB. Nie chodzi o pamiec - tyle jej nie ma - tylko o to, ze wysoko
 * pod czterema gigabajtami leza rejestry urzadzen i bufor obrazu.
 * Cztery tablice po 512 wpisow opisuja to w komplecie i mieszcza sie
 * w obrazie jadra, bez zadnego przydzielania pamieci, ktorego na tym
 * etapie jeszcze nie umiemy.
 */

.set MB2_MAGIC, 0xE85250D6
.set MB2_ARCH,  0

.section .multiboot, "a", @progbits
.align 8
mb2_header:
.long MB2_MAGIC
.long MB2_ARCH
.long mb2_header_end - mb2_header
.long -(MB2_MAGIC + MB2_ARCH + (mb2_header_end - mb2_header))

    .align 8
    .short 1                         /* zadanie informacji */
    .short 0
    .long 8 + 5 * 4
    .long 1                          /* linia polecen */
    .long 3                          /* moduly */
    .long 6                          /* mapa pamieci */
    .long 14                         /* RSDP (ACPI 1.0) */
    .long 15                         /* RSDP (ACPI 2.0) */

    .align 8
    .short 6                         /* moduly wyrownane do stron */
    .short 0
    .long 8

    /* Tryb obrazu: zero znaczy "bez preferencji". Zadanie konkretnej
       rozdzielczosci ma sens pod BIOS-em, gdzie karta zna cala liste
       trybow VESA. Pod UEFI wybor jest taki, jaki firmware akurat
       wystawia przez GOP - i gdy zazadamy czegos spoza tej listy,
       loader nie ustawia NICZEGO. A pod UEFI nie ma trybu tekstowego,
       do ktorego moglibysmy wtedy wrocic: system zostalby bez obrazu. */
    .align 8
    .short 5                         /* tryb obrazu */
    .short 0
    .long 20
    .long 0                          /* szerokosc - dowolna */
    .long 0                          /* wysokosc - dowolna */
    .long 0                          /* glebia - dowolna */

    .align 8
    .short 0
    .short 0
    .long 8
mb2_header_end:

/* Tablice stron: po jednej ramce na poziom. Wyrownanie do 4 KiB jest
   wymagane - procesor bierze z wpisu tylko starsze bity adresu. */
.section .bss
.align 4096
.global boot_pml4
boot_pml4:  .skip 4096
boot_pdpt:  .skip 4096
boot_pd:    .skip 4 * 4096          /* cztery tablice = cztery gigabajty */

.align 16
stack_bottom:
.skip 16384
.global stack_top
stack_top:

.section .rodata
.align 8
/* Tablica deskryptorow trybu dlugiego. W trybie dlugim baza i limit
   nie znacza juz nic - liczy sie bit L (64-bitowy kod) i poziom
   uprawnien. Deskryptor danych jest tylko po to, zeby bylo co
   zaladowac do rejestrow segmentowych. */
gdt64:
    .quad 0                                  /* wpis zerowy */
gdt64_code:
    .quad (1<<43) | (1<<44) | (1<<47) | (1<<53)   /* kod, obecny, 64-bit */
gdt64_data:
    .quad (1<<44) | (1<<47) | (1<<41)             /* dane, obecne, zapis */
gdt64_end:
gdt64_ptr:
    .short gdt64_end - gdt64 - 1
    .long gdt64

.section .text
.code32
.global _start
.type _start, @function
_start:
    movl $stack_top, %esp
    movl %eax, %edi                  /* magic - do przekazania dalej */
    movl %ebx, %esi                  /* wskaznik na informacje od loadera */

    /* --- 1. Tablice stron: pierwszy gigabajt stronami po 2 MiB --- */
    movl $boot_pdpt, %eax
    orl  $0x03, %eax                 /* obecna + zapis */
    movl %eax, boot_pml4

    /* Cztery wpisy katalogu nadrzednego - po jednym na gigabajt */
    movl $boot_pd, %eax
    orl  $0x03, %eax
    movl $0, %ecx
2:
    movl %eax, boot_pdpt(, %ecx, 8)
    movl $0, boot_pdpt + 4(, %ecx, 8)
    addl $4096, %eax
    incl %ecx
    cmpl $4, %ecx
    jne 2b

    /* 2048 stron po 2 MiB: adres kazdej to jej numer przesuniety o 21
       bitow - najwyzszy wypada na 0xFFE00000, wiec starsza polowa
       wpisu zostaje zerem */
    movl $0, %ecx
1:
    movl %ecx, %eax
    shll $21, %eax
    orl  $0x83, %eax                 /* obecna + zapis + STRONA WIELKA */
    movl %eax, boot_pd(, %ecx, 8)
    movl $0, boot_pd + 4(, %ecx, 8)
    incl %ecx
    cmpl $2048, %ecx
    jne 1b

    movl $boot_pml4, %eax
    movl %eax, %cr3

    /* --- 2. PAE --- */
    movl %cr4, %eax
    orl  $(1 << 5), %eax
    movl %eax, %cr4

    /* --- 3. Bit LME w EFER (rejestr modelu 0xC0000080) --- */
    movl $0xC0000080, %ecx
    rdmsr
    orl  $(1 << 8), %eax
    wrmsr

    /* --- 4. Stronicowanie: procesor wchodzi w tryb zgodnosci --- */
    /* Razem ze stronicowaniem (bit 31) wlaczamy ochrone zapisu
       (bit 16). Bez niej jadro moze pisac po stronach oznaczonych jako
       tylko-do-odczytu, a to zabija kopiowanie przy zapisie: strona
       wspoldzielona przez dwa procesy zostalaby po cichu nadpisana
       zamiast rozdzielona. */
    movl %cr0, %eax
    orl  $((1 << 31) | (1 << 16)), %eax
    movl %eax, %cr0

    /* --- 5. Dopiero skok daleki przelacza go naprawde --- */
    lgdt gdt64_ptr
    ljmp $0x08, $long_start

.code64
long_start:
    /* W trybie dlugim rejestry segmentowe sa niemal bez znaczenia,
       ale muszą wskazywać na poprawny deskryptor */
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw %ax, %fs
    movw %ax, %gs

    movq $stack_top, %rsp
    /* magic i wskaznik czekaja juz w edi/esi - gorne polowy zerujemy,
       bo w trybie 32-bitowym nie istnialy */
    movl %edi, %edi
    movl %esi, %esi
    call kmain64

    cli
2:  hlt
    jmp 2b

.size _start, . - _start
.section .note.GNU-stack, "", @progbits
