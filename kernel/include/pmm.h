#ifndef PROTONOS_PMM_H
#define PROTONOS_PMM_H

#include <stdint.h>
#include "multiboot.h"

#define PAGE_SIZE 4096u

/* Buduje bitmapę ramek na podstawie mapy pamięci Multiboot */
void pmm_init(const struct multiboot_info *mbi);

/* Zwraca adres fizyczny wolnej ramki 4 KiB (0 = brak pamięci) */
uint32_t pmm_alloc_frame(void);

/* Zwraca adres pierwszej z n kolejnych wolnych ramek (0 = brak) */
uint32_t pmm_alloc_contiguous(uint32_t n);

void pmm_free_frame(uint32_t addr);

uint32_t pmm_total_frames(void);
uint32_t pmm_used_frames(void);
uint32_t pmm_ram_kib(void);    /* cala wykryta pamiec RAM w KiB */

#endif
