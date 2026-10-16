#ifndef THREADOS_USERMODE_H
#define THREADOS_USERMODE_H

#include <stdint.h>

/* Jump into ring 3 through iret. Never returns. */
void enter_ring3(uint32_t entry, uint32_t user_stack_top);

/* Return to ring 3 from a prepared register frame (fork - the child
   returns from the syscall where the parent did). Never returns. */
struct regs;
void enter_ring3_regs(const struct regs *r);

#endif
