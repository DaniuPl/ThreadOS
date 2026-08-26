#ifndef PROTONOS_PIPE_H
#define PROTONOS_PIPE_H

#include <stdint.h>

/* Potoki miedzyprocesowe: bufor kolowy w jadrze laczacy stdout
   producenta ze stdin konsumenta. Read blokuje na pustym potoku
   dopoki sa pisarze; po ich zamknieciu zwraca 0 (EOF). */

int  pipe_alloc(void);                       /* nowy potok (1 czytelnik, 1 pisarz) */
int  pipe_read(int idx, void *buf, uint32_t len);
int  pipe_write(int idx, const void *buf, uint32_t len);
void pipe_add_reader(int idx);               /* fork: jeszcze jeden czytelnik */
void pipe_add_writer(int idx);               /* fork: jeszcze jeden pisarz */
void pipe_close_reader(int idx);
void pipe_close_writer(int idx);

#endif
