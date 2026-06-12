/* Sterownik portu szeregowego COM1 (UART 16550)
 *
 * Bezcenne do debugowania: QEMU z opcją -serial stdio przekierowuje
 * wszystko, co tu wypiszemy, na terminal hosta.
 */

#include "include/serial.h"
#include "include/io.h"

#define COM1 0x3F8

void serial_init(void)
{
    outb(COM1 + 1, 0x00);   /* wyłącz przerwania UART */
    outb(COM1 + 3, 0x80);   /* DLAB=1: dostęp do dzielnika */
    outb(COM1 + 0, 0x03);   /* dzielnik 3 -> 38400 bodów */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);   /* 8 bitów, brak parzystości, 1 bit stopu */
    outb(COM1 + 2, 0xC7);   /* włącz i wyczyść FIFO */
    outb(COM1 + 4, 0x0B);   /* DTR + RTS + OUT2 */
}

void serial_putchar(char c)
{
    /* Czekaj, aż bufor nadawczy będzie pusty (bit 5 LSR) */
    while (!(inb(COM1 + 5) & 0x20))
        ;
    outb(COM1, (uint8_t)c);
    if (c == '\n')
        serial_putchar('\r');
}

void serial_write(const char *s)
{
    while (*s)
        serial_putchar(*s++);
}
