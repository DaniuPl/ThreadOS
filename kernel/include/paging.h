#ifndef THREADOS_PAGING_H
#define THREADOS_PAGING_H

#include <stdint.h>

/* Start of user space. Below this address the kernel keeps an identity
   mapping of all the physical memory it manages - so it is this
   address, and not the amount of RAM, that marks the border between
   the two worlds. A user program is linked from it (user/user.ld). */
#define PAGING_USER_BASE 0x40000000u        /* 1 GiB */

/* How much physical memory the kernel can cover with the identity
   mapping (must agree with pmm.c and fit below PAGING_USER_BASE) */
#define PAGING_MANAGED_MAX (512u * 1024 * 1024)

/* The first 64 MiB are described by static tables - the kernel has to
   see memory before it can ask the PMM for frames for further tables */
#define PAGING_STATIC_LIMIT (64u * 1024 * 1024)

/* Identity-maps all detected memory (up to PAGING_MANAGED_MAX) and
   turns paging on */
void paging_init(void);

/* How far the identity mapping really reaches (bytes) */
uint32_t paging_identity_limit(void);

/* Identity-maps a range of physical memory (e.g. a VESA framebuffer
   sitting high, beyond 64 MiB). Returns 0 or -1. */
int paging_map_phys(uint32_t phys, uint32_t size);

/* The kernel's CR3 - the address space of kernel threads */
uint32_t paging_kernel_cr3(void);

/* Process address spaces: a page directory sharing the kernel's
   (supervisor) mappings plus its own user pages above 64 MiB */
uint32_t paging_new_address_space(void);
int paging_map_user_in(uint32_t dir_phys, uint32_t vaddr, uint32_t frame);

/* The same, but without write permission - for memory-mapped files */
int paging_map_user_ro_in(uint32_t dir_phys, uint32_t vaddr, uint32_t frame);

/* A page of a writable file mapping: writable, but marked so that fork
   shares it instead of copying, and so that swapping leaves it alone. */
int paging_map_user_file_in(uint32_t dir_phys, uint32_t vaddr, uint32_t frame);

/* Has such a page been written since it was last flushed to disk
   (the "dirty" bit set by the processor) */
int paging_file_dirty(uint32_t dir_phys, uint32_t vaddr);

/* Clears the "dirty" bit - called after a page is written back to its file */
void paging_file_clean(uint32_t dir_phys, uint32_t vaddr);
uint32_t paging_get_mapping(uint32_t dir_phys, uint32_t vaddr);

/* Maps only if there is nothing at that address yet. Returns 0 when
   somebody was there first - two cores can fault on the same page of
   one address space at the same moment, and the loser has to learn
   that it lost. */
int paging_map_user_if_absent(uint32_t dir_phys, uint32_t vaddr,
                              uint32_t frame);

/* Is there nothing at all at that address? Stricter than 'not
   present': a page evicted to disk is absent too, and filling it with
   zeros would lose what is on the disk. */
int paging_entry_empty(uint32_t dir_phys, uint32_t vaddr);
void paging_destroy_address_space(uint32_t dir_phys);

/* Clones an address space for fork(): not one page of the process is
   copied - both spaces get the same frames without write permission,
   and a copy appears only on the first write (copy-on-write).
   Returns the CR3 of the clone or 0. */
uint32_t paging_clone_address_space(uint32_t src_phys);

/* Evicts up to 'want' unused pages of the given address space to disk
   (second-chance algorithm). Returns how many it freed. */
int paging_reclaim_pages(uint32_t dir_phys, int want);

/* Pulls back from disk the page the process has just reached for.
   1 = it is back, the instruction can be resumed. */
int paging_swap_in(uint32_t dir_phys, uint32_t vaddr);

/* Handling of a page fault on a page given over to copy-on-write.
   Returns 1 when the fault was settled (the instruction can resume). */
int paging_handle_cow(uint32_t vaddr, uint32_t err_code);

#endif
