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

/* The physical address behind a virtual one (0 = not mapped) */
uint64_t paging64_lookup(uint64_t virt);

/* Has a page of a writable mapping been written to since the last
   flush to disk (the "dirty" bit set by the processor) */
int paging64_file_dirty(uint64_t virt);

/* Clears the "dirty" bit - called after a page has been flushed to
   the file */
void paging64_file_clean(uint64_t virt);

/* Is the whole range accessible to ring 3 (the USER bit at every level
   of the tables)? For checking pointers from a program. */
int paging64_user_range(uint64_t virt, uint64_t len);

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
int paging64_map_phys_wc(uint64_t phys, uint64_t len);
/* Firmware memory, identity-mapped into the kernel's own space (for
   UEFI runtime services) */
int paging64_firmware_map(uint64_t phys, uint64_t len, int exec,
                          int uncached);

#endif
