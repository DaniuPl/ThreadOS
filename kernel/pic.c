/* PIC 8259 - the hardware interrupt controller
 *
 * After startup the BIOS maps IRQ 0-7 onto vectors 8-15, which collide
 * with CPU exceptions in protected mode. We remap them onto 32-47.
 */

#include "include/pic.h"
#include "include/io.h"

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

#define PIC_EOI   0x20

void pic_init(void)
{
    /* ICW1: initialisation, an ICW4 will follow */
    outb(PIC1_CMD, 0x11); io_wait();
    outb(PIC2_CMD, 0x11); io_wait();
    /* ICW2: vector offsets */
    outb(PIC1_DATA, 32); io_wait();
    outb(PIC2_DATA, 40); io_wait();
    /* ICW3: the slave on the master's IRQ2 line */
    outb(PIC1_DATA, 0x04); io_wait();
    outb(PIC2_DATA, 0x02); io_wait();
    /* ICW4: 8086 mode */
    outb(PIC1_DATA, 0x01); io_wait();
    outb(PIC2_DATA, 0x01); io_wait();

    /* Mask all IRQs - the drivers will unmask their own */
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

void pic_send_eoi(uint8_t irq)
{
    if (irq >= 8)
        outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);
}

void pic_unmask(uint8_t irq)
{
    uint16_t port = irq < 8 ? PIC1_DATA : PIC2_DATA;
    uint8_t bit = irq < 8 ? irq : irq - 8;
    outb(port, inb(port) & ~(1 << bit));
    /* Slave IRQs need the cascade line (IRQ2) open on the master */
    if (irq >= 8)
        outb(PIC1_DATA, inb(PIC1_DATA) & ~(1 << 2));
}

void pic_mask(uint8_t irq)
{
    uint16_t port = irq < 8 ? PIC1_DATA : PIC2_DATA;
    uint8_t bit = irq < 8 ? irq : irq - 8;
    outb(port, inb(port) | (1 << bit));
}
