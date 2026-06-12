/* Wspólna obsługa przerwań - wołana ze stubów w kernel/isr.S */

#include <stddef.h>
#include "include/isr.h"
#include "include/pic.h"
#include "include/printk.h"

static const char *exception_names[32] = {
    "Divide Error", "Debug", "NMI", "Breakpoint",
    "Overflow", "BOUND Range Exceeded", "Invalid Opcode",
    "Device Not Available", "Double Fault", "Coprocessor Segment Overrun",
    "Invalid TSS", "Segment Not Present", "Stack-Segment Fault",
    "General Protection Fault", "Page Fault", "(zarezerwowany)",
    "x87 FPU Error", "Alignment Check", "Machine Check",
    "SIMD FP Exception", "Virtualization Exception", "Control Protection",
    "(zarezerwowany)", "(zarezerwowany)", "(zarezerwowany)",
    "(zarezerwowany)", "(zarezerwowany)", "(zarezerwowany)",
    "(zarezerwowany)", "(zarezerwowany)", "(zarezerwowany)",
    "(zarezerwowany)",
};

static irq_handler_t irq_handlers[16];

void irq_install_handler(int irq, irq_handler_t handler)
{
    irq_handlers[irq] = handler;
}

void isr_handler(struct regs *r)
{
    if (r->int_no < 32) {
        /* Breakpoint (int3) to wyjątek-pułapka: zgłoś i jedź dalej */
        if (r->int_no == 3) {
            printk("[INT3] breakpoint pod eip=%x\n", r->eip);
            return;
        }

        printk("\n!!! WYJATEK CPU %u: %s (err=%x) pod eip=%x\n",
               r->int_no, exception_names[r->int_no], r->err_code, r->eip);
        if (r->int_no == 14) {
            uint32_t cr2;
            __asm__ volatile("movl %%cr2, %0" : "=r"(cr2));
            printk("Page fault pod adresem (CR2): %x\n", cr2);
        }
        printk("System zatrzymany.\n");
        for (;;)
            __asm__ volatile("cli; hlt");
    }

    if (r->int_no >= IRQ_BASE && r->int_no < IRQ_BASE + 16) {
        int irq = r->int_no - IRQ_BASE;
        if (irq_handlers[irq])
            irq_handlers[irq](r);
        pic_send_eoi(irq);
    }
}
