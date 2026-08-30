/* printk - formatted kernel printing
 *
 * Every character goes to the VGA screen and to the serial port at the
 * same time, so the messages are also visible when QEMU is started
 * with -serial stdio.
 *
 * The whole print is atomic. Without that a message from a thread -
 * a DHCP lease, for instance - could land in the middle of a line
 * being printed by another thread or by an interrupt handler, and what
 * was left on the screen was a mess.
 *
 * Since there is more than one core, turning interrupts off is no
 * longer enough: two cores really do write at the same time and
 * interleave character by character. The console is one, so the lock
 * is one too.
 */

#include <stdarg.h>
#include <stdint.h>
#include "include/printk.h"
#include "include/vga.h"
#include "include/serial.h"
#include "include/io.h"
#include "include/spinlock.h"

static void kputchar(char c)
{
    vga_putchar(c);
    serial_putchar(c);
}

static void kputs(const char *s)
{
    while (*s)
        kputchar(*s++);
}

static void print_uint(uintptr_t value, uint32_t base)
{
    char buf[11]; /* at most 10 decimal digits plus a NUL */
    static const char digits[] = "0123456789abcdef";
    int i = sizeof(buf) - 1;

    buf[i] = '\0';
    do {
        buf[--i] = digits[value % base];
        value /= base;
    } while (value);
    kputs(&buf[i]);
}

static struct spinlock lock = SPINLOCK_FREE;

void printk(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);

    irqflags_t irq = spin_lock_irq(&lock);

    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            kputchar(*fmt);
            continue;
        }
        switch (*++fmt) {
        case 's':
            kputs(va_arg(args, const char *));
            break;
        case 'c':
            kputchar((char)va_arg(args, int));
            break;
        case 'd': {
            int v = va_arg(args, int);
            if (v < 0) {
                kputchar('-');
                v = -v;
            }
            print_uint((uint32_t)v, 10);
            break;
        }
        case 'u':
            print_uint(va_arg(args, uint32_t), 10);
            break;
        case 'x':
            print_uint(va_arg(args, uint32_t), 16);
            break;
        case 'p':
            kputs("0x");
            print_uint((uintptr_t)va_arg(args, void *), 16);
            break;
        case '%':
            kputchar('%');
            break;
        case '\0':
            va_end(args);
            spin_unlock_irq(&lock, irq);
            return;
        default:
            kputchar('%');
            kputchar(*fmt);
        }
    }

    va_end(args);
    spin_unlock_irq(&lock, irq);
}
