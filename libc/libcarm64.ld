/* Programs for the ARM64 kernel built against musl (libc/)
 *
 * libc/libc64.ld, at the address where the ARM64 kernel runs programs
 * (kernelarm64/proc.h, userarm64/user.ld): 256 GiB, the bottom of the
 * top half of what TTBR0 covers. Everything else is the PC's script,
 * whose words follow.
 *
 * The same address and the same page-aligned split as user64/user64.ld
 * - code, read-only data, writable data, each with rights of its own -
 * plus what a C library's start-up walks through: .init and .fini (the
 * prologue and epilogue come from crti.o and crtn.o), and the arrays
 * of constructors and destructors, whose bounds musl finds by these
 * symbols.
 *
 * Thread-local storage: musl's start-up looks for its template in the
 * program headers, which ThreadOS does not pass on. The symbols at the
 * end describe it instead, and the start-up code (crt/crt1.c) turns
 * them into the one program header musl looks at.
 */

ENTRY(_start)

SECTIONS
{
    . = 0x4000000000;

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

/* Addresses and sizes as absolute symbols: the C code reads their
   addresses. The template is .tdata; .tbss after it is zeros. */
PROVIDE_HIDDEN(__tls_image  = ADDR(.tdata));
PROVIDE_HIDDEN(__tls_filesz = SIZEOF(.tdata));
PROVIDE_HIDDEN(__tls_memsz  = SIZEOF(.tbss) == 0 ? SIZEOF(.tdata)
                              : ADDR(.tbss) + SIZEOF(.tbss) - ADDR(.tdata));
PROVIDE_HIDDEN(__tls_align  = MAX(ALIGNOF(.tdata), ALIGNOF(.tbss)));
