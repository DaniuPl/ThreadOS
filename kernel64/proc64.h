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

/* Ile ramek kosztowal ostatni fork - z dzieleniem stron sa to same
   tablice stron, bez ani jednej strony pamieci programu */
uint32_t proc64_last_fork_cost(void);

/* Podmienia obraz procesu na program z initrd. Przy powodzeniu nie
   ma do czego wrocic - ramka 'r' opisuje juz nowy program. */
int64_t proc64_exec(struct regs *r, const char *name, uint64_t len);

#endif
