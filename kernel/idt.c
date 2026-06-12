/* IDT - Interrupt Descriptor Table
 *
 * Tablica 256 bramek mówiących CPU, dokąd skoczyć przy każdym
 * wektorze przerwania. Wypełniamy wyjątki 0-31 i IRQ 32-47.
 */

#include <stdint.h>
#include "include/idt.h"
#include "include/gdt.h"
#include "include/string.h"

struct idt_entry {
    uint16_t base_low;
    uint16_t selector;      /* selektor segmentu kodu w GDT */
    uint8_t  zero;
    uint8_t  flags;         /* P | DPL | typ bramki */
    uint16_t base_high;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

static struct idt_entry idt[256];
static struct idt_ptr idt_pointer;

/* Stuby z kernel/isr.S */
#define DECLARE_STUB(n) extern void isr##n(void);
#define STUB_LIST(X) \
    X(0)  X(1)  X(2)  X(3)  X(4)  X(5)  X(6)  X(7) \
    X(8)  X(9)  X(10) X(11) X(12) X(13) X(14) X(15) \
    X(16) X(17) X(18) X(19) X(20) X(21) X(22) X(23) \
    X(24) X(25) X(26) X(27) X(28) X(29) X(30) X(31) \
    X(32) X(33) X(34) X(35) X(36) X(37) X(38) X(39) \
    X(40) X(41) X(42) X(43) X(44) X(45) X(46) X(47)
STUB_LIST(DECLARE_STUB)
extern void isr128(void);

static void idt_set_gate(int n, void (*handler)(void), uint8_t flags)
{
    uint32_t base = (uint32_t)handler;
    idt[n].base_low  = base & 0xFFFF;
    idt[n].base_high = (base >> 16) & 0xFFFF;
    idt[n].selector  = GDT_KERNEL_CODE;
    idt[n].zero      = 0;
    idt[n].flags     = flags;
}

void idt_init(void)
{
    memset(idt, 0, sizeof(idt));
    idt_pointer.limit = sizeof(idt) - 1;
    idt_pointer.base  = (uint32_t)&idt;

    /* 0x8E = obecna, ring 0, 32-bitowa bramka przerwania */
#define SET_GATE(n) idt_set_gate(n, isr##n, 0x8E);
    STUB_LIST(SET_GATE)
#undef SET_GATE

    /* Syscall: 0xEE = DPL 3, wiec int 0x80 wolno wykonac z ring 3 */
    idt_set_gate(0x80, isr128, 0xEE);

    __asm__ volatile("lidt %0" : : "m"(idt_pointer));
}
