/* GDT - the Global Descriptor Table
 *
 * GRUB leaves us its own GDT, but there is no guarantee where it lies
 * or what it holds - the kernel has to load one of its own. We use a
 * flat memory model: all segments span the full 4 GiB. The user
 * (ring 3) segments are already prepared for future processes.
 */

#include <stdint.h>
#include "include/gdt.h"
#include "include/smp.h"

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;
    uint8_t  granularity;   /* the upper 4 bits of the limit plus flags */
    uint8_t  base_high;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

/* The TSS - in protected mode used only so that the CPU knows which
   kernel stack (ss0:esp0) to jump to on an interrupt from ring 3 */
struct tss {
    uint32_t prev_tss;
    uint32_t esp0;
    uint32_t ss0;
    uint32_t unused[23];
} __attribute__((packed));

/* A task state segment for EVERY core.
 *
 * The processor reads the kernel stack from it on entry from ring 3 -
 * and every core enters there from a different task and onto a
 * different stack. One shared segment would mean two cores entering
 * the kernel on the same stack, and that is exactly the kind of bug
 * that does not show itself at once.
 *
 * There have to be as many descriptors as cores too: the ltr
 * instruction marks a descriptor as BUSY, so a second core loading the
 * same selector would take a protection fault. The table stays one -
 * only the entries and the selectors differ. */
static struct gdt_entry gdt[5 + SMP_CORES];
static struct gdt_ptr gdt_pointer;
static struct tss tss[SMP_CORES];

static void gdt_set_entry(int i, uint32_t base, uint32_t limit,
                          uint8_t access, uint8_t gran)
{
    gdt[i].base_low    = base & 0xFFFF;
    gdt[i].base_middle = (base >> 16) & 0xFF;
    gdt[i].base_high   = (base >> 24) & 0xFF;
    gdt[i].limit_low   = limit & 0xFFFF;
    gdt[i].granularity = ((limit >> 16) & 0x0F) | (gran & 0xF0);
    gdt[i].access      = access;
}

void gdt_init(void)
{
    gdt_pointer.limit = sizeof(gdt) - 1;
    gdt_pointer.base  = (uint32_t)&gdt;

    /* access: P | DPL | S | type; gran: G (4K pages) | D (32-bit) */
    gdt_set_entry(0, 0, 0, 0, 0);                   /* null */
    gdt_set_entry(1, 0, 0xFFFFF, 0x9A, 0xC0);       /* kernel code, ring 0 */
    gdt_set_entry(2, 0, 0xFFFFF, 0x92, 0xC0);       /* kernel data, ring 0 */
    gdt_set_entry(3, 0, 0xFFFFF, 0xFA, 0xC0);       /* user code, ring 3 */
    gdt_set_entry(4, 0, 0xFFFFF, 0xF2, 0xC0);       /* user data, ring 3 */

    for (int i = 0; i < SMP_CORES; i++) {
        tss[i].ss0 = GDT_KERNEL_DATA;
        /* access 0x89 = present, 32-bit TSS; gran 0x00 = the limit in bytes */
        gdt_set_entry(5 + i, (uint32_t)&tss[i], sizeof(tss[i]) - 1,
                      0x89, 0x00);
    }

    gdt_load();

    /* ONLY the boot core loads a task state segment. The ltr
       instruction marks a descriptor as busy, so the same table cannot
       be loaded twice - and there is no point anyway: there is one
       TSS, because it serves entry into ring 0 from ring 3, and user
       programs run only on the boot core for now. */
    gdt_load_tss();
}

/* Every core loads ITS OWN task state segment. The selector is
   computed from its number: the fifth table entry plus as much as the
   number is. */
void gdt_load_tss(void)
{
    uint16_t sel = (uint16_t)(GDT_TSS + smp_core_apic() * 8);
    __asm__ volatile("ltr %0" : : "r"(sel) : "memory");
}

/* Loads the already-built table on the current core and reloads the
   segment registers; CS requires a far jump. A secondary core calls
   this after entering protected mode, to step off the trampoline's
   temporary table onto the kernel's shared one. */
void gdt_load(void)
{
    __asm__ volatile(
        "lgdt %0\n\t"
        "movw %1, %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        "movw %%ax, %%ss\n\t"
        "ljmpl %2, $1f\n\t"
        "1:\n\t"
        :
        : "m"(gdt_pointer), "i"(GDT_KERNEL_DATA), "i"(GDT_KERNEL_CODE)
        : "eax", "memory");
}

void tss_set_esp0(uint32_t esp0)
{
    tss[smp_core()].esp0 = esp0;
}
