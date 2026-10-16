/* The ThreadOS shared library
 *
 * Linked at a FIXED address - the same one the kernel gives it in every
 * process (USER_LIB_BASE in kernel64/elf64.h). That way the addresses in its symbol
 * table are already final, and neither the library nor the kernel has
 * anything to recompute at load time.
 *
 * The writable data gets pages of its own, because it is the only part
 * that cannot be shared: the library keeps its state in there (the
 * generator's seed, the TLS settings), and one process's state has no
 * business being visible in another. The code and the constants are
 * shared - and that is the whole of the saving.
 */

ENTRY(_does_not_start)

SECTIONS
{
    . = 0x4F000000;

    .text   : { *(.text*) }
    .rodata : { *(.rodata*) }
    .hash   : { *(.hash) }
    .gnu.hash : { *(.gnu.hash) }
    .dynsym : { *(.dynsym) }
    .dynstr : { *(.dynstr) }
    .rel.dyn  : { *(.rel.*) }
    .rela.dyn : { *(.rela.*) }

    /* From here on comes everything that can be written to - and
       that is exactly why the page boundary is here. The segment above
       is shared by every process, so it must not hold a single byte
       anybody could change; .dynamic and the address tables are
       writable, so they sit below. */
    . = ALIGN(4096);
    .dynamic : { *(.dynamic) }
    .data   : { *(.data*) *(.got) *(.got.plt) }
    .bss    : { *(.bss*) *(COMMON) }

    /DISCARD/ : { *(.note*) *(.comment) *(.eh_frame*) }
}
