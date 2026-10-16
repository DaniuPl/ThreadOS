#ifndef THREADOS_PIC_H
#define THREADOS_PIC_H

#include <stdint.h>

/* Remaps the PIC: IRQ 0-15 onto vectors 32-47 and masks all IRQs */
void pic_init(void);

void pic_send_eoi(uint8_t irq);
void pic_unmask(uint8_t irq);
void pic_mask(uint8_t irq);

#endif
