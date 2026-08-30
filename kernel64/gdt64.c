/* The long mode descriptor table
 *
 * In long mode a segment's base and limit no longer mean anything -
 * the processor does not check them. Three things are left: the
 * privilege level, the L bit (64-bit code) and whether the descriptor
 * describes code or data.
 *
 * The order of the entries is not arbitrary. The sysret instruction,
 * which returns from the kernel to a program, does not take the
 * selectors from memory - it builds them out of a single value written
 * into the STAR register: data is that value plus eight, code plus
 * sixteen. So the user data segment has to lie before its code
 * segment, even though intuition suggests the opposite.
 *
 * The TSS in long mode no longer serves to switch tasks. It has been
 * left one job, but an indispensable one: to tell the processor which
 * stack to move to when an interrupt catches it in ring 3.
 */

#include <stdint.h>
#include "gdt64.h"
#include "../kernel/include/string.h"
#include "../kernel/include/smp.h"

struct tss64 {
    uint32_t reserved0;
    uint64_t rsp0, rsp1, rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

/* A task state segment for EVERY core.
 *
 * The processor reads the kernel stack from it on entry from ring 3 -
 * and every core enters there from a different task and onto a
 * different stack. There have to be as many descriptors as cores too:
 * ltr marks a descriptor as BUSY, so a second core loading the same
 * selector would get a protection fault.
 *
 * Five ordinary entries of eight bytes each, and then TWO for every
 * TSS - its address is sixty-four bits wide and does not fit into a
 * single entry. */
static uint64_t gdt[5 + 2 * SMP_CORES];
static struct tss64 tss[SMP_CORES];
static struct gdt_ptr gdtp;

static uint64_t entry(uint8_t access, uint8_t flags)
{
    /* Base zero, maximum limit - nobody looks at them anyway */
    return ((uint64_t)access << 40) | ((uint64_t)flags << 52) | 0xFFFFULL;
}

void gdt64_set_kernel_stack(uint64_t rsp)
{
    tss[smp_core()].rsp0 = rsp;
}

void gdt64_init(void)
{
    memset(tss, 0, sizeof(tss));
    for (int i = 0; i < SMP_CORES; i++)
        tss[i].iomap_base = sizeof(tss[i]);   /* no port map */

    gdt[0] = 0;
    gdt[1] = entry(0x9A, 0x2);           /* kernel code, the L bit */
    gdt[2] = entry(0x92, 0x0);           /* kernel data */
    gdt[3] = entry(0xF2, 0x0);           /* user data (ring 3) */
    gdt[4] = entry(0xFA, 0x2);           /* user code, the L bit */

    /* A TSS descriptor takes up two entries - the address is 64 bits */
    for (int i = 0; i < SMP_CORES; i++) {
        uint64_t base = (uint64_t)(uintptr_t)&tss[i];
        uint64_t limit = sizeof(tss[i]) - 1;
        gdt[5 + 2 * i] = (limit & 0xFFFF) | ((base & 0xFFFFFF) << 16) |
                         ((uint64_t)0x89 << 40) |
                         (((limit >> 16) & 0xF) << 48) |
                         (((base >> 24) & 0xFF) << 56);
        gdt[6 + 2 * i] = base >> 32;
    }

    gdtp.limit = sizeof(gdt) - 1;
    gdtp.base = (uint64_t)(uintptr_t)gdt;

    gdt64_load();

    gdt64_load_tss();
}

/* Every core loads ITS OWN task state segment. The selector is
   computed from its number: the fifth entry of the table plus sixteen
   bytes for every preceding core. */
void gdt64_load_tss(void)
{
    uint16_t sel = (uint16_t)(GDT64_TSS + smp_core_apic() * 16);
    __asm__ volatile("ltr %0" : : "r"(sel) : "memory");
}

/* Loads an already built table on the current core. A secondary core
   calls this once it reaches long mode, to move off the temporary
   table from the trampoline onto the kernel's common one. */
void gdt64_load(void)
{
    __asm__ volatile(
        "lgdt %0\n\t"
        /* Reloading CS requires a far jump; in long mode that is done
           through a return with the selector pushed */
        "pushq %1\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n"
        "1:\n\t"
        "movw %w2, %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%ss\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        :
        : "m"(gdtp), "i"((uint64_t)GDT64_KERNEL_CODE),
          "i"(GDT64_KERNEL_DATA)
        : "rax", "memory");
}
