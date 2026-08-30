#ifndef PROTONOS_GDT_H
#define PROTONOS_GDT_H

#include <stdint.h>

/* Segment selectors (index in the GDT * 8) */
#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_CODE   0x18
#define GDT_USER_DATA   0x20
#define GDT_TSS         0x28

void gdt_init(void);

/* Loads the already-built table on the current core (secondary cores) */
void gdt_load(void);

/* Loads the task state segment BELONGING TO THIS CORE. Each has its
   own: the processor reads the kernel stack from it on entry from
   ring 3, and every core enters there from a different task. */
void gdt_load_tss(void);

/* Sets the kernel stack the CPU will jump to on an interrupt from ring 3 */
void tss_set_esp0(uint32_t esp0);

#endif
