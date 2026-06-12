/* GDT - Global Descriptor Table
 *
 * GRUB zostawia nam swoją GDT, ale nie mamy gwarancji, gdzie leży
 * i co zawiera - jądro musi załadować własną. Używamy płaskiego
 * modelu pamięci: wszystkie segmenty obejmują pełne 4 GiB.
 * Segmenty użytkownika (ring 3) już przygotowane pod przyszłe procesy.
 */

#include <stdint.h>
#include "include/gdt.h"

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;
    uint8_t  granularity;   /* górne 4 bity limitu + flagi */
    uint8_t  base_high;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

static struct gdt_entry gdt[5];
static struct gdt_ptr gdt_pointer;

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

    /* access: P | DPL | S | typ; gran: G (strony 4K) | D (32-bit) */
    gdt_set_entry(0, 0, 0, 0, 0);                   /* null */
    gdt_set_entry(1, 0, 0xFFFFF, 0x9A, 0xC0);       /* kod jądra, ring 0 */
    gdt_set_entry(2, 0, 0xFFFFF, 0x92, 0xC0);       /* dane jądra, ring 0 */
    gdt_set_entry(3, 0, 0xFFFFF, 0xFA, 0xC0);       /* kod użytkownika, ring 3 */
    gdt_set_entry(4, 0, 0xFFFFF, 0xF2, 0xC0);       /* dane użytkownika, ring 3 */

    /* Załaduj GDT, przeładuj rejestry segmentowe; CS wymaga dalekiego skoku */
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
