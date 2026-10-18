/* Four-level paging
 *
 * A virtual address is four nine-bit indices plus an offset within a
 * page. Every level is one 4 KiB frame with 512 entries of 64 bits.
 *
 * We describe memory with 2 MiB pages - one entry instead of 512, so a
 * whole hundred-megabyte area fits in a few tables rather than in
 * hundreds. Finer mappings are done by paging64_map(), and those are
 * exactly what processes will need.
 *
 * There is a bit that 32-bit paging did not have: no-execute. A data
 * page can now be marked so that the processor refuses to execute
 * anything lying in it - a framebuffer or a heap is no longer a
 * potential home for somebody else's code.
 */

#include <stdint.h>
#include "paging64.h"
#include "../kernel/include/tlb.h"
#include "../kernel/include/spinlock.h"
#include "../kernel/include/pmm.h"
#include "../kernel/include/fbcon.h"
#include "../kernel/include/string.h"
#include "../kernel/include/printk.h"
#include "../kernel/include/swap.h"
#include "../kernel/include/shm.h"

#define PTE_PRESENT 0x001
#define PTE_WRITE   0x002
#define PTE_HUGE    0x080
#define PTE_PWT     0x008  /* with PCD and PAT: which PAT entry */
#define PTE_ACCESSED 0x020  /* set by the processor when a page is used */
#define PTE_DIRTY    0x040  /* set by the processor on a write to a page */
#define ADDR_MASK   0x000FFFFFFFFFF000ULL

#define HUGE_SIZE   (2u * 1024 * 1024)

static uint64_t *pml4;             /* the kernel's space - the template for the rest */
static uint64_t identity_top;
static int nx_on;
static uint32_t cow_splits;        /* how many pages copying has split */

/* The address space the processor is working on right now.
 *
 * We do not keep it in a variable: the only truth is the CR3 register,
 * and its contents are a physical address - which, under the identity
 * mapping, is also the table's virtual address. */
static uint64_t *root_now(void)
{
    uint64_t cr3;
    __asm__ volatile("movq %%cr3, %0" : "=r"(cr3));
    return (uint64_t *)(uintptr_t)(cr3 & ADDR_MASK);
}

static uint64_t *table_at(uint64_t entry)
{
    return (uint64_t *)(uintptr_t)(entry & ADDR_MASK);
}

/* Returns the lower-level table, creating it when it does not exist yet.
 *
 * 'extra' are the permissions that have to be open along the way. The
 * processor computes access as the intersection of the entries from
 * ALL levels, so a closed intermediate entry invalidates the most
 * generous entry at the end of the path - a page marked as accessible
 * to the user was invisible to it anyway, because a table along the
 * way did not have that bit.
 *
 * We open them generously, and that is safe: access is decided by the
 * most restrictive entry anyway, and that is always the last one. */
static uint64_t *ensure(uint64_t *table, int idx, uint64_t extra)
{
    if (!(table[idx] & PTE_PRESENT)) {
        uint32_t frame = pmm_alloc_frame();
        if (!frame)
            return (uint64_t *)0;
        memset((void *)(uintptr_t)frame, 0, PAGE_SIZE);
        table[idx] = (uint64_t)frame | PTE_PRESENT | PTE_WRITE | extra;
    } else {
        table[idx] |= extra;        /* adding permissions to an existing path */
    }
    return table_at(table[idx]);
}

/* --- The lock on the page tables -------------------------------------
 *
 * The same reasoning as in the 32-bit kernel (kernel/paging.c): a page
 * fault no longer takes the big kernel lock for the causes that only
 * need a frame and one entry, so the tables need a lock of their own.
 * One for the whole kernel; the section under it is a walk of four
 * levels and a single write.
 *
 * Waiting under it we keep answering shootdowns - a fault arrives with
 * interrupts off, and a core that goes deaf while it waits is a core
 * somebody else waits for forever. */
static struct spinlock tables = SPINLOCK_FREE;

static void tables_lock(void)
{
    while (!spin_trylock(&tables)) {
        tlb_service();
        __asm__ volatile("pause" ::: "memory");
    }
}

static void tables_unlock(void)
{
    spin_unlock(&tables);
}

/* A changed entry of the CURRENT space.
 *
 * Every caller below works on the space the core is standing in - the
 * table walk starts from CR3 - so the one to be told is whoever else is
 * standing in it. On a machine where nobody else is, this costs exactly
 * one invlpg, the same as before. */
static void invalidate(uint64_t addr)
{
    tlb_invalidate((uintptr_t)paging64_current_space(), (uintptr_t)addr);
}

/* 'only_absent' = 1 means: map only if there is nothing there yet.
 *
 * That is what a page fault needs. Two cores can fault on the SAME page
 * of one address space at the same moment, both find it missing and
 * both bring a frame - and the loser has to learn that it lost, or the
 * winner's page, already written to, is replaced by an empty one. */
static int map_in_flags(uint64_t *top, uint64_t virt, uint64_t phys,
                        uint64_t flags, int only_absent)
{
    if (!top)
        return -1;

    /* The user bit has to run the whole path - otherwise ring 3 will
       not see the page even though its own entry allows it */
    uint64_t path = flags & PG_USER;
    int rc = -1;

    tables_lock();
    uint64_t *pdpt = ensure(top, (int)((virt >> 39) & 0x1FF), path);
    if (!pdpt)
        goto out;
    uint64_t *pd = ensure(pdpt, (int)((virt >> 30) & 0x1FF), path);
    if (!pd)
        goto out;

    int i2 = (int)((virt >> 21) & 0x1FF);
    if (pd[i2] & PTE_HUGE)
        goto out;               /* an area already described by a 2 MiB page */
    uint64_t *pt = ensure(pd, i2, path);
    if (!pt)
        goto out;

    int i = (int)((virt >> 12) & 0x1FF);
    if (only_absent && pt[i] != 0)
        goto out;               /* somebody was here first */

    pt[i] = (phys & ADDR_MASK) | PTE_PRESENT | flags;
    rc = 0;
out:
    tables_unlock();
    return rc;
}

static int map_in(uint64_t *top, uint64_t virt, uint64_t phys,
                  uint64_t flags)
{
    return map_in_flags(top, virt, phys, flags, 0);
}

int paging64_map(uint64_t virt, uint64_t phys, uint64_t flags)
{
    int rc = map_in(root_now(), virt, phys, flags);
    if (rc == 0)
        invalidate(virt);               /* only the current space has anything to
                                       invalidate in the translation buffer */
    return rc;
}

int paging64_map_if_absent(uint64_t virt, uint64_t phys, uint64_t flags)
{
    int rc = map_in_flags(root_now(), virt, phys, flags, 1);
    if (rc == 0)
        invalidate(virt);
    return rc == 0;
}

/* Is there nothing at all at this address?
 *
 * Stricter than 'not present': a page evicted to disk is absent too,
 * and filling it with zeros would lose what is on the disk. */
int paging64_entry_empty(uint64_t virt)
{
    uint64_t *top = root_now();
    int empty = 0;

    tables_lock();
    if (top) {
        uint64_t e = top[(virt >> 39) & 0x1FF];
        if (!(e & PTE_PRESENT))
            empty = 1;
        else {
            e = table_at(e)[(virt >> 30) & 0x1FF];
            if (!(e & PTE_PRESENT))
                empty = 1;
            else {
                e = table_at(e)[(virt >> 21) & 0x1FF];
                if (!(e & PTE_PRESENT))
                    empty = 1;
                else if (!(e & PTE_HUGE))
                    empty = table_at(e)[(virt >> 12) & 0x1FF] == 0;
            }
        }
    }
    tables_unlock();
    return empty;
}

/* One entry per two megabytes - that is how we describe memory and the
   framebuffer */
static int map_huge(uint64_t virt, uint64_t phys, uint64_t flags)
{
    uint64_t *pdpt = ensure(pml4, (int)((virt >> 39) & 0x1FF), 0);
    if (!pdpt)
        return -1;
    uint64_t *pd = ensure(pdpt, (int)((virt >> 30) & 0x1FF), 0);
    if (!pd)
        return -1;
    pd[(virt >> 21) & 0x1FF] = (phys & ~(uint64_t)(HUGE_SIZE - 1)) |
                               PTE_PRESENT | PTE_HUGE | flags;
    return 0;
}

uint64_t paging64_lookup(uint64_t virt)
{
    uint64_t *top = root_now();
    if (!top)
        return 0;
    uint64_t e = top[(virt >> 39) & 0x1FF];
    if (!(e & PTE_PRESENT))
        return 0;
    e = table_at(e)[(virt >> 30) & 0x1FF];
    if (!(e & PTE_PRESENT))
        return 0;
    e = table_at(e)[(virt >> 21) & 0x1FF];
    if (!(e & PTE_PRESENT))
        return 0;
    if (e & PTE_HUGE)
        return (e & ADDR_MASK) + (virt & (HUGE_SIZE - 1));
    e = table_at(e)[(virt >> 12) & 0x1FF];
    if (!(e & PTE_PRESENT))
        return 0;
    return (e & ADDR_MASK) + (virt & 0xFFF);
}

/* A pointer to the page table entry of a given address, or NULL. We
   return the entry rather than the frame: the caller wants the bits
   too. */
static uint64_t *page_entry(uint64_t virt)
{
    uint64_t *top = root_now();
    if (!top)
        return (uint64_t *)0;
    uint64_t e = top[(virt >> 39) & 0x1FF];
    if (!(e & PTE_PRESENT))
        return (uint64_t *)0;
    e = table_at(e)[(virt >> 30) & 0x1FF];
    if (!(e & PTE_PRESENT))
        return (uint64_t *)0;
    e = table_at(e)[(virt >> 21) & 0x1FF];
    if (!(e & PTE_PRESENT) || (e & PTE_HUGE))
        return (uint64_t *)0;
    return &table_at(e)[(virt >> 12) & 0x1FF];
}

/* Has a page of a writable mapping been touched since it was last
   written back to disk? The processor answers for us - it sets the
   "dirty" bit itself on every write, so we write back only what
   changed. */
int paging64_file_dirty(uint64_t virt)
{
    uint64_t *w = page_entry(virt);
    if (!w)
        return 0;
    return (*w & PTE_PRESENT) && (*w & PG_FILE) && (*w & PTE_DIRTY);
}

/* Clears the "dirty" bit after a page has been written back to its file */
void paging64_file_clean(uint64_t virt)
{
    uint64_t *w = page_entry(virt);
    if (!w)
        return;
    *w &= ~(uint64_t)PTE_DIRTY;
    invalidate(virt);
}

/* Removes a page's mapping and returns the frame that lay under it
   (0 = there was nothing). We do not free the intermediate tables:
   they cost one frame per two megabytes and will soon come in handy. */
uint64_t paging64_unmap(uint64_t virt)
{
    uint64_t *top = root_now();
    if (!top)
        return 0;
    uint64_t e = top[(virt >> 39) & 0x1FF];
    if (!(e & PTE_PRESENT))
        return 0;
    uint64_t *pdpt = table_at(e);
    e = pdpt[(virt >> 30) & 0x1FF];
    if (!(e & PTE_PRESENT))
        return 0;
    uint64_t *pd = table_at(e);
    e = pd[(virt >> 21) & 0x1FF];
    if (!(e & PTE_PRESENT) || (e & PTE_HUGE))
        return 0;
    uint64_t *pt = table_at(e);
    int i = (int)((virt >> 12) & 0x1FF);
    if (!(pt[i] & PTE_PRESENT))
        return 0;
    uint64_t phys = pt[i] & ADDR_MASK;
    pt[i] = 0;
    invalidate(virt);
    return phys;
}

/* Is the whole range visible to ring 3?
 *
 * The kernel has no right to believe a pointer supplied by a program:
 * a process could slip in the address of its own page tables or of
 * kernel memory and have them printed out for it. Access rights are
 * the intersection over all four levels, so the USER bit has to be on
 * each of them - exactly as the processor checks it. */
int paging64_user_range(uint64_t virt, uint64_t len)
{
    uint64_t *top = root_now();
    if (!top || len == 0)
        return len == 0;
    if (virt + len < virt)                   /* the counter wrapped around */
        return 0;

    for (uint64_t va = virt & ~0xFFFULL; va < virt + len; va += 4096) {
        uint64_t e = top[(va >> 39) & 0x1FF];
        if (!(e & PTE_PRESENT) || !(e & PG_USER))
            return 0;
        e = table_at(e)[(va >> 30) & 0x1FF];
        if (!(e & PTE_PRESENT) || !(e & PG_USER))
            return 0;
        e = table_at(e)[(va >> 21) & 0x1FF];
        if (!(e & PTE_PRESENT) || !(e & PG_USER))
            return 0;
        if (e & PTE_HUGE)
            continue;
        e = table_at(e)[(va >> 12) & 0x1FF];
        if (!(e & PTE_PRESENT) || !(e & PG_USER))
            return 0;
    }
    return 1;
}

/* --- Process address spaces --------------------------------------
 *
 * Every process gets its own table root, but kernel memory has to be
 * visible from each of them - otherwise the first interrupt after a
 * CR3 switch would execute in a void.
 *
 * The division falls out of the address layout itself. The top level
 * splits the space into chunks of 512 GiB, the second into gigabytes.
 * All physical memory, the kernel and the framebuffer fit into chunks
 * other than that second gigabyte where the programs live. So it is
 * enough to give a process its own second-level table and clear that
 * one entry in it: the rest stays SHARED, because it points at the
 * same frames as the kernel's. A mapping added by the kernel after
 * startup is visible at once in all the spaces at the same time.
 */

uint64_t paging64_kernel_space(void)
{
    return (uint64_t)(uintptr_t)pml4;
}

uint64_t paging64_new_space(void)
{
    if (!pml4)
        return 0;

    uint32_t root = pmm_alloc_frame();
    if (!root)
        return 0;
    uint32_t mid = pmm_alloc_frame();
    if (!mid) {
        pmm_free_frame(root);
        return 0;
    }

    uint64_t *top = (uint64_t *)(uintptr_t)root;
    uint64_t *pdpt = (uint64_t *)(uintptr_t)mid;

    memcpy(top, pml4, PAGE_SIZE);                 /* everything from the kernel */
    memcpy(pdpt, table_at(pml4[0]), PAGE_SIZE);   /* the first 512 GiB */
    pdpt[PAGING64_USER_PDPT] = 0;                 /* the program's gigabyte - empty */

    /* The user bit on this path opens it only from above: access is
       decided by the most restrictive entry anyway, and that lies at
       the end - in the page table */
    top[0] = (uint64_t)mid | PTE_PRESENT | PTE_WRITE | PG_USER;
    return (uint64_t)root;
}

/* Duplicates a process's address space - without copying memory.
 *
 * The parent and the child get the same frames. A page that was
 * writable loses its write permission in BOTH and gets the COW marker:
 * on the first attempt to write, the processor reports a page fault,
 * and only then does the kernel make a copy - and only of that one
 * page.
 *
 * Read-only pages (code, constants) do not even need that: since
 * nobody will change them, they can stay shared forever. The owner
 * counter in the PMM sees to that - a frame goes back to the pool only
 * when the last one releases it.
 *
 * The gain is directly proportional to the program's size: fork stops
 * costing as much as the process occupies in memory and starts costing
 * as much as it has page tables.
 *
 * Returns the root of the new space or 0. */
uint64_t paging64_clone_space(uint64_t src)
{
    uint64_t dst = paging64_new_space();
    if (!dst)
        return 0;

    uint64_t *stop = (uint64_t *)(uintptr_t)src;
    uint64_t *dtop = (uint64_t *)(uintptr_t)dst;
    uint64_t e = table_at(stop[0])[PAGING64_USER_PDPT];
    if (!(e & PTE_PRESENT))
        return dst;                     /* an empty process - nothing to share */

    uint64_t *pd = table_at(e);
    for (int i = 0; i < 512; i++) {
        if (!(pd[i] & PTE_PRESENT) || (pd[i] & PTE_HUGE))
            continue;
        uint64_t *pt = table_at(pd[i]);
        for (int j = 0; j < 512; j++) {
            if (!(pt[j] & PTE_PRESENT))
                continue;

            uint64_t va = ((uint64_t)PAGING64_USER_PDPT << 30) |
                          ((uint64_t)i << 21) | ((uint64_t)j << 12);
            uint64_t frame = pt[j] & ADDR_MASK;
            uint64_t flags = pt[j] &
                             (PG_WRITE | PG_USER | PG_NX | PG_COW | PG_FILE);

            /* A writable page goes to both as read-only with the
               marker - otherwise the parent would write over the
               child's memory without knowing it. The exception is a
               writable file mapping: there the parent and the child
               ARE to write over the same thing, because they write to
               one file. */
            if ((flags & PG_WRITE) && !(flags & PG_FILE)) {
                flags = (flags & ~(uint64_t)PG_WRITE) | PG_COW;
                pt[j] = frame | PTE_PRESENT | flags;
                invalidate(va);
            }

            pmm_frame_ref((uint32_t)frame);
            if (map_in(dtop, va, frame, flags) < 0) {
                pmm_free_frame((uint32_t)frame);
                paging64_free_space(dst);
                return 0;
            }
        }
    }
    return dst;
}

/* A write to a page shared after a fork.
 *
 * If the frame has only one owner left, there is nothing to split -
 * handing back write permission is enough. That is a common case: the
 * other side of the fork has already finished or wrote that page
 * itself earlier.
 *
 * Returns 1 when the page was split and the program can retry the
 * write; 0 means this was not a fault of that kind. */
int paging64_handle_cow(uint64_t virt)
{
    uint64_t *top = root_now();
    if (!top)
        return 0;

    /* One core at a time from here to the end of the split: two of them
       faulting on the same shared page would otherwise both make a copy,
       and the frame they replaced would lose an owner twice. */
    tables_lock();
    uint64_t e = top[(virt >> 39) & 0x1FF];
    if (!(e & PTE_PRESENT))
        goto not_ours;
    e = table_at(e)[(virt >> 30) & 0x1FF];
    if (!(e & PTE_PRESENT))
        goto not_ours;
    e = table_at(e)[(virt >> 21) & 0x1FF];
    if (!(e & PTE_PRESENT) || (e & PTE_HUGE))
        goto not_ours;

    uint64_t *pt = table_at(e);
    int i = (int)((virt >> 12) & 0x1FF);
    if (!(pt[i] & PTE_PRESENT) || !(pt[i] & PG_COW)) {
        /* Somebody split it a moment ago - which is an answer too: the
           instruction may be repeated and will go through */
        int done = (pt[i] & PTE_PRESENT) && (pt[i] & PG_WRITE) &&
                   !(pt[i] & PG_COW);
        tables_unlock();
        return done;
    }

    uint64_t old = pt[i] & ADDR_MASK;
    uint64_t flags = (pt[i] & (PG_USER | PG_NX)) | PG_WRITE;

    if (pmm_frame_refs((uint32_t)old) <= 1) {
        pt[i] = old | PTE_PRESENT | flags;      /* we are alone */
        tables_unlock();
        invalidate(virt);
        return 1;
    }
    cow_splits++;

    uint32_t fresh = pmm_alloc_frame();
    if (!fresh) {
        tables_unlock();
        return 0;
    }
    memcpy((void *)(uintptr_t)fresh, (const void *)(uintptr_t)old, PAGE_SIZE);
    pmm_free_frame((uint32_t)old);              /* one owner fewer */

    pt[i] = (uint64_t)fresh | PTE_PRESENT | flags;
    tables_unlock();
    invalidate(virt);
    return 1;

not_ours:
    tables_unlock();
    return 0;
}

uint32_t paging64_cow_splits(void)
{
    return cow_splits;
}

uint64_t paging64_current_space(void)
{
    uint64_t cr3;
    __asm__ volatile("movq %%cr3, %0" : "=r"(cr3));
    return cr3 & ADDR_MASK;
}

void paging64_switch(uint64_t space)
{
    tlb_switch((uintptr_t)space);
}

/* Releases a process's address space: the program's pages, its tables
   and the root. It does not touch the kernel tables - those are shared
   and live on. */
void paging64_free_space(uint64_t space)
{
    if (!space || space == (uint64_t)(uintptr_t)pml4)
        return;

    uint64_t *top = (uint64_t *)(uintptr_t)space;
    uint64_t *pdpt = table_at(top[0]);
    uint64_t e = pdpt[PAGING64_USER_PDPT];

    if (e & PTE_PRESENT) {
        uint64_t *pd = table_at(e);
        for (int i = 0; i < 512; i++) {
            if (!(pd[i] & PTE_PRESENT) || (pd[i] & PTE_HUGE))
                continue;
            uint64_t *pt = table_at(pd[i]);
            for (int j = 0; j < 512; j++) {
                if (pt[j] & PTE_PRESENT)
                    pmm_free_frame((uint32_t)(pt[j] & ADDR_MASK));
                else if (pt[j] & PG_SWAPPED)
                    swap_free_slot((int)((pt[j] & ADDR_MASK) >> 12));
            }
            pmm_free_frame((uint32_t)(pd[i] & ADDR_MASK));
        }
        pmm_free_frame((uint32_t)(e & ADDR_MASK));
    }
    pmm_free_frame((uint32_t)((uint64_t)(uintptr_t)pdpt & ADDR_MASK));
    pmm_free_frame((uint32_t)space);
}

/* --- Swapping pages to disk -------------------------------------------
 *
 * The entry of a page evicted to disk: without the present bit, with a
 * marker and the slot number in the upper bits. The processor does not
 * look at such an entry - to it that is simply a page that is not
 * there.
 *
 * The access rights travel with it. They must not be guessed on the
 * way back: a page of program code is read-only and executable, a data
 * page the other way round. Giving everybody one and the same set of
 * rights ends with a program taking an exception at its first
 * instruction after pulling its own code back from disk - because the
 * code came back with a no-execute mark.
 */
static uint64_t swap_pte(int slot, uint64_t right)
{
    return ((uint64_t)slot << 12) | PG_SWAPPED |
           (right & (PG_WRITE | PG_USER | PG_NX));
}

/* The clock hand: where the last search for a victim stopped. Without
   it every search would start from the beginning and evict the same
   lowest-lying pages over and over. */
static int clock_pd, clock_pt;

/* Evicts at most 'want' pages of the given space to disk.
 *
 * The second-chance algorithm: the processor marks the "accessed" bit
 * in every entry it uses. A page with that bit is spared, we only
 * clear it - if nobody touches it before the next sweep, it goes. So
 * we take the ones that really lay fallow.
 *
 * We leave alone shared pages (shm), pages given over to copy-on-write
 * and frames belonging to more than one space: with those, swapping
 * would require counting owners on disk as well.
 *
 * We walk only the program's gigabyte (PAGING64_USER_PDPT). Kernel
 * memory does not go to disk - and even if it did, there would be
 * nobody to bring it back, because it is brought back by the page
 * fault handler. */
int paging64_reclaim_pages(uint64_t space, int want)
{
    if (!space || space == (uint64_t)(uintptr_t)pml4)
        return 0;

    uint64_t *top = (uint64_t *)(uintptr_t)space;
    if (!(top[0] & PTE_PRESENT))
        return 0;
    uint64_t e = table_at(top[0])[PAGING64_USER_PDPT];
    if (!(e & PTE_PRESENT) || (e & PTE_HUGE))
        return 0;
    uint64_t *pd = table_at(e);
    int freed = 0;

    /* Two sweeps: the first clears the "accessed" bits, the second takes */
    for (int pass = 0; pass < 2 && freed < want; pass++) {
        for (int n = 0; n < 512 && freed < want; n++) {
            int i = (clock_pd + n) % 512;
            if (!(pd[i] & PTE_PRESENT) || (pd[i] & PTE_HUGE))
                continue;
            uint64_t *pt = table_at(pd[i]);
            /* Inside a table we also start where we stopped - otherwise
               every search would sweep again over pages that went to
               disk long ago */
            int start = (n == 0) ? clock_pt : 0;
            for (int k = 0; k < 512 && freed < want; k++) {
                int j = (start + k) % 512;
                uint64_t w = pt[j];
                if (!(w & PTE_PRESENT) || !(w & PG_USER) || (w & PG_COW))
                    continue;
                /* There is no point evicting a page of a writable mapping
                   to the swap file - its place on disk is the mapped
                   file itself */
                if (w & PG_FILE)
                    continue;
                uint64_t frame = w & ADDR_MASK;
                if (shm_is_shared((uint32_t)frame) ||
                    pmm_frame_refs((uint32_t)frame) != 1)
                    continue;
                if (w & PTE_ACCESSED) {
                    pt[j] = w & ~(uint64_t)PTE_ACCESSED;   /* second chance */
                    continue;
                }

                int slot = swap_alloc_slot();
                if (slot < 0)
                    goto end;              /* the swap file is full */
                if (swap_write(slot, (const void *)(uintptr_t)frame) < 0) {
                    swap_free_slot(slot);
                    goto end;
                }
                pt[j] = swap_pte(slot, w);
                pmm_free_frame((uint32_t)frame);
                clock_pd = i;
                clock_pt = j + 1;
                freed++;
            }
        }
        /* After the first sweep all the bits are cleared - the second
           sweep will find victims, if there is anything to take at
           all */
    }
end:
    /* Many entries changed at once and the frames behind them are gone
       - throwing the whole cache away is cheaper than naming them one
       by one, and it has to reach every core running this space. */
    if (freed)
        tlb_flush_space((uintptr_t)space);
    return freed;
}

/* Pulls back from disk a page the program has just tried to touch.
   Returns 1 when the page is back and the instruction can be resumed. */
int paging64_swap_in(uint64_t space, uint64_t virt)
{
    if (!space)
        return 0;

    uint64_t *top = (uint64_t *)(uintptr_t)space;
    uint64_t e = top[(virt >> 39) & 0x1FF];
    if (!(e & PTE_PRESENT))
        return 0;
    e = table_at(e)[(virt >> 30) & 0x1FF];
    if (!(e & PTE_PRESENT) || (e & PTE_HUGE))
        return 0;
    e = table_at(e)[(virt >> 21) & 0x1FF];
    if (!(e & PTE_PRESENT) || (e & PTE_HUGE))
        return 0;

    uint64_t *pt = table_at(e);
    int i = (int)((virt >> 12) & 0x1FF);
    uint64_t w = pt[i];
    if ((w & PTE_PRESENT) || !(w & PG_SWAPPED))
        return 0;

    /* We cut the address out with a mask: the upper bits of the entry
   carry the access rights */
    int slot = (int)((w & ADDR_MASK) >> 12);
    uint32_t frame = pmm_alloc_frame();
    if (!frame)
        return 0;                       /* not even enough memory to bring it back */
    if (swap_read(slot, (void *)(uintptr_t)frame) < 0) {
        pmm_free_frame(frame);
        return 0;
    }
    pt[i] = (uint64_t)frame | PTE_PRESENT |
            (w & (PG_WRITE | PG_USER | PG_NX));
    swap_free_slot(slot);
    invalidate(virt);
    return 1;
}

/* Gluing onto the mapping an area lying outside memory.
 *
 * Device registers lie high - well above what the mapping built at
 * startup describes, because that one reaches only the top of the
 * memory the PMM manages. Without this the first read from a BAR ends
 * in a page fault.
 *
 * Writing yes, executing no: there is no code in a device's
 * registers. */
/* --- The screen's memory: write-combining ----------------------------
 *
 * A framebuffer is memory on the graphics card, and the firmware's
 * MTRRs mark it uncached: every store is a bus transaction of its own.
 * Under QEMU that costs nothing - the "card" is RAM - and on a real
 * laptop a frame of 1366x768 took tens of milliseconds, which is a
 * desktop that animates in jerks. Write-combining lets the processor
 * gather stores into whole lines and send those; it is what every
 * system maps a framebuffer as.
 *
 * The page attribute table (the PAT MSR) says what each combination
 * of a page entry's PWT, PCD and PAT bits means. Entry 1 - PWT alone -
 * is write-through by default, and nothing in this kernel asks for it;
 * it becomes write-combining. A PAT entry of WC beats an MTRR of UC.
 * Every core has its own copy of the MSR and all of them have to say
 * the same, so the secondary cores do this too (kernel/smp.c). */
#define MSR_PAT 0x277
#define PAT_WC_AT_1 0x0007040600070106ULL  /* WB WC UC- UC WB WT UC- UC */

static int pat_wc;

void paging64_pat_init(void)
{
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1));
    if (!(d & (1u << 16)))
        return;                      /* no PAT: the screen stays uncached */
    /* The caches must not hold lines under an old meaning */
    __asm__ volatile("wbinvd" ::: "memory");
    __asm__ volatile("wrmsr" : : "c"(MSR_PAT), "a"((uint32_t)PAT_WC_AT_1),
                     "d"((uint32_t)(PAT_WC_AT_1 >> 32)));
    __asm__ volatile("wbinvd" ::: "memory");
    uint64_t cr3;
    __asm__ volatile("movq %%cr3, %0; movq %0, %%cr3" : "=r"(cr3) :: "memory");
    pat_wc = 1;
}

int paging64_fb_wc(void)
{
    return pat_wc;
}

/* The same as below, for a framebuffer: write-combining where the
   processor has a PAT */
int paging64_map_phys_wc(uint64_t phys, uint64_t len)
{
    uint64_t start = phys & ~0xFFFULL;
    uint64_t end = (phys + len + PAGE_SIZE - 1) & ~0xFFFULL;

    for (uint64_t a = start; a < end; a += PAGE_SIZE) {
        if (paging64_lookup(a) == a)
            continue;
        if (paging64_map(a, a, PG_WRITE | PG_NX |
                               (pat_wc ? PTE_PWT : 0)) < 0)
            return -1;
        tlb_flush_kernel((uintptr_t)a);
    }
    return 0;
}

int paging64_map_phys(uint64_t phys, uint64_t len)
{
    uint64_t start = phys & ~0xFFFULL;
    uint64_t end = (phys + len + PAGE_SIZE - 1) & ~0xFFFULL;

    for (uint64_t a = start; a < end; a += PAGE_SIZE) {
        if (paging64_lookup(a) == a)
            continue;
        if (paging64_map(a, a, PG_WRITE | PG_NX) < 0)
            return -1;
        /* A kernel table - every space has it, so every core does */
        tlb_flush_kernel((uintptr_t)a);
    }
    return 0;
}

uint64_t paging64_identity_top(void)
{
    return identity_top;
}

int paging64_nx_enabled(void)
{
    return nx_on;
}

int paging64_init(void)
{
    /* No-execute has to be enabled in EFER first - otherwise bit 63 of
       an entry is reserved and setting it ends in a fault */
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(0xC0000080));
    low |= (1u << 11);                       /* NXE */
    __asm__ volatile("wrmsr" : : "a"(low), "d"(high), "c"(0xC0000080));
    nx_on = 1;
    paging64_pat_init();

    uint32_t root = pmm_alloc_frame();
    if (!root)
        return -1;
    memset((void *)(uintptr_t)root, 0, PAGE_SIZE);
    pml4 = (uint64_t *)(uintptr_t)root;

    /* All the memory the PMM manages - identity-mapped. The kernel,
       its stack and all the page tables lie in that area, so nothing
       vanishes from under our feet after a CR3 switch. */
    uint64_t top = (pmm_managed_top() + HUGE_SIZE - 1) & ~(uint64_t)(HUGE_SIZE - 1);
    for (uint64_t a = 0; a < top; a += HUGE_SIZE)
        if (map_huge(a, a, PTE_WRITE | PG_NX) < 0)
            return -1;
    identity_top = top;

    /* The kernel code has to stay executable - the first two
       megabytes above one megabyte are the kernel image, so we lift
       the no-execute mark from that one entry */
    if (map_huge(0, 0, PTE_WRITE) < 0 ||
        map_huge(HUGE_SIZE, HUGE_SIZE, PTE_WRITE) < 0)
        return -1;

    /* The framebuffer lies high, outside memory - and it certainly has
       no code in it to execute. Write-combining, see above. */
    if (fbcon_have()) {
        uint64_t fb = fbcon_addr();
        uint64_t end = fb + fbcon_bytes();
        for (uint64_t a = fb & ~(uint64_t)(HUGE_SIZE - 1); a < end;
             a += HUGE_SIZE)
            if (map_huge(a, a, PTE_WRITE | PG_NX |
                               (pat_wc ? PTE_PWT : 0)) < 0)
                return -1;
    }

    __asm__ volatile("movq %0, %%cr3" : : "r"((uint64_t)root) : "memory");
    return 0;
}
