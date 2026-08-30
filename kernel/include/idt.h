#ifndef PROTONOS_IDT_H
#define PROTONOS_IDT_H

void idt_init(void);

/* Loads the already-built table on the current core (secondary cores) */
void idt_load(void);

#endif
