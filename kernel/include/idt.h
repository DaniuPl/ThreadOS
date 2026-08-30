#ifndef PROTONOS_IDT_H
#define PROTONOS_IDT_H

void idt_init(void);

/* Laduje juz zbudowana tablice na biezacym rdzeniu (rdzenie wtorne) */
void idt_zaladuj(void);

#endif
