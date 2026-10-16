#ifndef THREADOS_IDT64_H
#define THREADOS_IDT64_H

/* Loads the long mode interrupt table (exceptions 0-31, IRQ 32-47) */
void idt64_init(void);

/* Loads an already built table on the current core (secondary cores) */
void idt64_load(void);

#endif
