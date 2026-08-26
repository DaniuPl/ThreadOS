#ifndef PROTONOS_USERMODE_H
#define PROTONOS_USERMODE_H

#include <stdint.h>

/* Skok do ring 3 przez iret. Nie wraca. */
void enter_ring3(uint32_t entry, uint32_t user_stack_top);

/* Powrót do ring 3 z gotowej ramki rejestrów (fork - dziecko wraca
   z syscalla tam, gdzie rodzic). Nie wraca. */
struct regs;
void enter_ring3_regs(const struct regs *r);

#endif
