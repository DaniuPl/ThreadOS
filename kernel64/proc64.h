#ifndef PROTONOS_PROC64_H
#define PROTONOS_PROC64_H

#include <stdint.h>

struct regs;

/* Powiela biezacy proces.
 *
 * W rodzicu zwraca numer zadania dziecka, w dziecku - zero. Dziecko
 * dostaje kopie calej przestrzeni adresowej i kopie ramki rejestrow,
 * wiec wraca do ring 3 tam, gdzie rodzic, i z tym samym stanem.
 * Ujemna wartosc znaczy, ze sie nie udalo. */
int64_t proc64_fork(struct regs *r);

#endif
