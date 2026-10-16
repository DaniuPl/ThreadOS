#ifndef THREADOS_GDT64_H
#define THREADOS_GDT64_H

#include <stdint.h>

/* Selectors. The order is not arbitrary: the sysret instruction builds
   the user selectors out of a single value in the STAR register - data
   comes from it plus eight, and code plus sixteen. So the user data
   segment has to lie BEFORE its code segment. */
#define GDT64_KERNEL_CODE 0x08
#define GDT64_KERNEL_DATA 0x10
#define GDT64_USER_DATA   0x18
#define GDT64_USER_CODE   0x20
#define GDT64_TSS         0x28

/* Loads the descriptor table with the ring 3 segments and the TSS */
void gdt64_init(void);

/* Loads an already built table on the current core (secondary cores) */
void gdt64_load(void);

/* Loads the task state segment BELONGING TO THIS CORE */
void gdt64_load_tss(void);

/* The kernel stack the processor moves to on an interrupt from
   ring 3 */
void gdt64_set_kernel_stack(uint64_t rsp);

#endif
