/* ThreadOS 64 programs that use the shared library
 *
 * The same starting address as user64/user64.ld, and the same page
 * alignment of the sections - the kernel grants rights from what the
 * ELF says, and the smallest unit of rights is a page.
 *
 * The difference is in what else has to go into the program. A dynamic
 * program does not carry the library's code; it carries the jump table
 * (.plt), the address table (.got.plt) and the description of what goes
 * there. The jump table is CODE, so it lies next to .text; the address
 * table is writable DATA, so it lies next to .data - otherwise either
 * the processor would refuse the jump, or it would let something
 * executable be written into.
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
