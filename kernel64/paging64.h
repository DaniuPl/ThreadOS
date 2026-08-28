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

/* Bit 9 procesor zostawia do dyspozycji systemu. Znaczy u nas: strona
   jest wspoldzielona po fork-u i ma zostac rozdzielona przy pierwszej
   probie zapisu. */
#define PG_COW    0x200

/* Ktory wpis drugiego poziomu nalezy do programu. Jeden wpis to
   gigabajt, a programy leza od 1 GiB w gore - USER64_BASE
   w kernel64/elf64.h musi sie z tym zgadzac. */
#define PAGING64_USER_PDPT 1

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

/* Przestrzenie adresowe procesow. Pamiec jadra jest w kazdej z nich
   wspolna - prywatny jest tylko gigabajt programu. */
uint64_t paging64_kernel_space(void);
uint64_t paging64_new_space(void);        /* 0 = nie udalo sie */
uint64_t paging64_clone_space(uint64_t src);   /* kopia dla fork() */

/* Proba zapisu do strony wspoldzielonej po fork-u. Zwraca 1, gdy
   strona zostala rozdzielona i program moze sprobowac jeszcze raz. */
int paging64_handle_cow(uint64_t virt);

/* Ile stron naprawde trzeba bylo skopiowac */
uint32_t paging64_cow_splits(void);
uint64_t paging64_current_space(void);
void paging64_switch(uint64_t space);
void paging64_free_space(uint64_t space);

/* Dokad siega mapowanie identycznosciowe pamieci */
uint64_t paging64_identity_top(void);

/* Czy zakaz wykonywania jest wlaczony (bit NXE w EFER) */
int paging64_nx_enabled(void);

#endif
