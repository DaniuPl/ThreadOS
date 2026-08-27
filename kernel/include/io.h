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

static inline void outw(uint16_t port, uint16_t value)
{
    __asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint16_t inw(uint16_t port)
{
    uint16_t value;
    __asm__ volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outl(uint16_t port, uint32_t value)
{
    __asm__ volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint32_t inl(uint16_t port)
{
    uint32_t value;
    __asm__ volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

/* Krótkie opóźnienie - zapis do nieużywanego portu 0x80.
   Daje staremu sprzętowi (np. PIC) czas na przetworzenie komendy. */
static inline void io_wait(void)
{
    outb(0x80, 0);
}

/* Wyłącza przerwania, zwracając poprzedni stan flag -
   do krytycznych sekcji współdzielonych z handlerami IRQ.

   W trybie długim rejestr flag jest 64-bitowy i instrukcje zdejmujące
   go ze stosu mają inny sufiks; poza tym sekwencja jest ta sama, więc
   różnicę zamyka jeden warunek zamiast osobnej wersji pliku. */
#ifdef __x86_64__
typedef unsigned long irqflags_t;
#define IRQ_PUSHF "pushfq; popq %0; cli"
#define IRQ_POPF  "pushq %0; popfq"
#else
typedef uint32_t irqflags_t;
#define IRQ_PUSHF "pushfl; popl %0; cli"
#define IRQ_POPF  "pushl %0; popfl"
#endif

static inline irqflags_t irq_save(void)
{
    irqflags_t flags;
    __asm__ volatile(IRQ_PUSHF : "=r"(flags) : : "memory");
    return flags;
}

static inline void irq_restore(irqflags_t flags)
{
    __asm__ volatile(IRQ_POPF : : "r"(flags) : "memory", "cc");
}

#endif
