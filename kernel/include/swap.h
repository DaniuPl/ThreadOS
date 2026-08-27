#ifndef PROTONOS_SWAP_H
#define PROTONOS_SWAP_H

#include <stdint.h>

/* Wymiana stron na dysk
 *
 * Magazyn stron wyrzuconych z pamieci. Miejsce rezerwujemy przez system
 * plikow (plik 'swap.sys'), zeby 'df' i 'fls' mowily prawde i zeby nikt
 * nam po nim nie pisal - ale same strony wedruja tam i z powrotem
 * wprost przez ATA, z pominieciem kodu PFS. PFS ma jeden wspolny bufor
 * na blok, a wymiana potrafi ruszyc w srodku dowolnej operacji na
 * plikach; wchodzenie mu tam w droge skonczyloby sie polamanym dyskiem.
 */

/* Zaklada plik wymiany (po fs_init, w kontekscie watku). */
void swap_init(void);

/* Rezerwuje wolne miejsce na strone; -1 gdy nie ma. */
int swap_alloc_slot(void);
void swap_free_slot(int slot);

/* Przenosi 4 KiB miedzy ramka a dyskiem. 0 = OK, -1 = blad. */
int swap_write(int slot, const void *frame);
int swap_read(int slot, void *frame);

/* Statystyki do komendy 'mem' */
void swap_stats(uint32_t *used, uint32_t *total,
                uint32_t *out, uint32_t *in);

#endif
