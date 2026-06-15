#ifndef PROTONOS_PAGING_H
#define PROTONOS_PAGING_H

/* Granica mapowania identycznościowego (musi zgadzać się z pmm.c) */
#define PAGING_IDENTITY_LIMIT (64u * 1024 * 1024)

#include <stdint.h>

/* Mapuje identycznościowo pierwsze 64 MiB i włącza stronicowanie */
void paging_init(void);

/* Mapuje identycznościowo zakres pamieci fizycznej (np. framebuffer
   VESA lezacy wysoko, poza 64 MiB). Zwraca 0 albo -1. */
int paging_map_phys(uint32_t phys, uint32_t size);

/* CR3 jądra - przestrzeń adresowa wątków jądra */
uint32_t paging_kernel_cr3(void);

/* Przestrzenie adresowe procesów: katalog stron dzielący mapowania
   jądra (supervisor) + własne strony użytkownika powyżej 64 MiB */
uint32_t paging_new_address_space(void);
int paging_map_user_in(uint32_t dir_phys, uint32_t vaddr, uint32_t frame);
uint32_t paging_get_mapping(uint32_t dir_phys, uint32_t vaddr);
void paging_destroy_address_space(uint32_t dir_phys);

#endif
