/* ThreadOS 64 programs built against musl (libc/)
 *
 * The same address and the same page-aligned split as user64/user64.ld
 * - code, read-only data, writable data, each with rights of its own -
 * plus what a C library's start-up walks through: .init and .fini (the
 * prologue and epilogue come from crti.o and crtn.o), and the arrays
 * of constructors and destructors, whose bounds musl finds by these
 * symbols.
 *
 * No thread-local storage: musl's start-up would look for it in the
 * program headers, which ThreadOS does not pass on, so a program that
 * has some fails to link rather than quietly sharing it.
 */

ENTRY(_start)

SECTIONS
{
    . = 0x40000000;

    .text   : { *(.text.unlikely .text.unlikely.*) *(.text .text.*) }
    .init   : { KEEP(*(.init)) }
    .fini   : { KEEP(*(.fini)) }
    . = ALIGN(4096);
    .rodata : { *(.rodata .rodata.*) }
    . = ALIGN(4096);
    .preinit_array : {
        PROVIDE_HIDDEN(__preinit_array_start = .);
        KEEP(*(.preinit_array))
        PROVIDE_HIDDEN(__preinit_array_end = .);
    }
    .init_array : {
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP(*(.init_array .ctors))
        PROVIDE_HIDDEN(__init_array_end = .);
    }
    .fini_array : {
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*)))
        KEEP(*(.fini_array .dtors))
        PROVIDE_HIDDEN(__fini_array_end = .);
    }
    .data   : { *(.data .data.*) }
    .tdata  : { *(.tdata .tdata.*) }
    .tbss   : { *(.tbss .tbss.*) }
    .bss    : { *(.bss .bss.*) *(COMMON) }

    /DISCARD/ : { *(.note*) *(.comment) *(.eh_frame*) }
}

ASSERT(SIZEOF(.tdata) + SIZEOF(.tbss) == 0,
       "thread-local variables are not supported (see libc/libc64.ld)")
