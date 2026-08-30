/* Paging
 *
 * We identity-map all detected physical memory (virtual address =
 * physical) with 4 KiB pages and turn on the PG bit in CR0. The kernel
 * carries on at the same addresses, but from now on every access
 * outside the mapped area causes a page fault instead of silent memory
 * corruption.
 *
 * The first 64 MiB are described by static tables - the kernel has to
 * see memory before it can ask the PMM for frames for anything.
 * Further tables come from the PMM, so the limit is set by the amount
 * of RAM, not by the size of a table in .bss.
 *
 * The mapping ends at PAGING_USER_BASE at the latest: above it user
 * space begins, and there the same number means something different in
 * every process.
 */

#include <stdint.h>
#include "include/paging.h"
#include "include/pmm.h"
#include "include/string.h"
#include "include/shm.h"
#include "include/swap.h"
#include "include/printk.h"

#define PTE_PRESENT 0x1
#define PTE_WRITE   0x2
#define PTE_USER    0x4
#define PTE_ACCESSED 0x20   /* set by the processor when a page is used */
#define PTE_DIRTY   0x40    /* set by the processor on a write to a page */
/* Bits 9, 10 and 11 are at the system's disposal; the hardware
   ignores them.
   Bit 9  - page given over to copy-on-write (fork).
   Bit 10 - page evicted to disk; the slot number sits in the upper
            bits of the entry, which nobody cares about while the page
            is absent anyway.
   Bit 11 - page of a writable file mapping. Such a page must be
            neither copied on write (fork is to SHARE it, because both
            processes write to the same file) nor evicted to the swap
            file (its place on disk is the file itself). */
#define PTE_COW     0x200
#define PTE_SWAPPED 0x400
#define PTE_FILE    0x800

#define NUM_TABLES (PAGING_STATIC_LIMIT / (4096u * 1024))

static uint32_t page_directory[1024] __attribute__((aligned(4096)));
static uint32_t page_tables[NUM_TABLES][1024] __attribute__((aligned(4096)));
static uint32_t identity_limit;   /* how far the mapping really reaches */

uint32_t paging_identity_limit(void)
{
    return identity_limit;
}

void paging_init(void)
{
    for (uint32_t t = 0; t < NUM_TABLES; t++) {
        for (uint32_t i = 0; i < 1024; i++)
            page_tables[t][i] = ((t * 1024 + i) * 4096) | PTE_PRESENT | PTE_WRITE;
        page_directory[t] = (uint32_t)page_tables[t] | PTE_PRESENT | PTE_WRITE;
    }
    identity_limit = PAGING_STATIC_LIMIT;

    /* The rest of the detected memory - page tables now come from the
       PMM. We do not go higher than the start of user space. */
    uint32_t top = pmm_managed_top();
    if (top > PAGING_USER_BASE)
        top = PAGING_USER_BASE;
    if (top > PAGING_STATIC_LIMIT &&
        paging_map_phys(PAGING_STATIC_LIMIT, top - PAGING_STATIC_LIMIT) == 0)
        identity_limit = top;

    /* PG (bit 31) turns paging on, WP (bit 16) makes the write ban
       apply to the kernel too. Without WP kernel code would write into
       read-only pages without asking and break copy-on-write - a
       syscall filling a process's buffer would bypass the whole
       mechanism. */
    __asm__ volatile(
        "movl %0, %%cr3\n\t"
        "movl %%cr0, %%eax\n\t"
        "orl $0x80010000, %%eax\n\t"
        "movl %%eax, %%cr0\n\t"
        :
        : "r"(page_directory)
        : "eax", "memory");
}

uint32_t paging_kernel_cr3(void)
{
    return (uint32_t)page_directory;
}

int paging_map_phys(uint32_t phys, uint32_t size)
{
    uint32_t start = phys & ~0xFFFu;
    uint32_t end = (phys + size + 0xFFFu) & ~0xFFFu;

    for (uint32_t a = start; a < end; a += 4096) {
        uint32_t pde = a >> 22;
        uint32_t pte = (a >> 12) & 0x3FF;
        if (!(page_directory[pde] & PTE_PRESENT)) {
            uint32_t pt = pmm_alloc_frame();   /* a low frame, visible directly */
            if (!pt)
                return -1;
            memset((void *)pt, 0, 4096);
            page_directory[pde] = pt | PTE_PRESENT | PTE_WRITE;
        }
        uint32_t *pt = (uint32_t *)(page_directory[pde] & ~0xFFFu);
        pt[pte] = a | PTE_PRESENT | PTE_WRITE;
        __asm__ volatile("invlpg (%0)" : : "r"(a) : "memory");
    }
    return 0;
}

/* Does this directory entry describe a kernel table?
 *
 * Kernel tables are shared by all address spaces: they must not be
 * destroyed when a process is cleaned up, nor may user pages be
 * written into them.
 *
 * Comparing directly against page_directory[i] is not enough - the
 * processor sets the "accessed" bit in an entry it uses, so the same
 * table soon has different entry values in different directories. The
 * USER bit settles it, though: kernel tables do not have it and never
 * will.
 */
static int kernel_table(uint32_t pde)
{
    return (pde & PTE_PRESENT) && !(pde & PTE_USER);
}

/* Every frame from the PMM lies in the identity-mapped area, so the
   kernel sees it directly at its physical address - we modify process
   page directories and tables without any window or temporary
   mapping. */

uint32_t paging_new_address_space(void)
{
    uint32_t dir_phys = pmm_alloc_frame();
    if (!dir_phys)
        return 0;
    uint32_t *dir = (uint32_t *)dir_phys;
    /* Kernel mappings shared (the same page tables, supervisor), the
       remaining entries empty - that is where the user program will
       live */
    for (int i = 0; i < 1024; i++)
        dir[i] = page_directory[i];
    return dir_phys;
}

/* A pointer to the page table entry of a given address, or NULL when
   there is no table. We return the entry rather than the frame: the
   caller wants the bits too. */
static uint32_t *page_entry(uint32_t dir_phys, uint32_t vaddr)
{
    uint32_t *dir = (uint32_t *)dir_phys;
    uint32_t pde = vaddr >> 22;
    if (!(dir[pde] & PTE_PRESENT) || kernel_table(dir[pde]))
        return (uint32_t *)0;
    return &((uint32_t *)(dir[pde] & ~0xFFFu))[(vaddr >> 12) & 0x3FF];
}

/* The shared part of mapping a user page. 'flags' are the extra entry
   bits: write permission (a read-only mapping must not have it, so
   that a write attempt ends in a protection fault rather than a
   quietly lost change) and the file page marker. */
static int map_user(uint32_t dir_phys, uint32_t vaddr, uint32_t frame,
                      uint32_t flags)
{
    /* Do not allow a user page to be mapped over the kernel area */
    if (vaddr < PAGING_USER_BASE)
        return 0;

    uint32_t *dir = (uint32_t *)dir_phys;
    uint32_t pde = vaddr >> 22;
    uint32_t pte = (vaddr >> 12) & 0x3FF;
    uint32_t pt_phys;

    if (!(dir[pde] & PTE_PRESENT)) {
        pt_phys = pmm_alloc_frame();
        if (!pt_phys)
            return 0;
        memset((void *)pt_phys, 0, 4096);
        dir[pde] = pt_phys | PTE_PRESENT | PTE_WRITE | PTE_USER;
    } else {
        if (kernel_table(dir[pde]))   /* a kernel table - leave it alone */
            return 0;
        pt_phys = dir[pde] & ~0xFFFu;
    }

    ((uint32_t *)pt_phys)[pte] = frame | PTE_PRESENT | PTE_USER | flags;
    /* If we map into the active space (sbrk), refresh the TLB */
    uint32_t active;
    __asm__ volatile("movl %%cr3, %0" : "=r"(active));
    if (active == dir_phys)
        __asm__ volatile("invlpg (%0)" : : "r"(vaddr) : "memory");
    return 1;
}

int paging_map_user_in(uint32_t dir_phys, uint32_t vaddr, uint32_t frame)
{
    return map_user(dir_phys, vaddr, frame, PTE_WRITE);
}

int paging_map_user_ro_in(uint32_t dir_phys, uint32_t vaddr, uint32_t frame)
{
    return map_user(dir_phys, vaddr, frame, 0);
}

int paging_map_user_file_in(uint32_t dir_phys, uint32_t vaddr, uint32_t frame)
{
    return map_user(dir_phys, vaddr, frame, PTE_WRITE | PTE_FILE);
}

/* Has a page of a writable mapping been touched since it was last
   written back to disk? The processor answers for us: it sets the
   "dirty" bit itself on every write, so we write back only what really
   changed. */
int paging_file_dirty(uint32_t dir_phys, uint32_t vaddr)
{
    uint32_t *entry = page_entry(dir_phys, vaddr);
    if (!entry)
        return 0;
    uint32_t e = *entry;
    return (e & PTE_PRESENT) && (e & PTE_FILE) && (e & PTE_DIRTY);
}

/* Clears the "dirty" bit after a page has been written back to disk */
void paging_file_clean(uint32_t dir_phys, uint32_t vaddr)
{
    uint32_t *entry = page_entry(dir_phys, vaddr);
    if (!entry)
        return;
    *entry &= ~(uint32_t)PTE_DIRTY;
    uint32_t active;
    __asm__ volatile("movl %%cr3, %0" : "=r"(active));
    if (active == dir_phys)
        __asm__ volatile("invlpg (%0)" : : "r"(vaddr) : "memory");
}

uint32_t paging_get_mapping(uint32_t dir_phys, uint32_t vaddr)
{
    uint32_t *dir = (uint32_t *)dir_phys;
    uint32_t pde = vaddr >> 22;
    if (!(dir[pde] & PTE_PRESENT) || kernel_table(dir[pde]))
        return 0;
    uint32_t pte = ((uint32_t *)(dir[pde] & ~0xFFFu))[(vaddr >> 12) & 0x3FF];
    return pte & PTE_PRESENT ? pte & ~0xFFFu : 0;
}

void paging_destroy_address_space(uint32_t dir_phys)
{
    uint32_t *dir = (uint32_t *)dir_phys;
    for (int i = 0; i < 1024; i++) {
        /* Free only process tables - the kernel ones are shared */
        if (!(dir[i] & PTE_PRESENT) || kernel_table(dir[i]))
            continue;
        uint32_t *pt = (uint32_t *)(dir[i] & ~0xFFFu);
        for (int j = 0; j < 1024; j++) {
            if (!(pt[j] & PTE_PRESENT)) {
                /* A page lying on disk - give its slot back */
                if (pt[j] & PTE_SWAPPED)
                    swap_free_slot((int)(pt[j] >> 12));
                continue;
            }
            uint32_t frame = pt[j] & ~0xFFFu;
            /* We do not free shared memory frames - other processes may
               still be using them */
            if (!shm_is_shared(frame))
                pmm_free_frame(frame);
        }
        pmm_free_frame((uint32_t)pt);
    }
    pmm_free_frame(dir_phys);
}

/* --- Copy-on-write (fork) -------------------------------------------
 *
 * Cloning an address space copies not one byte of the process's
 * memory: both sides get the same physical frames, only without write
 * permission and with the PTE_COW marker. A frame knows how many
 * owners it has (pmm_frame_ref). The first write on either side falls
 * into a page fault, and paging_handle_cow() then makes a private copy
 * of that one page.
 */

static void flush_if_active(uint32_t dir_phys)
{
    uint32_t active;
    __asm__ volatile("movl %%cr3, %0" : "=r"(active));
    if (active == dir_phys)
        __asm__ volatile("movl %0, %%cr3" : : "r"(active) : "memory");
}

uint32_t paging_clone_address_space(uint32_t src_phys)
{
    uint32_t dst_phys = pmm_alloc_frame();
    if (!dst_phys)
        return 0;
    uint32_t *src = (uint32_t *)src_phys;
    uint32_t *dst = (uint32_t *)dst_phys;
    memset(dst, 0, 4096);

    for (int i = 0; i < 1024; i++) {
        /* Kernel tables are shared - we copy the entry unchanged */
        if (!(src[i] & PTE_PRESENT) || kernel_table(src[i])) {
            dst[i] = page_directory[i];
            continue;
        }

        uint32_t pt_phys = pmm_alloc_frame();
        if (!pt_phys) {
            paging_destroy_address_space(dst_phys);
            flush_if_active(src_phys);
            return 0;
        }
        uint32_t *spt = (uint32_t *)(src[i] & ~0xFFFu);
        uint32_t *dpt = (uint32_t *)pt_phys;

        for (int j = 0; j < 1024; j++) {
            uint32_t e = spt[j];
            if (!(e & PTE_PRESENT) && (e & PTE_SWAPPED)) {
                /* The parent's page lies on disk - pull it back before
                   sharing it with the child (one slot in the swap file
                   cannot belong to two address spaces) */
                uint32_t va = ((uint32_t)i << 22) | ((uint32_t)j << 12);
                if (paging_swap_in(src_phys, va))
                    e = spt[j];
            }
            if (!(e & PTE_PRESENT)) {
                dpt[j] = 0;
                continue;
            }
            uint32_t frame = e & ~0xFFFu;
            /* Shared memory is to stay shared - we do not copy it */
            if (shm_is_shared(frame)) {
                dpt[j] = e;
                continue;
            }
            /* A page of a writable file mapping is to stay SHARED
               together with its write permission: both processes write
               to the same file, so a private copy in the child would
               mean its writes never reach the disk. */
            if (e & PTE_FILE) {
                dpt[j] = e;
                pmm_frame_ref(frame);
                continue;
            }
            /* A page that is not writable anyway (a mapped file) stays
               as it is. Marking it for copy-on-write would mean the
               first write is to SUCCEED - and it is to fail, which is
               its whole point. */
            if (!(e & PTE_WRITE)) {
                dpt[j] = e;
                pmm_frame_ref(frame);
                continue;
            }
            e = (e & ~(uint32_t)PTE_WRITE) | PTE_COW;
            spt[j] = e;
            dpt[j] = e;
            pmm_frame_ref(frame);
        }
        dst[i] = pt_phys | PTE_PRESENT | PTE_WRITE | PTE_USER;
    }

    /* The parent has just lost write permission to its own pages */
    flush_if_active(src_phys);
    return dst_phys;
}

int paging_handle_cow(uint32_t vaddr, uint32_t err_code)
{
    if (!(err_code & 0x2))              /* not a write - none of our business */
        return 0;

    uint32_t dir_phys;
    __asm__ volatile("movl %%cr3, %0" : "=r"(dir_phys));
    uint32_t *dir = (uint32_t *)dir_phys;
    uint32_t pde = vaddr >> 22;
    if (!(dir[pde] & PTE_PRESENT) || kernel_table(dir[pde]))
        return 0;

    uint32_t *pt = (uint32_t *)(dir[pde] & ~0xFFFu);
    uint32_t idx = (vaddr >> 12) & 0x3FF;
    uint32_t e = pt[idx];
    if (!(e & PTE_PRESENT) || !(e & PTE_COW))
        return 0;

    uint32_t frame = e & ~0xFFFu;
    if (pmm_frame_refs(frame) <= 1) {
        /* We are alone now - handing back write permission is enough */
        pt[idx] = (e & ~(uint32_t)PTE_COW) | PTE_WRITE;
    } else {
        uint32_t copy = pmm_alloc_frame();
        if (!copy)
            return 0;                   /* out of memory - the process will die */
        memcpy((void *)copy, (const void *)frame, 4096);
        pmm_free_frame(frame);          /* one owner fewer */
        pt[idx] = copy | (e & 0xFFFu & ~(uint32_t)PTE_COW) | PTE_WRITE;
    }
    __asm__ volatile("invlpg (%0)" : : "r"(vaddr) : "memory");
    return 1;
}

/* --- swapping -------------------------------------------------------- */

/* The entry of a page evicted to disk: absent, with a marker and the
   slot number in the upper bits. The processor does not interpret such
   an entry. */
static uint32_t swap_pte(int slot)
{
    return ((uint32_t)slot << 12) | PTE_SWAPPED;
}

/* The clock hand: where the last search for a victim stopped. Without
   it every search would start from the beginning and evict the same
   lowest-lying pages over and over. */
static uint32_t clock_pde = 256;
static uint32_t clock_pte;

/* Evicts one page of the given address space to disk.
 *
 * The second-chance algorithm: the processor marks the "accessed" bit
 * in every entry it uses. A page with that bit is spared, we only
 * clear it - if nobody touches it before the next sweep, it goes. So
 * we take the ones that really lay fallow.
 *
 * We leave alone shared pages (shm), pages given over to copy-on-write
 * and frames belonging to more than one address space - with those,
 * swapping would require counting owners on disk as well.
 */
int paging_reclaim_pages(uint32_t dir_phys, int want)
{
    uint32_t *dir = (uint32_t *)dir_phys;
    int freed = 0;

    /* Two sweeps: the first clears the "accessed" bits, the second takes */
    for (int pass = 0; pass < 2 && freed < want; pass++) {
        /* User space is directory entries 256..767 */
        for (uint32_t n = 0; n < 512 && freed < want; n++) {
            uint32_t i = 256 + (clock_pde - 256 + n) % 512;
            if (!(dir[i] & PTE_PRESENT) || kernel_table(dir[i]))
                continue;
            uint32_t *pt = (uint32_t *)(dir[i] & ~0xFFFu);
            /* Inside a table we also start where we stopped - otherwise
               every search would sweep again over pages that went to
               disk long ago */
            uint32_t start = (n == 0) ? clock_pte : 0;
            for (uint32_t k = 0; k < 1024 && freed < want; k++) {
                uint32_t j = (start + k) % 1024;
                uint32_t e = pt[j];
                if (!(e & PTE_PRESENT) || !(e & PTE_USER) || (e & PTE_COW))
                    continue;
                /* There is no point evicting a page of a writable mapping
                   to the swap file - its place on disk is the mapped
                   file itself */
                if (e & PTE_FILE)
                    continue;
                uint32_t frame = e & ~0xFFFu;
                if (shm_is_shared(frame) || pmm_frame_refs(frame) != 1)
                    continue;
                if (e & PTE_ACCESSED) {
                    pt[j] = e & ~(uint32_t)PTE_ACCESSED;   /* second chance */
                    continue;
                }

                int slot = swap_alloc_slot();
                if (slot < 0)
                    goto done;              /* the swap file is full */
                if (swap_write(slot, (const void *)frame) < 0) {
                    swap_free_slot(slot);
                    goto done;
                }
                pt[j] = swap_pte(slot);
                pmm_free_frame(frame);
                clock_pde = i;
                clock_pte = j + 1;
                freed++;
            }
        }
        /* After the first sweep all the bits are cleared - the second
           sweep will find victims, if there is anything to take at
           all */
    }
done:
    if (freed)
        flush_if_active(dir_phys);
    return freed;
}

/* Pulls back from disk a page the process has just tried to touch.
   Returns 1 when the page is back (the instruction can be resumed). */
int paging_swap_in(uint32_t dir_phys, uint32_t vaddr)
{
    uint32_t *dir = (uint32_t *)dir_phys;
    uint32_t pde = vaddr >> 22;
    if (!(dir[pde] & PTE_PRESENT) || kernel_table(dir[pde]))
        return 0;

    uint32_t *pt = (uint32_t *)(dir[pde] & ~0xFFFu);
    uint32_t idx = (vaddr >> 12) & 0x3FF;
    uint32_t e = pt[idx];
    if ((e & PTE_PRESENT) || !(e & PTE_SWAPPED))
        return 0;

    int slot = (int)(e >> 12);
    uint32_t frame = pmm_alloc_frame();
    if (!frame)
        return 0;                       /* not even enough memory to bring it back */
    if (swap_read(slot, (void *)frame) < 0) {
        pmm_free_frame(frame);
        return 0;
    }
    pt[idx] = frame | PTE_PRESENT | PTE_WRITE | PTE_USER;
    swap_free_slot(slot);
    __asm__ volatile("invlpg (%0)" : : "r"(vaddr) : "memory");
    return 1;
}
