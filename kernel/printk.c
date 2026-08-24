/* printk - formatowane wypisywanie jądra
 *
 * Każdy znak idzie jednocześnie na ekran VGA i port szeregowy,
 * więc komunikaty widać też przy uruchomieniu QEMU z -serial stdio.
 *
 * Cały wypis jest niepodzielny (przerwania wyłączone na czas jednego
 * printk). Bez tego komunikat z wątku - na przykład dzierżawa DHCP -
 * potrafił wejść w środek linii wypisywanej właśnie przez inny wątek
 * albo przez obsługę przerwania, i na ekranie zostawała sieczka.
 */

#include <stdarg.h>
#include <stdint.h>
#include "include/printk.h"
#include "include/vga.h"
#include "include/serial.h"
#include "include/io.h"

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

static void print_uint(uint32_t value, uint32_t base)
{
    char buf[11]; /* max 10 cyfr dziesiętnych + NUL */
    static const char digits[] = "0123456789abcdef";
    int i = sizeof(buf) - 1;

    buf[i] = '\0';
    do {
        buf[--i] = digits[value % base];
        value /= base;
    } while (value);
    kputs(&buf[i]);
}

void printk(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);

    uint32_t irq = irq_save();

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
            print_uint((uint32_t)va_arg(args, void *), 16);
            break;
        case '%':
            kputchar('%');
            break;
        case '\0':
            va_end(args);
            irq_restore(irq);
            return;
        default:
            kputchar('%');
            kputchar(*fmt);
        }
    }

    va_end(args);
    irq_restore(irq);
}
