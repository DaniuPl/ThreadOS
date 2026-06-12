#ifndef PROTONOS_IO_H
#define PROTONOS_IO_H

#include <stdint.h>

/* Dostęp do portów I/O x86 */

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

/* Krótkie opóźnienie - zapis do nieużywanego portu 0x80.
   Daje staremu sprzętowi (np. PIC) czas na przetworzenie komendy. */
static inline void io_wait(void)
{
    outb(0x80, 0);
}

#endif
