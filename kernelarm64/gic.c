/* The Generic Interrupt Controller, version 2
 *
 * What the 8259 and the APIC are on a PC. It has two halves at two
 * addresses: the distributor, which knows every interrupt - whether it
 * is enabled, how urgent it is, which core it goes to - and the CPU
 * interface, one per core, which hands a core the most urgent pending
 * interrupt (reading IAR acknowledges it) and is told when it has been
 * dealt with (writing EOIR).
 *
 * Interrupt IDs 0-15 are software-generated, 16-31 are private to each
 * core (its timers among them), 32 and up are shared devices.
 *
 * The addresses come from the device tree. A GICv3 is a different
 * thing - its CPU interface is system registers, not memory - and is
 * left for later; on one, this says so and takes no interrupts.
 */

#include <stdint.h>
#include "arm64.h"
#include "uart.h"

#define GICD_CTLR       0x000
#define GICD_ISENABLER  0x100
#define GICD_IPRIORITYR 0x400
#define GICD_ITARGETSR  0x800

#define GICC_CTLR       0x000
#define GICC_PMR        0x004
#define GICC_IAR        0x00C
#define GICC_EOIR       0x010

#define SPURIOUS        1023
#define HANDLED_IDS     64

static uintptr_t gicd, gicc;
static void (*handlers[HANDLED_IDS])(void);

static inline void w32(uintptr_t base, unsigned off, uint32_t v)
{
    *(volatile uint32_t *)(base + off) = v;
}

static inline uint32_t r32(uintptr_t base, unsigned off)
{
    return *(volatile uint32_t *)(base + off);
}

int gic_init(int version, uint64_t dist, uint64_t cpu)
{
    if (version != 2 || !dist || !cpu)
        return -1;
    gicd = (uintptr_t)dist;
    gicc = (uintptr_t)cpu;

    w32(gicd, GICD_CTLR, 0);
    w32(gicc, GICC_CTLR, 0);
    w32(gicd, GICD_CTLR, 1);
    gic_init_cpu();
    return 0;
}

/* The CPU interface is banked: the same address reaches a different
   one on every core, so every core sets up its own */
void gic_init_cpu(void)
{
    if (!gicc)
        return;
    /* Let every priority through; urgency is sorted out per interrupt */
    w32(gicc, GICC_PMR, 0xFF);
    w32(gicc, GICC_CTLR, 1);
}

void gic_enable(unsigned intid, void (*handler)(void))
{
    if (intid >= HANDLED_IDS || !gicd)
        return;
    handlers[intid] = handler;
    /* Priority registers take a byte per interrupt; lower is more
       urgent, and anything under the mask in PMR gets through. For the
       first 32 IDs both these and the enable bits are banked per core,
       like the CPU interface: each core enables its own timer. */
    *(volatile uint8_t *)(gicd + GICD_IPRIORITYR + intid) = 0xA0;
    /* A device's interrupt (32 and on) goes to the first core: nobody
       is its target until told, and then it is never raised */
    if (intid >= 32)
        *(volatile uint8_t *)(gicd + GICD_ITARGETSR + intid) = 1;
    w32(gicd, GICD_ISENABLER + 4 * (intid / 32), 1u << (intid % 32));
}

void gic_irq(void)
{
    if (!gicc)
        return;
    uint32_t iar = r32(gicc, GICC_IAR);
    uint32_t id = iar & 0x3FF;
    if (id == SPURIOUS)
        return;              /* gone by the time it was asked for */
    if (id < HANDLED_IDS && handlers[id])
        handlers[id]();
    else
        printk("An interrupt nobody asked for: %u\n", id);
    w32(gicc, GICC_EOIR, iar);
}
