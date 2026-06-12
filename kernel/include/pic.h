#ifndef PROTONOS_PIC_H
#define PROTONOS_PIC_H

#include <stdint.h>

/* Remapuje PIC: IRQ 0-15 na wektory 32-47 i maskuje wszystkie IRQ */
void pic_init(void);

void pic_send_eoi(uint8_t irq);
void pic_unmask(uint8_t irq);
void pic_mask(uint8_t irq);

#endif
