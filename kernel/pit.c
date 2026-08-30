/* PIT 8253/8254 - the programmable timer
 *
 * Channel 0 generates IRQ0 at a given frequency. This is the heart of
 * the future scheduler; for now it counts time and shows the uptime in
 * a corner of the screen.
 */

#include "include/pit.h"
#include "include/io.h"
#include "include/isr.h"
#include "include/pic.h"
#include "include/vga.h"
#include "include/task.h"

#define PIT_CHANNEL0 0x40
#define PIT_COMMAND  0x43
#define PIT_BASE_HZ  1193182u

static volatile uint32_t ticks;
static uint32_t frequency;

uint32_t pit_ticks(void)
{
    return ticks;
}

/* Draws "uptime: Ns" in the top right corner, once a second */
static void draw_uptime(uint32_t seconds)
{
    char buf[24] = "uptime: ";
    int i = 8;
    char tmp[11];
    int t = 0;

    do {
        tmp[t++] = '0' + seconds % 10;
        seconds /= 10;
    } while (seconds);
    while (t)
        buf[i++] = tmp[--t];
    buf[i++] = 's';
    buf[i] = '\0';

    vga_write_at(80 - i, 0, buf);
}

static void pit_irq(struct regs *r)
{
    (void)r;
    ticks++;
    if (ticks % frequency == 0)
        draw_uptime(ticks / frequency);

    /* Preemption: on every tick give the CPU to the next thread */
    schedule();
}

void pit_init(uint32_t frequency_hz)
{
    frequency = frequency_hz;
    uint32_t divisor = PIT_BASE_HZ / frequency_hz;

    /* channel 0, lobyte/hibyte, mode 3 (square wave) */
    outb(PIT_COMMAND, 0x36);
    outb(PIT_CHANNEL0, divisor & 0xFF);
    outb(PIT_CHANNEL0, (divisor >> 8) & 0xFF);

    irq_install_handler(0, pit_irq);
    pic_unmask(0);
}
