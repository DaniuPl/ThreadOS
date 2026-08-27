/* Wspolna obsluga przerwan w trybie dlugim
 *
 * Rola ta sama, co w jadrze 32-bitowym: wyjatek procesora zatrzymuje
 * system z diagnostyka, a przerwanie sprzetowe trafia do handlera
 * zarejestrowanego przez sterownik. Rozni sie tylko ramka rejestrow
 * i to, ze adresy sa 64-bitowe.
 */

#include <stddef.h>
#include <stdint.h>
#include "../kernel/include/isr.h"
#include "../kernel/include/pic.h"
#include "../kernel/include/printk.h"
#include "../kernel/include/vga.h"

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
    if (irq >= 0 && irq < 16)
        irq_handlers[irq] = handler;
}

void isr64_handler(struct regs *r)
{
    if (r->int_no < 32) {
        /* Breakpoint to wyjatek-pulapka: zglos i jedz dalej */
        if (r->int_no == 3) {
            printk("[INT3] breakpoint pod rip=%x\n", (uint32_t)r->rip);
            return;
        }

        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        printk("\n!!! WYJATEK CPU %u: %s (err=%x)\n",
               (uint32_t)r->int_no, exception_names[r->int_no],
               (uint32_t)r->err_code);
        printk("rip=%x:%x rsp=%x:%x\n",
               (uint32_t)(r->rip >> 32), (uint32_t)r->rip,
               (uint32_t)(r->rsp >> 32), (uint32_t)r->rsp);
        if (r->int_no == 14) {
            uint64_t cr2;
            __asm__ volatile("movq %%cr2, %0" : "=r"(cr2));
            printk("Page fault pod adresem (CR2): %x:%x\n",
                   (uint32_t)(cr2 >> 32), (uint32_t)cr2);
        }
        printk("System zatrzymany.\n");
        for (;;)
            __asm__ volatile("cli; hlt");
    }

    if (r->int_no >= IRQ_BASE && r->int_no < IRQ_BASE + 16) {
        int irq = (int)(r->int_no - IRQ_BASE);
        /* EOI przed handlerem - tak samo jak w jadrze 32-bitowym */
        pic_send_eoi(irq);
        if (irq_handlers[irq])
            irq_handlers[irq](r);
    }
}
