#ifndef THREADOS_FPU64_H
#define THREADOS_FPU64_H

#include <stdint.h>

/* The floating-point unit and the SSE registers
 *
 * On x86-64 SSE is not an add-on but part of the calling convention -
 * the compiler uses it without asking, because every processor in this
 * mode has it. The processor, however, starts with its support turned
 * off and every such instruction ends in an exception until the system
 * says outright that it can handle these registers. "Can" means one
 * thing here: it is able to save and restore them across a task
 * switch.
 */

/* Enables SSE in the control registers. 1 = enabled. */
int fpu64_init(void);

/* Enables SSE on THIS core. CR0 and CR4 belong to the core, so every
   one has to be told separately - a core that is not told answers an
   ordinary XMM instruction with an invalid opcode. */
void fpu64_enable_core(void);

/* Room for one task's state (512 B, aligned to 16). Returns NULL when
   out of memory. */
void *fpu64_alloc(void);
void fpu64_free(void *area);

/* Saves the current task's state and loads the next one's */
void fpu64_save(void *area);
void fpu64_load(const void *area);

int fpu64_enabled(void);

#endif
