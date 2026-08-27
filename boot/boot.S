/* ProtonOS - punkt wejścia jądra (Multiboot 1)
 *
 * GRUB ładuje jądro i skacze do _start w trybie chronionym 32-bit.
 * Tutaj ustawiamy stos i wołamy kmain() z kodu C.
 */

/* Stałe nagłówka Multiboot */
.set MB_ALIGN,    1 << 0             /* wyrównaj załadowane moduły do granic stron */
.set MB_MEMINFO,  1 << 1             /* przekaż mapę pamięci */
.set MB_FLAGS,    MB_ALIGN | MB_MEMINFO
.set MB_MAGIC,    0x1BADB002
.set MB_CHECKSUM, -(MB_MAGIC + MB_FLAGS)

/* Nagłówek Multiboot - musi być w pierwszych 8 KiB obrazu */
.section .multiboot, "a", @progbits
.align 4
.long MB_MAGIC
.long MB_FLAGS
.long MB_CHECKSUM

/* Nagłówek Multiboot 2
 *
 * Obraz nosi oba nagłówki naraz i to jest celowe: QEMU uruchamiane
 * z opcją -kernel rozumie tylko starszy, a GRUB woli nowszy. Loader
 * wybiera ten, który zna, i przekazuje inny magic w eax - jądro
 * rozpoznaje po nim, w której postaci dostało informacje o maszynie.
 *
 * Nowszy jest drogą do UEFI: tam nie ma ani BIOS-u, ani starego
 * Multiboota, a wskaźnik na tablice ACPI podaje właśnie loader.
 */
.set MB2_MAGIC, 0xE85250D6
.set MB2_ARCH,  0                    /* tryb chroniony i386 */

.align 8
mb2_header:
.long MB2_MAGIC
.long MB2_ARCH
.long mb2_header_end - mb2_header
.long -(MB2_MAGIC + MB2_ARCH + (mb2_header_end - mb2_header))

    /* O co prosimy loader: linia poleceń, moduły, mapa pamięci
       i wskaźnik na tablice ACPI (pod UEFI nie ma go gdzie szukać) */
    .align 8
    .short 1                         /* znacznik: żądanie informacji */
    .short 0
    .long 8 + 5 * 4
    .long 1                          /* linia poleceń */
    .long 3                          /* moduły */
    .long 6                          /* mapa pamięci */
    .long 14                         /* RSDP (ACPI 1.0) */
    .long 15                         /* RSDP (ACPI 2.0) */

    /* Moduły wyrównane do granic stron - tak samo jak w Multiboot 1 */
    .align 8
    .short 6
    .short 0
    .long 8

    /* Prośba o tryb graficzny z liniowym buforem obrazu.
       Pod UEFI nie ma trybu tekstowego VGA - bufor znaków pod 0xB8000
       po prostu nie istnieje - więc jedyną drogą do pokazania czegokolwiek
       jest bufor obrazu podany przez loader. Znaki rysujemy wtedy sami. */
    .align 8
    .short 5                         /* znacznik: tryb obrazu */
    .short 0
    .long 20
    .long 1024                       /* szerokość */
    .long 768                        /* wysokość */
    .long 32                         /* bitów na piksel */

    .align 8
    .short 0                         /* znacznik końca */
    .short 0
    .long 8
mb2_header_end:

/* Stos jądra - 16 KiB, wyrównany do 16 bajtów (wymóg ABI System V) */
.section .bss
.align 16
stack_bottom:
.skip 16384
.global stack_top
stack_top:

.section .text
.global _start
.type _start, @function
_start:
    movl $stack_top, %esp

    /* Argumenty dla kmain: magic (eax) i wskaźnik na info Multiboot (ebx) */
    pushl %ebx
    pushl %eax
    call kmain

    /* kmain nie powinno wrócić - jeśli wróci, zatrzymaj CPU */
    cli
1:  hlt
    jmp 1b

.size _start, . - _start

/* Zaznacz, że ten plik nie potrzebuje wykonywalnego stosu */
.section .note.GNU-stack, "", @progbits
