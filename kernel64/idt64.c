/* The interrupt descriptor table in long mode
 *
 * An entry is sixteen bytes instead of eight - the handler address is
 * 64-bit and has to fit into it in three pieces. An IST field has been
 * added too: it lets every vector demand its own, chosen up front,
 * stack. We do not use it yet (zero = the same stack as the
 * interrupted code), but it is exactly that field which saves the
 * system on a stack fault, when the existing stack can no longer be
 * trusted.
 */

#include <stdint.h>
#include "../kernel/include/isr.h"
#include "idt64.h"

#define IDT_ENTRIES 256
#define KERNEL_CS   0x08
/* Present, ring 0, an interrupt gate (it clears interrupts for the
   duration of the handler) */
#define GATE_INTERRUPT 0x8E

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;                /* 0 = the ordinary stack */
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct idt_entry idt[IDT_ENTRIES];
static struct idt_ptr idtp;

static void set_gate(int n, void (*handler)(void))
{
    uint64_t addr = (uint64_t)(uintptr_t)handler;
    idt[n].offset_low = (uint16_t)addr;
    idt[n].selector = KERNEL_CS;
    idt[n].ist = 0;
    idt[n].type_attr = GATE_INTERRUPT;
    idt[n].offset_mid = (uint16_t)(addr >> 16);
    idt[n].offset_high = (uint32_t)(addr >> 32);
    idt[n].zero = 0;
}

/* Stubs from boot64/isr64.S - exceptions 0-31 and hardware interrupts
   32-47 */
#define DECL(n) extern void isr64_##n(void);
#define LIST(X) \
    X(0)  X(1)  X(2)  X(3)  X(4)  X(5)  X(6)  X(7)  \
    X(8)  X(9)  X(10) X(11) X(12) X(13) X(14) X(15) \
    X(16) X(17) X(18) X(19) X(20) X(21) X(22) X(23) \
    X(24) X(25) X(26) X(27) X(28) X(29) X(30) X(31) \
    X(32) X(33) X(34) X(35) X(36) X(37) X(38) X(39) \
    X(40) X(41) X(42) X(43) X(44) X(45) X(46) X(47)

LIST(DECL)
extern void isr64_48(void);   /* the local APIC timer (secondary cores) */
extern void isr64_49(void);   /* the inter-core wake-up */
extern void isr64_50(void);   /* a changed mapping (TLB shootdown) */

#define ENTRY(n) set_gate(n, isr64_##n);

void idt64_init(void)
{
    LIST(ENTRY)
    set_gate(48, isr64_48);              /* the local APIC timer */
    set_gate(49, isr64_49);              /* the inter-core wake-up */
    set_gate(50, isr64_50);              /* a changed mapping */

    idtp.limit = sizeof(idt) - 1;
    idtp.base = (uint64_t)(uintptr_t)idt;
    idt64_load();
}

/* The table itself is a single one for the whole machine - once built
   nobody changes it any more, so every core can point at the same
   one. */
void idt64_load(void)
{
    __asm__ volatile("lidt %0" : : "m"(idtp));
}
