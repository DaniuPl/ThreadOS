/* Skrypt linkera ProtonOS - jądro ładowane pod 1 MiB (konwencja Multiboot) */

ENTRY(_start)

SECTIONS
{
    . = 1M;

    _kernel_start = .;

    /* Nagłówek Multiboot musi być w pierwszych 8 KiB pliku - dlatego
       ma własną sekcję na samym początku obrazu */
    .multiboot : ALIGN(4)
    {
        KEEP(*(.multiboot))
    }

    .text : ALIGN(4K)
    {
        *(.text)
    }

    /* Program użytkownika - osobne strony, bo dostają bit USER */
    .user : ALIGN(4K)
    {
        _user_start = .;
        *(.user_text)
        *(.user_data)
        . = ALIGN(4K);
        _user_end = .;
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
