#ifndef PROTONOS_USERMODE_H
#define PROTONOS_USERMODE_H

#include <stdint.h>

/* Skok do ring 3 przez iret. Nie wraca. */
void enter_ring3(uint32_t entry, uint32_t user_stack_top);

#endif
