/* Skrypt konsolidatora jadra 64-bitowego. Uklad taki sam jak
   w 32-bitowym: obraz pod 1 MiB, naglowek Multiboot 2 na poczatku.
   GRUB wgrywa obraz ELF64 i zostawia nas w trybie chronionym - w tryb
   dlugi wchodzimy sami (boot64/boot64.S). */

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
