#ifndef THREADOS_PAGING64_H
#define THREADOS_PAGING64_H

#include <stdint.h>

/* Four-level paging (long mode)
 *
 * A virtual address breaks down into four nine-bit indices plus an
 * offset within the page: PML4 -> PDPT -> PD -> PT -> page. Every
 * entry is 64 bits wide, so 512 of them fit in a 4 KiB frame.
 *
 * The tables laid out in the kernel image by boot64.S were enough to
 * get going at all. Now we build our own, out of frames from the PMM -
 * only those make it possible to map anything after startup.
 */

#define PG_WRITE  0x002
#define PG_USER   0x004
#define PG_NX     (1ULL << 63)      /* no code execution */

/* Bit 9 is left by the processor at the system's disposal. For us it
   means: the page is shared after a fork and is to be split on the
   first attempt to write. */
#define PG_COW    0x200

/* Bit 10, also at the system's disposal. It means: the page went out
   to disk, and the upper bits of the entry hold the number of its slot
   in the swap file. The processor never looks at an entry without the
   present bit anyway. */
#define PG_SWAPPED 0x400

/* Bit 11, the last of the free ones. It means: a page of a writable
   file mapping. Such a page must neither be copied on write (fork is
   to SHARE it, because both processes write to the same file) nor
   thrown out to the swap file - its place on disk is the file
   itself. */
#define PG_FILE   0x800

/* Which second-level entry belongs to the program. One entry is a
   gigabyte, and programs lie from 1 GiB upwards - USER64_BASE in
   kernel64/elf64.h has to agree with this. */
#define PAGING64_USER_PDPT 1

/* And the mapping area (USER64_MMAP_BASE): the second entry of the top
   table, a gigabyte per page directory, this many of them */
#define PAGING64_MMAP_PML4 1
#define PAGING64_MMAP_GIGS 64

/* All physical memory below 4 GiB, mapped once more where only the
   kernel looks: the top half of the address space (PML4 entry 256).
   The identity map reaches only the low gigabyte - programs lie at
   1 GiB - so a program's page from higher up (pmm_alloc_user_frame),
   or a table of the firmware's in that gigabyte, is reached here.
   RAM is cached; what lies above it - device registers, the
   firmware's tables - is not. */
#define PHYS_MAP_BASE 0xFFFF800000000000ULL
#define PHYS_MAP_SIZE (4ULL << 30)

static inline void *phys_ptr(uint64_t phys)
{
    return (void *)(uintptr_t)(PHYS_MAP_BASE + phys);
}

/* Builds our own tables and switches the processor over to them.
   0 = done. */
int paging64_init(void);

/* Maps a single 4 KiB page. 0 = mapped. */
int paging64_map(uint64_t virt, uint64_t phys, uint64_t flags);

/* Maps only if there is nothing at that address yet. Returns 1 when it
   mapped, 0 when somebody was there first - two cores can fault on the
   same page of one space at the same moment. */
int paging64_map_if_absent(uint64_t virt, uint64_t phys, uint64_t flags);

/* Is there nothing at all at that address? Stricter than 'not
   present': a page evicted to disk is absent too. */
int paging64_entry_empty(uint64_t virt);

/* Removes a page's mapping; returns the frame that lay under it */
uint64_t paging64_unmap(uint64_t virt);
int paging64_discard(uint64_t virt, uint64_t len);

/* The physical address behind a virtual one (0 = not mapped) */
uint64_t paging64_lookup(uint64_t virt);

/* Has a page of a writable mapping been written to since the last
   flush to disk (the "dirty" bit set by the processor) */
int paging64_file_dirty(uint64_t virt);

/* Clears the "dirty" bit - called after a page has been flushed to
   the file */
void paging64_file_clean(uint64_t virt);

/* Is the whole range the program's to read (paging64_user_range) or to
   write into (paging64_user_writable)? For checking pointers from a
   program: the USER bit at every level of the tables, and for writing
   a writable page as well. Pages the program owns but has not touched
   yet are brought in on the way, so after a yes the kernel may use the
   whole range at once. */
int paging64_user_range(uint64_t virt, uint64_t len);
int paging64_user_writable(uint64_t virt, uint64_t len);

/* Is this an address ring 3 may run code at - the program's own, and
   not marked as data? For an entry point a program supplies. */
int paging64_user_code(uint64_t virt);

/* What a fault from ring 3 would do at this address of the current
   program: bring a page back from the swap file or from a mapped file,
   or give it a fresh one in its heap, its stack or its mapping area.
   1 = the page is there now, 0 = the address is none of those. It
   lives with the page fault handler, in kernel64/isr64.c; the two
   checks above call it for the pages a program has not touched yet. */
int isr64_page_in(uint64_t addr);

/* Process address spaces. Kernel memory is common to every one of
   them - only the program's gigabyte is private. */
uint64_t paging64_kernel_space(void);
uint64_t paging64_new_space(void);        /* 0 = it did not work */
uint64_t paging64_clone_space(uint64_t src);   /* a copy for fork() */

/* An attempted write to a page shared after a fork. Returns 1 when the
   page has been split and the program may try again. */
int paging64_handle_cow(uint64_t virt);

/* How many pages really had to be copied */
uint32_t paging64_cow_splits(void);
uint64_t paging64_current_space(void);
void paging64_switch(uint64_t space);
void paging64_free_space(uint64_t space);

/* Adds to the mapping an area lying outside memory (device
   registers). 0 = done. */
int paging64_map_phys(uint64_t phys, uint64_t len);

/* A pointer to physical memory outside the low gigabyte (ACPI tables):
   the address itself once mapped, or the direct map's view where the
   address is a program's. NULL when it cannot be reached. */
void *paging64_phys_view(uint64_t phys, uint64_t len);

/* --- Swapping pages out to disk -----------------------------------
 *
 * Throws at most 'want' program pages out of the given space; returns
 * how many it managed to hand over. Swap_in fetches back the one the
 * program has just tried to touch (1 = it is back, the instruction can
 * be repeated). */
int paging64_reclaim_pages(uint64_t space, int want);
int paging64_swap_in(uint64_t space, uint64_t virt);

/* How far the identity mapping of memory reaches */
uint64_t paging64_identity_top(void);

/* Is no-execute enabled (the NXE bit in EFER) */
int paging64_nx_enabled(void);
/* The page attribute table with entry 1 as write-combining (done on
   every core); whether the processor had one; a framebuffer mapped so */
void paging64_pat_init(void);
int paging64_fb_wc(void);
/* SMEP on this core, if the processor has it (1 when it was turned on);
   whether it is on */
int paging64_smep_init(void);
int paging64_smep(void);
int paging64_map_phys_wc(uint64_t phys, uint64_t len);
/* Firmware memory, identity-mapped into the kernel's own space (for
   UEFI runtime services) */
int paging64_firmware_map(uint64_t phys, uint64_t len, int exec,
                          int uncached);

#endif
