#ifndef PROTONOS_PAGING_H
#define PROTONOS_PAGING_H

#include <stdint.h>

/* Początek przestrzeni użytkownika. Poniżej tego adresu jądro trzyma
   mapowanie identycznościowe całej pamięci fizycznej, którą zarządza -
   to więc ten adres, a nie ilość RAM-u, wyznacza granicę między
   światami. Program użytkownika linkuje się od niego (user/user.ld). */
#define PAGING_USER_BASE 0x40000000u        /* 1 GiB */

/* Ile pamięci fizycznej jądro potrafi objąć mapowaniem
   identycznościowym (musi zgadzać się z pmm.c i mieścić poniżej
   PAGING_USER_BASE) */
#define PAGING_MANAGED_MAX (512u * 1024 * 1024)

/* Pierwsze 64 MiB opisują tablice statyczne - jądro musi widzieć
   pamięć, zanim zacznie prosić PMM o ramki na dalsze tablice */
#define PAGING_STATIC_LIMIT (64u * 1024 * 1024)

/* Mapuje identycznościowo całą wykrytą pamięć (do PAGING_MANAGED_MAX)
   i włącza stronicowanie */
void paging_init(void);

/* Dokąd naprawdę sięga mapowanie identycznościowe (bajty) */
uint32_t paging_identity_limit(void);

/* Mapuje identycznościowo zakres pamieci fizycznej (np. framebuffer
   VESA lezacy wysoko, poza 64 MiB). Zwraca 0 albo -1. */
int paging_map_phys(uint32_t phys, uint32_t size);

/* CR3 jądra - przestrzeń adresowa wątków jądra */
uint32_t paging_kernel_cr3(void);

/* Przestrzenie adresowe procesów: katalog stron dzielący mapowania
   jądra (supervisor) + własne strony użytkownika powyżej 64 MiB */
uint32_t paging_new_address_space(void);
int paging_map_user_in(uint32_t dir_phys, uint32_t vaddr, uint32_t frame);

/* To samo, ale bez prawa zapisu - dla plikow odwzorowanych w pamieci */
int paging_map_user_ro_in(uint32_t dir_phys, uint32_t vaddr, uint32_t frame);
uint32_t paging_get_mapping(uint32_t dir_phys, uint32_t vaddr);
void paging_destroy_address_space(uint32_t dir_phys);

/* Klonuje przestrzen adresowa na potrzeby fork(): zadna strona procesu
   nie jest kopiowana - obie przestrzenie dostaja te same ramki bez
   prawa zapisu, a kopia powstaje dopiero przy pierwszym zapisie
   (kopiowanie przy zapisie). Zwraca CR3 kopii albo 0. */
uint32_t paging_clone_address_space(uint32_t src_phys);

/* Wyrzuca na dysk do 'want' nieuzywanych stron z podanej przestrzeni
   adresowej (algorytm drugiej szansy). Zwraca, ile zwolnil. */
int paging_reclaim_pages(uint32_t dir_phys, int want);

/* Sciaga z dysku strone, po ktora proces wlasnie siegnal.
   1 = wrocila, mozna wznowic instrukcje. */
int paging_swap_in(uint32_t dir_phys, uint32_t vaddr);

/* Obsluga page faulta na stronie oddanej na kopiowanie przy zapisie.
   Zwraca 1, gdy blad zostal zalatwiony (mozna wznowic instrukcje). */
int paging_handle_cow(uint32_t vaddr, uint32_t err_code);

#endif
