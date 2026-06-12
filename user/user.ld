/* Programy uzytkownika ProtonOS - ladowane pod klasyczny adres ELF */

ENTRY(_start)

SECTIONS
{
    . = 0x08048000;

    .text   : { *(.text*) }
    .rodata : { *(.rodata*) }
    .data   : { *(.data*) }
    .bss    : { *(.bss*) *(COMMON) }

    /DISCARD/ : { *(.note*) *(.comment) *(.eh_frame*) }
}
