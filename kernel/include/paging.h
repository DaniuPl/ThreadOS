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

/* Klonuje przestrzen adresowa na potrzeby fork(): zadna strona procesu
   nie jest kopiowana - obie przestrzenie dostaja te same ramki bez
   prawa zapisu, a kopia powstaje dopiero przy pierwszym zapisie
   (kopiowanie przy zapisie). Zwraca CR3 kopii albo 0. */
uint32_t paging_clone_address_space(uint32_t src_phys);

/* Obsluga page faulta na stronie oddanej na kopiowanie przy zapisie.
   Zwraca 1, gdy blad zostal zalatwiony (mozna wznowic instrukcje). */
int paging_handle_cow(uint32_t vaddr, uint32_t err_code);

#endif
