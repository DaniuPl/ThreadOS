#ifndef PROTONOS_ISR_H
#define PROTONOS_ISR_H

#include <stdint.h>

/* Processor state saved by an interrupt stub.
   The field order matches the order in which things are pushed.

   Long mode has twice as many registers and twice as wide, and on an
   interrupt the processor pushes the user stack ALWAYS, not only on a
   change of privilege level. The frame is therefore different - but
   the rest of the kernel touches it rarely enough that one conditional
   here is enough, instead of a separate version of every file that
   sees interrupts at all. */
#ifdef __x86_64__
struct regs {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rdx, rcx, rbx, rax;
    uint64_t int_no, err_code;
    uint64_t rip, cs, rflags, rsp, ss;   /* pushed by hardware */
};
#else
struct regs {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax; /* pushal */
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags;       /* pushed by the CPU in hardware */
    /* On entry from ring 3 the CPU also pushes the user stack.
       Only meaningful when (cs & 3) == 3 - among others in fork(),
       where the child returns to ring 3 with the same frame as the
       parent. */
    uint32_t useresp, ss;
};
#endif

/* IRQ 0-15 are mapped onto vectors 32-47 */
#define IRQ_BASE 32

typedef void (*irq_handler_t)(struct regs *r);

void irq_install_handler(int irq, irq_handler_t handler);

#endif
