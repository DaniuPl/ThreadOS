/* Programy uzytkownika ProtonOS
 *
 * Przestrzen uzytkownika zaczyna sie od 1 GiB: ponizej jadro trzyma
 * mapowanie identycznosciowe calej pamieci fizycznej (PAGING_USER_BASE
 * w kernel/include/paging.h - obie liczby musza sie zgadzac). */

ENTRY(_start)

SECTIONS
{
    . = 0x40000000;

    .text   : { *(.text*) }
    .rodata : { *(.rodata*) }
    .data   : { *(.data*) }
    .bss    : { *(.bss*) *(COMMON) }

    /DISCARD/ : { *(.note*) *(.comment) *(.eh_frame*) }
}
