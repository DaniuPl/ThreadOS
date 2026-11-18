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
 * level 1 where every entry is a gigabyte. Only TTBR0 is used: a
 * program's table (below) is this one with the program's pages added.
 */

#include <stdint.h>
#include "arm64.h"
#include "proc.h"
#include "../kernel/include/pmm.h"

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

/* --- the tables of a program ------------------------------------------
 *
 * A program has a table of its own for TTBR0: the kernel's entries (the
 * devices and RAM, each address on itself, reachable only at EL1 - their
 * AP bits give EL0 nothing) copied in, and its own pages above
 * USER_BASE, which no entry of the kernel's reaches. The kernel's half
 * being the same in every table is what lets the kernel run, take an
 * interrupt or switch threads in whichever table is loaded, and read a
 * program's memory where it is, through the pointer the program gave.
 *
 * A program's pages are 4 KiB, in level 2 and level 3 tables made as
 * they are needed. Every one is non-global (nG), never executable at
 * EL1 (PXN), and executable at EL0 only where the program's file said
 * so; writable ones are not executable.
 */


#define DESC_PAGE   0x3UL
#define AP_EL0_RW   (1UL << 6)
#define AP_EL0_RO   (3UL << 6)
#define NG          (1UL << 11)
#define ADDR_MASK   0x0000FFFFFFFFF000UL

uint64_t mmu_kernel_ttbr0(void)
{
    return (uint64_t)(uintptr_t)l1;
}

static uint64_t *zeroed_frame(void)
{
    uint32_t pa = pmm_alloc_frame();
    if (!pa)
        return 0;
    uint64_t *p = (uint64_t *)(uintptr_t)pa;
    for (int i = 0; i < ENTRIES; i++)
        p[i] = 0;
    return p;
}

uint64_t *uspace_new(void)
{
    uint64_t *t = zeroed_frame();
    if (!t)
        return 0;
    for (int i = 0; i < ENTRIES; i++)
        t[i] = i < USER_L1_FIRST ? l1[i] : 0;
    return t;
}

/* The level 3 entry for va, its tables made on the way when 'make' */
static uint64_t *pte_of(uint64_t *t, uint64_t va, int make)
{
    unsigned i1 = (va >> 30) & 511, i2 = (va >> 21) & 511,
             i3 = (va >> 12) & 511;
    if (i1 < USER_L1_FIRST)
        return 0;
    uint64_t *tables[2] = { t, 0 };
    unsigned idx[2] = { i1, i2 };
    for (int lv = 0; lv < 2; lv++) {
        uint64_t e = tables[lv][idx[lv]];
        if (!(e & 1)) {
            if (!make)
                return 0;
            uint64_t *n = zeroed_frame();
            if (!n)
                return 0;
            tables[lv][idx[lv]] = (uint64_t)(uintptr_t)n | DESC_TABLE;
            e = tables[lv][idx[lv]];
        }
        if (lv == 0)
            tables[1] = (uint64_t *)(uintptr_t)(e & ADDR_MASK);
        else
            return (uint64_t *)(uintptr_t)(e & ADDR_MASK) + i3;
    }
    return 0;
}

int uspace_map(uint64_t *t, uint64_t va, uint64_t pa, int flags)
{
    uint64_t *pte = pte_of(t, va, 1);
    if (!pte || (*pte & 1))
        return -1;                       /* outside, no memory, or taken */
    uint64_t d = (pa & ADDR_MASK) | DESC_PAGE | ATTRIDX(ATTR_NORMAL) |
                 SH_INNER | AF | NG | PXN |
                 ((flags & UMAP_WRITE) ? AP_EL0_RW : AP_EL0_RO) |
                 ((flags & UMAP_EXEC) && !(flags & UMAP_WRITE) ? 0 : UXN);
    *pte = d;
    return 0;
}

uint64_t uspace_lookup(uint64_t *t, uint64_t va, int *flags)
{
    uint64_t *pte = pte_of(t, va, 0);
    if (!pte || !(*pte & 1))
        return 0;
    if (flags)
        *flags = ((*pte & (3UL << 6)) == AP_EL0_RW ? UMAP_WRITE : 0) |
                 (*pte & UXN ? 0 : UMAP_EXEC);
    return (*pte & ADDR_MASK) | (va & 0xFFF);
}

/* Every page of the program, every table, then the table itself. Only
   once no core has it loaded: its thread is gone (proc.c). */
void uspace_free(uint64_t *t)
{
    for (unsigned i1 = USER_L1_FIRST; i1 < ENTRIES; i1++) {
        if (!(t[i1] & 1))
            continue;
        uint64_t *l2 = (uint64_t *)(uintptr_t)(t[i1] & ADDR_MASK);
        for (unsigned i2 = 0; i2 < ENTRIES; i2++) {
            if (!(l2[i2] & 1))
                continue;
            uint64_t *l3 = (uint64_t *)(uintptr_t)(l2[i2] & ADDR_MASK);
            for (unsigned i3 = 0; i3 < ENTRIES; i3++)
                if (l3[i3] & 1)
                    pmm_free_frame((uint32_t)(l3[i3] & ADDR_MASK));
            pmm_free_frame((uint32_t)(uintptr_t)l3);
        }
        pmm_free_frame((uint32_t)(uintptr_t)l2);
    }
    pmm_free_frame((uint32_t)(uintptr_t)t);
}

/* Code written through the data side is not yet what the instruction
   side fetches: the lines go out to where both meet, and the
   instruction cache forgets what it had */
void cache_sync_code(uint64_t start, uint64_t len)
{
    uint64_t ctr = read_sysreg(ctr_el0);
    uint64_t dline = 4UL << ((ctr >> 16) & 0xF);
    uint64_t iline = 4UL << (ctr & 0xF);
    uint64_t end = start + len;
    for (uint64_t a = start & ~(dline - 1); a < end; a += dline)
        __asm__ volatile("dc cvau, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb ish" ::: "memory");
    for (uint64_t a = start & ~(iline - 1); a < end; a += iline)
        __asm__ volatile("ic ivau, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb ish; isb" ::: "memory");
}
