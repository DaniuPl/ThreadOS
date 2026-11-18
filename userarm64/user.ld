/* Programs for the ARM64 kernel (kernelarm64/proc.h): linked at
 * USER_BASE, 256 GiB, where no mapping of the kernel's is. Each kind of
 * memory starts a page of its own - code, read-only data, data - because
 * a page is mapped with one set of rights: never one that is both
 * written and run.
 */
ENTRY(_start)

SECTIONS
{
    . = 0x4000000000;
    .text : { *(.text._start) *(.text .text.*) }
    . = ALIGN(4096);
    .rodata : { *(.rodata .rodata.*) }
    . = ALIGN(4096);
    .data : { *(.data .data.*) }
    .bss : { *(.bss .bss.* COMMON) }
    /DISCARD/ : { *(.comment) *(.note .note.*) *(.eh_frame*) }
}
