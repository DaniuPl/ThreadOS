#ifndef PROTONOS_ISR_H
#define PROTONOS_ISR_H

#include <stdint.h>

/* Stan procesora zapisany przez stub przerwania (kernel/isr.S).
   Kolejność pól odpowiada kolejności odkładania na stos. */
struct regs {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax; /* pushal */
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags;       /* odkładane sprzętowo przez CPU */
    /* Przy wejściu z ring 3 CPU odkłada jeszcze stos użytkownika.
       Ważne tylko wtedy, gdy (cs & 3) == 3 - m.in. przy fork(), gdzie
       dziecko wraca do ring 3 z tą samą ramką co rodzic. */
    uint32_t useresp, ss;
};

/* IRQ 0-15 są mapowane na wektory 32-47 */
#define IRQ_BASE 32

typedef void (*irq_handler_t)(struct regs *r);

void irq_install_handler(int irq, irq_handler_t handler);

#endif
