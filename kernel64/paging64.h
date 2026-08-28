#ifndef PROTONOS_PAGING64_H
#define PROTONOS_PAGING64_H

#include <stdint.h>

/* Stronicowanie czteropoziomowe (tryb dlugi)
 *
 * Adres wirtualny rozklada sie na cztery indeksy po dziewiec bitow
 * plus przesuniecie w stronie: PML4 -> PDPT -> PD -> PT -> strona.
 * Kazdy wpis ma 64 bity, wiec w ramce 4 KiB miesci sie ich 512.
 *
 * Tablice ulozone w obrazie jadra przez boot64.S wystarczyly, zeby
 * w ogole ruszyc. Teraz budujemy wlasne, z ramek od PMM - dopiero one
 * pozwalaja mapowac cokolwiek po starcie.
 */

#define PG_WRITE  0x002
#define PG_USER   0x004
#define PG_NX     (1ULL << 63)      /* zakaz wykonywania kodu */

/* Buduje wlasne tablice i przelacza na nie procesor. 0 = gotowe. */
int paging64_init(void);

/* Mapuje pojedyncza strone 4 KiB. 0 = zmapowana. */
int paging64_map(uint64_t virt, uint64_t phys, uint64_t flags);

/* Zdejmuje mapowanie strony; zwraca ramke, ktora pod nia lezala */
uint64_t paging64_unmap(uint64_t virt);

/* Adres fizyczny spod adresu wirtualnego (0 = nie zmapowany) */
uint64_t paging64_lookup(uint64_t virt);

/* Czy caly zakres jest dostepny dla ring 3 (bit USER na kazdym
   poziomie tablic)? Do sprawdzania wskaznikow z programu. */
int paging64_user_range(uint64_t virt, uint64_t len);

/* Dokad siega mapowanie identycznosciowe pamieci */
uint64_t paging64_identity_top(void);

/* Czy zakaz wykonywania jest wlaczony (bit NXE w EFER) */
int paging64_nx_enabled(void);

#endif
