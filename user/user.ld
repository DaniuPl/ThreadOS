/* ThreadOS user programs
 *
 * User space starts at 1 GiB: below that the kernel keeps an identity
 * mapping of the whole of physical memory (PAGING_USER_BASE in
 * kernel/include/paging.h - the two numbers have to agree). */

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
