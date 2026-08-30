#ifndef PROTONOS_IDT64_H
#define PROTONOS_IDT64_H

/* Laduje tablice przerwan trybu dlugiego (wyjatki 0-31, IRQ 32-47) */
void idt64_init(void);

/* Laduje juz zbudowana tablice na biezacym rdzeniu (rdzenie wtorne) */
void idt64_load(void);

#endif
