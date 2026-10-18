/* The linker script for the 64-bit kernel. The same layout as the
   32-bit one: the image at 1 MiB, the Multiboot 2 header at the start.
   GRUB loads the ELF64 image and leaves us in protected mode - long
   mode we enter ourselves (boot64/boot64.S). */

ENTRY(_start)

SECTIONS
{
    . = 1M;

    _kernel_start = .;

    .multiboot : ALIGN(8)
    {
        KEEP(*(.multiboot))
    }

    .text : ALIGN(4K) { *(.text) }
    .rodata : ALIGN(4K) { *(.rodata*) }
    .data : ALIGN(4K) { *(.data) }
    .bss : ALIGN(4K)
    {
        *(COMMON)
        *(.bss)
    }

    _kernel_end = .;

    /DISCARD/ : { *(.note*) *(.comment) *(.eh_frame*) }
}
