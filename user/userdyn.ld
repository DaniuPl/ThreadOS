/* ProtonOS user programs that use the shared library
 *
 * The same starting address as user/user.ld - the difference is in what
 * else has to go into the program. A dynamic program does not carry the
 * library's code; it carries the jump table (.plt) and the address
 * table (.got.plt), which the kernel fills in at load time, plus the
 * description of what goes there (.dynsym, .dynstr, .rel.plt).
 */

ENTRY(_start)

SECTIONS
{
    . = 0x40000000;

    .text     : { *(.text*) }
    .plt      : { *(.plt) *(.iplt) }
    .rodata   : { *(.rodata*) }
    .hash     : { *(.hash) }
    .gnu.hash : { *(.gnu.hash) }
    .dynsym   : { *(.dynsym) }
    .dynstr   : { *(.dynstr) }
    .rel.plt  : { *(.rel.plt) *(.rel.dyn) }
    .rela.plt : { *(.rela.plt) *(.rela.dyn) }
    .dynamic  : { *(.dynamic) }

    .data     : { *(.data*) }
    .got      : { *(.got) }
    .got.plt  : { *(.got.plt) }
    .bss      : { *(.bss*) *(COMMON) }

    /DISCARD/ : { *(.note*) *(.comment) *(.eh_frame*) *(.interp) }
}
