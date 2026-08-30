/* Programy ProtonOS 64 korzystajace z biblioteki wspoldzielonej
 *
 * Ten sam adres poczatkowy, co user64/user64.ld, i to samo wyrownanie
 * sekcji do stron - jadro nadaje prawa na podstawie tego, co mowi ELF,
 * a najmniejsza jednostka praw jest strona.
 *
 * Roznica jest w tym, co jeszcze trzeba w programie umiescic. Program
 * dynamiczny nie niesie kodu biblioteki; niesie tablice skokow (.plt),
 * tablice adresow (.got.plt) i opis tego, co ma tam trafic. Tablica
 * skokow to KOD, wiec lezy przy .text; tablica adresow to DANE
 * zapisywalne, wiec lezy przy .data - inaczej albo procesor odmowilby
 * skoku, albo dalby sie w niej zapisac czemus wykonywalnemu.
 */

ENTRY(_start)

SECTIONS
{
    . = 0x40000000;

    .text     : { *(.text*) }
    .plt      : { *(.plt) *(.iplt) }
    . = ALIGN(4096);
    .rodata   : { *(.rodata*) }
    .hash     : { *(.hash) }
    .gnu.hash : { *(.gnu.hash) }
    .dynsym   : { *(.dynsym) }
    .dynstr   : { *(.dynstr) }
    .rela.plt : { *(.rela.plt) *(.rela.dyn) }
    . = ALIGN(4096);
    .dynamic  : { *(.dynamic) }
    .data     : { *(.data*) }
    .got      : { *(.got) }
    .got.plt  : { *(.got.plt) }
    .bss      : { *(.bss*) *(COMMON) }

    /DISCARD/ : { *(.note*) *(.comment) *(.eh_frame*) *(.interp) }
}
