/* The MMU: one address space, mapped onto itself
 *
 * With the MMU off, every access is to Device memory: nothing is
 * cached, an unaligned access is a fault, and the exclusive loads and
 * stores a lock is built from do not work. So the MMU goes on early,
 * with the simplest tables there are - each address mapped to itself -
 * just to give each range of the physical map the right kind:
 *
 *   the first gigabyte (flash, the GIC, the UART, other devices) as
 *   Device memory, in 2 MiB blocks, never executable;
 *   RAM, from the device tree, as Normal write-back memory, in 1 GiB
 *   blocks.
 *
 * The first 2 MiB block is left out on purpose: address 0 is then not
 * the flash but a fault, and a null pointer is caught instead of read.
 *
 * 4 KiB granule, 39-bit addresses (T0SZ 25), so the walk starts at
 * level 1 where every entry is a gigabyte. Only TTBR0 is used.
 */

#include <stdint.h>
#include "arm64.h"

extern char __kernel_start[];

#define ENTRIES 512
static uint64_t l1[ENTRIES] __attribute__((aligned(4096)));
static uint64_t l2_low[ENTRIES] __attribute__((aligned(4096)));

/* MAIR_EL1 holds eight memory kinds; a descriptor names one by index */
#define ATTR_DEVICE 0               /* Device-nGnRE */
#define ATTR_NORMAL 1               /* Normal, write-back, read/write-allocate */
#define MAIR_VALUE  (0x04UL << (8 * ATTR_DEVICE) | 0xFFUL << (8 * ATTR_NORMAL))

#define DESC_BLOCK  0x1UL
#define DESC_TABLE  0x3UL
#define ATTRIDX(i)  ((uint64_t)(i) << 2)
#define SH_INNER    (3UL << 8)
#define AF          (1UL << 10)     /* accessed: else the first touch faults */
#define PXN         (1UL << 53)
#define UXN         (1UL << 54)

#define GIB         (1UL << 30)
#define MIB2        (1UL << 21)

/* Built once, by the first core; every core then walks the same tables */
int mmu_build(uint64_t ram_base, uint64_t ram_size)
{
    /* RAM in whole gigabytes, above the first: that is virt's map, and
       anything else would need finer tables than these */
    if (ram_size == 0 || (ram_base & (GIB - 1)) || ram_base < GIB ||
        ram_base + ram_size > ENTRIES * GIB)
        return -1;

    for (unsigned i = 1; i < ENTRIES; i++)
        l2_low[i] = i * MIB2 | ATTRIDX(ATTR_DEVICE) | AF | PXN | UXN |
                    DESC_BLOCK;
    l1[0] = (uint64_t)(uintptr_t)l2_low | DESC_TABLE;
    /* No code runs from RAM but the kernel's own: none at EL0 at all
       (UXN), and at EL1 none outside the gigabyte the kernel image lies
       in (PXN) - a buffer there that held bytes from outside is not a
       place to jump into. Finer than a gigabyte would need tables of
       2 MiB entries for the kernel's own, which is for later. */
    uint64_t kernel_gb = (uint64_t)(uintptr_t)__kernel_start / GIB;
    for (uint64_t gb = ram_base / GIB; gb <= (ram_base + ram_size - 1) / GIB;
         gb++)
        l1[gb] = gb * GIB | ATTRIDX(ATTR_NORMAL) | SH_INNER | AF | UXN |
                 (gb == kernel_gb ? 0 : PXN) | DESC_BLOCK;
    return 0;
}

/* Each core has its own MMU registers, so each core does this */
void mmu_switch_on(void)
{
    /* The physical address size the core supports goes into TCR as is */
    uint64_t parange = read_sysreg(id_aa64mmfr0_el1) & 0xF;
    uint64_t tcr = 25                   /* T0SZ: 39-bit addresses */
                 | 1UL << 8             /* IRGN0: walks cached, write-back */
                 | 1UL << 10            /* ORGN0: the same, outer */
                 | 3UL << 12            /* SH0: inner shareable */
                 | 0UL << 14            /* TG0: 4 KiB granule */
                 | 1UL << 23            /* EPD1: no walks through TTBR1 */
                 | parange << 32;       /* IPS */

    write_sysreg(mair_el1, MAIR_VALUE);
    write_sysreg(tcr_el1, tcr);
    write_sysreg(ttbr0_el1, (uint64_t)(uintptr_t)l1);
    /* The tables were written with the MMU off, straight to memory; the
       barriers make sure the walker sees them, and nothing stale is left
       in the TLB from before */
    __asm__ volatile("dsb ish; tlbi vmalle1; dsb ish; isb" ::: "memory");

    uint64_t sctlr = read_sysreg(sctlr_el1);
    sctlr |= 1UL << 0                   /* M: the MMU */
           | 1UL << 2                   /* C: data cache */
           | 1UL << 12;                 /* I: instruction cache */
    write_sysreg(sctlr_el1, sctlr);
    __asm__ volatile("isb" ::: "memory");
}
