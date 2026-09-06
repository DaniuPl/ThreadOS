/* The ProtonOS linker script - the kernel loads at 1 MiB (the Multiboot convention) */

ENTRY(_start)

SECTIONS
{
    . = 1M;

    _kernel_start = .;

    /* The Multiboot header has to be in the first 8 KiB of the file -
       hence a section of its own at the very start of the image */
    /* A Multiboot 2 header has to be aligned to 8 bytes */
    .multiboot : ALIGN(8)
    {
        KEEP(*(.multiboot))
    }

    .text : ALIGN(4K)
    {
        *(.text)
    }

    .rodata : ALIGN(4K)
    {
        *(.rodata)
    }

    .data : ALIGN(4K)
    {
        *(.data)
    }

    .bss : ALIGN(4K)
    {
        *(COMMON)
        *(.bss)
    }

    _kernel_end = .;
}
