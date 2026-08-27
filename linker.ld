/* Skrypt linkera ProtonOS - jądro ładowane pod 1 MiB (konwencja Multiboot) */

ENTRY(_start)

SECTIONS
{
    . = 1M;

    _kernel_start = .;

    /* Nagłówek Multiboot musi być w pierwszych 8 KiB pliku - dlatego
       ma własną sekcję na samym początku obrazu */
    /* Naglowek Multiboot 2 wymaga wyrownania do 8 bajtow */
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
