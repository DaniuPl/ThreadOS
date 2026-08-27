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

/* Zwalnia jednego wlasciciela ramki; ramka wraca do puli dopiero,
   gdy byl ostatni (zob. pmm_frame_ref - kopiowanie przy zapisie) */
void pmm_free_frame(uint32_t addr);

/* Dopisuje wlasciciela ramki (strona wspoldzielona po fork) */
void pmm_frame_ref(uint32_t addr);

/* Liczba wlascicieli ramki (0 = wolna) */
uint32_t pmm_frame_refs(uint32_t addr);

uint32_t pmm_total_frames(void);
uint32_t pmm_used_frames(void);
uint32_t pmm_ram_kib(void);    /* cala wykryta pamiec RAM w KiB */

/* Adres tuz za ostatnia zarzadzana ramka - stad stronicowanie wie,
   dokad ciagnac mapowanie identycznosciowe */
uint32_t pmm_managed_top(void);

#endif
