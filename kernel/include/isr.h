#ifndef THREADOS_ISR_H
#define THREADOS_ISR_H

#include <stdint.h>

/* Processor state saved by an interrupt stub.
   The field order matches the order in which things are pushed.

   Long mode has twice as many registers and twice as wide, and on an
   interrupt the processor pushes the user stack ALWAYS, not only on a
   change of privilege level. The frame is therefore different - but
   the rest of the kernel touches it rarely enough that one conditional
   here is enough, instead of a separate version of every file that
   sees interrupts at all. */
struct regs {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rdx, rcx, rbx, rax;
    uint64_t int_no, err_code;
    uint64_t rip, cs, rflags, rsp, ss;   /* pushed by hardware */
};

/* IRQ 0-15 are mapped onto vectors 32-47 */
#define IRQ_BASE 32

typedef void (*irq_handler_t)(struct regs *r);

void irq_install_handler(int irq, irq_handler_t handler);

/* How many page faults were served without the big kernel lock, and how
   many had to take it. The commonest way into the kernel there is - so
   the question of whether it still closes the kernel for everybody
   deserves an answer that is not a stopwatch. */
uint32_t isr_faults_nolock(void);
uint32_t isr_faults_locked(void);

#endif
