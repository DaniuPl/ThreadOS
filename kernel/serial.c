/* COM1 serial port driver (UART 16550)
 *
 * Priceless for debugging: QEMU with the -serial stdio option
 * redirects everything we print here to the host's terminal.
 */

#include "include/serial.h"
#include "include/io.h"

#define COM1 0x3F8

void serial_init(void)
{
    outb(COM1 + 1, 0x00);   /* disable UART interrupts */
    outb(COM1 + 3, 0x80);   /* DLAB=1: access to the divisor */
    outb(COM1 + 0, 0x03);   /* divisor 3 -> 38400 baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);   /* 8 bits, no parity, 1 stop bit */
    outb(COM1 + 2, 0xC7);   /* enable and clear the FIFO */
    outb(COM1 + 4, 0x0B);   /* DTR + RTS + OUT2 */
}

void serial_putchar(char c)
{
    /* Wait until the transmit buffer is empty (bit 5 of the LSR) */
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
