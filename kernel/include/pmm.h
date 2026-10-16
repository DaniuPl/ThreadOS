#ifndef THREADOS_PMM_H
#define THREADOS_PMM_H

#include <stdint.h>
#include "multiboot.h"

#define PAGE_SIZE 4096u

/* Builds the frame bitmap from the Multiboot memory map */
void pmm_init(const struct multiboot_info *mbi);

/* Returns the physical address of a free 4 KiB frame (0 = out of memory) */
uint32_t pmm_alloc_frame(void);

/* Whom to ask for memory when there are no free frames. The function
   must free at least one frame and return 1; 0 means "there is nothing
   left to give back". Set by the swapper (swap_init). */
void pmm_set_reclaim(int (*fn)(void));

/* Returns the address of the first of n consecutive free frames (0 = none) */
uint32_t pmm_alloc_contiguous(uint32_t n);

/* Releases one owner of a frame; the frame goes back to the pool only
   when it was the last one (see pmm_frame_ref - copy-on-write) */
void pmm_free_frame(uint32_t addr);

/* Adds an owner to a frame (a page shared after fork) */
void pmm_frame_ref(uint32_t addr);

/* Number of owners of a frame (0 = free) */
uint32_t pmm_frame_refs(uint32_t addr);

uint32_t pmm_total_frames(void);
uint32_t pmm_used_frames(void);
uint32_t pmm_ram_kib(void);    /* all detected RAM in KiB */

/* The address just past the last managed frame - from this paging
   knows how far to stretch the identity mapping */
uint32_t pmm_managed_top(void);

#endif
