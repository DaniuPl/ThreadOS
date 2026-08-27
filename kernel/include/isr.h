#ifndef PROTONOS_ISR_H
#define PROTONOS_ISR_H

#include <stdint.h>

/* Stan procesora zapisany przez stub przerwania.
   Kolejność pól odpowiada kolejności odkładania na stos.

   Tryb długi ma dwa razy więcej rejestrów i dwa razy szersze, a przy
   przerwaniu procesor odkłada stos użytkownika ZAWSZE, nie tylko przy
   zmianie poziomu uprawnień. Ramka jest więc inna - ale reszta jądra
   dotyka jej na tyle rzadko, że wystarczy tu jeden warunek zamiast
   osobnej wersji każdego pliku, który przerwania w ogóle widzi. */
#ifdef __x86_64__
struct regs {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rdx, rcx, rbx, rax;
    uint64_t int_no, err_code;
    uint64_t rip, cs, rflags, rsp, ss;   /* odkładane sprzętowo */
};
#else
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
#endif

/* IRQ 0-15 są mapowane na wektory 32-47 */
#define IRQ_BASE 32

typedef void (*irq_handler_t)(struct regs *r);

void irq_install_handler(int irq, irq_handler_t handler);

#endif
