/* Wspólna obsługa przerwań - wołana ze stubów w kernel/isr.S */

#include <stddef.h>
#include "include/isr.h"
#include "include/paging.h"
#include "include/pic.h"
#include "include/printk.h"
#include "include/syscall.h"
#include "include/task.h"

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
    if (r->int_no == SYSCALL_VECTOR) {
        syscall_handler(r);
        task_kill_pending();    /* dorecz zabicie takze przy powrocie z syscalla */
        return;
    }

    if (r->int_no < 32) {
        /* Page fault na stronie oddanej na kopiowanie przy zapisie
           (fork) nie jest bledem - zrob prywatna kopie i wznow
           instrukcje. Dotyczy tez zapisow jadra (CR0.WP), np. gdy
           syscall wypelnia bufor swiezo rozwidlonego procesu. */
        if (r->int_no == 14) {
            uint32_t cr2;
            __asm__ volatile("movl %%cr2, %0" : "=r"(cr2));
            if (paging_handle_cow(cr2, r->err_code))
                return;
        }

        /* Breakpoint (int3) to wyjątek-pułapka: zgłoś i jedź dalej */
        if (r->int_no == 3) {
            printk("[INT3] breakpoint pod eip=%x\n", r->eip);
            return;
        }

        /* Wyjątek w ring 3: ginie tylko proces użytkownika, nie system */
        if ((r->cs & 3) == 3) {
            printk("\n[jadro] proces '%s' ubity: %s (err=%x, eip=%x)\n",
                   task_current_name(), exception_names[r->int_no],
                   r->err_code, r->eip);
            if (r->int_no == 14) {
                uint32_t cr2;
                __asm__ volatile("movl %%cr2, %0" : "=r"(cr2));
                printk("[jadro] proces siegnal po adres %x\n", cr2);
            }
            task_exit(-1);          /* ubity przez wyjatek CPU */
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
        /* EOI przed handlerem: handler timera przelacza konteksty,
           wiec do "konca" tego przerwania moze dojsc dopiero, gdy
           watek znow dostanie procesor - PIC nie moze na to czekac.
           Bramki przerwan trzymaja IF=0, wiec zagniezdzenie nie grozi. */
        pic_send_eoi(irq);
        if (irq_handlers[irq])
            irq_handlers[irq](r);
    }

    /* Przy powrocie do ring 3 dorecz oczekujace zabicie (kill) -
       proces ginie tu, w punkcie powrotu do trybu uzytkownika */
    if ((r->cs & 3) == 3)
        task_kill_pending();
}
