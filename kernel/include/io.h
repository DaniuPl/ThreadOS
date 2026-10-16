#ifndef THREADOS_IO_H
#define THREADOS_IO_H

#include <stdint.h>

/* Access to x86 I/O ports */

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

/* A short delay - a write to the unused port 0x80.
   Gives old hardware (e.g. the PIC) time to digest a command. */
static inline void io_wait(void)
{
    outb(0x80, 0);
}

/* Turns interrupts off and returns the previous flag state -
   for critical sections shared with IRQ handlers.

   In long mode the flags register is 64 bits wide and the instructions
   taking it off the stack have a different suffix; otherwise the
   sequence is the same, so one conditional closes the difference
   instead of a separate version of the file. */
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
