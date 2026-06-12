/* ProtonOS - punkt wejścia jądra (Multiboot 1)
 *
 * GRUB ładuje jądro i skacze do _start w trybie chronionym 32-bit.
 * Tutaj ustawiamy stos i wołamy kmain() z kodu C.
 */

/* Stałe nagłówka Multiboot */
.set MB_ALIGN,    1 << 0             /* wyrównaj załadowane moduły do granic stron */
.set MB_MEMINFO,  1 << 1             /* przekaż mapę pamięci */
.set MB_FLAGS,    MB_ALIGN | MB_MEMINFO
.set MB_MAGIC,    0x1BADB002
.set MB_CHECKSUM, -(MB_MAGIC + MB_FLAGS)

/* Nagłówek Multiboot - musi być w pierwszych 8 KiB obrazu */
.section .multiboot, "a", @progbits
.align 4
.long MB_MAGIC
.long MB_FLAGS
.long MB_CHECKSUM

/* Stos jądra - 16 KiB, wyrównany do 16 bajtów (wymóg ABI System V) */
.section .bss
.align 16
stack_bottom:
.skip 16384
stack_top:

.section .text
.global _start
.type _start, @function
_start:
    movl $stack_top, %esp

    /* Argumenty dla kmain: magic (eax) i wskaźnik na info Multiboot (ebx) */
    pushl %ebx
    pushl %eax
    call kmain

    /* kmain nie powinno wrócić - jeśli wróci, zatrzymaj CPU */
    cli
1:  hlt
    jmp 1b

.size _start, . - _start

/* Zaznacz, że ten plik nie potrzebuje wykonywalnego stosu */
.section .note.GNU-stack, "", @progbits
