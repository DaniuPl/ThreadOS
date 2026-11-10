#ifndef THREADOS_IO_H
#define THREADOS_IO_H

#include <stdint.h>

#if defined(__x86_64__)

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
   for critical sections shared with IRQ handlers. In long mode the
   flags register is 64 bits wide. */
typedef unsigned long irqflags_t;
#define IRQ_PUSHF "pushfq; popq %0; cli"
#define IRQ_POPF  "pushq %0; popfq"

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

#elif defined(__aarch64__)

/* ARM has no I/O ports - a device is memory at an address - so of this
   header only the interrupt flag is left. Its place is the I bit of
   DAIF: read the four mask bits whole, set I, and later write back
   exactly what was there. */
typedef unsigned long irqflags_t;

static inline irqflags_t irq_save(void)
{
    irqflags_t flags;
    __asm__ volatile("mrs %0, daif\n\tmsr daifset, #2"
                     : "=r"(flags) : : "memory");
    return flags;
}

static inline void irq_restore(irqflags_t flags)
{
    __asm__ volatile("msr daif, %0" : : "r"(flags) : "memory");
}

#else
#error "io.h: no interrupt flag for this processor"
#endif

#endif
