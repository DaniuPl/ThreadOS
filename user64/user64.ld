/* ThreadOS 64 user programs
 *
 * Programs start at 1 GiB, because below it lies the identity mapping
 * of physical memory.
 * USER64_BASE in kernel64/elf64.h has to agree with this.
 *
 * The sections are page-aligned, because the kernel grants access
 * rights from what the ELF says and the smallest unit of rights is a
 * page. So the code gets pages of its own (to read and to execute) and
 * the data gets its own (to write, with execution forbidden).
 */

ENTRY(_start)

SECTIONS
{
    . = 0x40000000;

    .text   : { *(.text*) }
    . = ALIGN(4096);
    .rodata : { *(.rodata*) }
    . = ALIGN(4096);
    .data   : { *(.data*) }
    .bss    : { *(.bss*) *(COMMON) }

    /DISCARD/ : { *(.note*) *(.comment) *(.eh_frame*) }
}
