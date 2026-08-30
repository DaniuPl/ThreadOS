#ifndef PROTONOS_PIPE64_H
#define PROTONOS_PIPE64_H

#include <stdint.h>

/* Potoki - bufor miedzy dwoma procesami
 *
 * Potok nie jest plikiem i nie ma nic wspolnego z dyskiem: to pierscien
 * w pamieci jadra plus dwa liczniki wlascicieli. Cala jego tresc bierze
 * sie z tego, kto go jeszcze trzyma - gdy zniknie ostatni piszacy,
 * czytajacy dostaje koniec danych zamiast czekac w nieskonczonosc.
 */

int pipe64_new(void);                    /* numer potoku albo -1 */
void pipe64_ref(int idx, int reader_task);
void pipe64_unref(int idx, int reader_task);

/* Blokuja, ale daja sie przerwac sygnalem (-1). Odczyt zwraca 0, gdy
   nie ma juz kto pisac - to koniec danych, nie blad. */
int pipe64_read(int idx, char *buf, uint32_t len);
int pipe64_write(int idx, const char *buf, uint32_t len);

#endif
