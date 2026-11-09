/* The ARM64 kernel's layout on QEMU's virt machine.
 *
 * RAM starts at 0x40000000, and for a kernel that is not Linux QEMU
 * puts the device tree right there, at the very start. The kernel goes
 * two megabytes higher so the two never overlap - the tree QEMU makes
 * is a megabyte at most.
 */

ENTRY(_start)

SECTIONS
{
    . = 0x40200000;

    .text : {
        *(.text.boot)
        *(.text .text.*)
    }
    .rodata : ALIGN(8) {
        *(.rodata .rodata.*)
    }
    .data : ALIGN(8) {
        *(.data .data.*)
    }
    .bss : ALIGN(16) {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(8);
        __bss_end = .;
    }
    __kernel_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.note .note.*)
    }
}
