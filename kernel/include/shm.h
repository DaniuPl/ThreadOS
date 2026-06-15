#ifndef PROTONOS_SHM_H
#define PROTONOS_SHM_H

#include <stdint.h>

/* Pamiec wspoldzielona miedzy procesami, identyfikowana kluczem.
   Pierwsze uzycie klucza alokuje ramki fizyczne; kolejne (z dowolnego
   procesu) wskazuja te same ramki. */

/* Zwraca fizyczny adres bazowy regionu dla klucza (alokuje przy
   pierwszym uzyciu); 0 przy bledzie. */
uint32_t shm_get(int key, uint32_t size);

/* Czy dana ramka nalezy do regionu wspoldzielonego (destroy jej nie
   zwalnia, dopoki moga jej uzywac inne procesy). */
int shm_is_shared(uint32_t frame);

#endif
