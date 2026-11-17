/* The PL011 UART at 0x09000000
 *
 * On x86 the serial port is reached through I/O ports; ARM has no such
 * thing, and a device is a range of addresses like any other memory.
 * Writing a byte to the data register sends it; the flag register
 * says whether the transmit queue is full. QEMU's PL011 needs no setup
 * to do this - the baud rate is a fiction on a virtual wire - and real
 * boards leave the console UART set up by their firmware.
 *
 * The address is virt's, written here rather than read from the device
 * tree, because this is what has to work before anything else does -
 * including reading the device tree.
 */

#include <stdarg.h>
#include <stdint.h>
#include "uart.h"
#include "../kernel/include/spinlock.h"

#define UART_BASE 0x09000000UL
#define UART_DR   0x00
#define UART_FR   0x18
#define FR_TXFF   (1u << 5)

static inline uint32_t reg_read(unsigned off)
{
    return *(volatile uint32_t *)(UART_BASE + off);
}

static inline void reg_write(unsigned off, uint32_t v)
{
    *(volatile uint32_t *)(UART_BASE + off) = v;
}

void uart_putc(char c)
{
    /* A terminal reading this wants a carriage return as well */
    if (c == '\n')
        uart_putc('\r');
    while (reg_read(UART_FR) & FR_TXFF)
        ;
    reg_write(UART_DR, (uint8_t)c);
}

void uart_puts(const char *s)
{
    while (*s)
        uart_putc(*s++);
}

static void put_uint(uint64_t v, unsigned base)
{
    char buf[21];
    int i = sizeof(buf) - 1;
    buf[i] = '\0';
    do {
        buf[--i] = "0123456789abcdef"[v % base];
        v /= base;
    } while (v);
    uart_puts(&buf[i]);
}

static void format(const char *fmt, va_list ap)
{
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            uart_putc(*fmt);
            continue;
        }
        int is_long = 0;
        if (fmt[1] == 'l') {
            is_long = 1;
            fmt++;
        }
        switch (*++fmt) {
        case 's':
            uart_puts(va_arg(ap, const char *));
            break;
        case 'c':
            uart_putc((char)va_arg(ap, int));
            break;
        case 'd': {
            int64_t v = is_long ? va_arg(ap, int64_t) : va_arg(ap, int);
            if (v < 0) {
                uart_putc('-');
                v = -v;
            }
            put_uint((uint64_t)v, 10);
            break;
        }
        case 'u':
            put_uint(is_long ? va_arg(ap, uint64_t) : va_arg(ap, uint32_t), 10);
            break;
        case 'x':
            put_uint(is_long ? va_arg(ap, uint64_t) : va_arg(ap, uint32_t), 16);
            break;
        case 'p':
            uart_puts("0x");
            put_uint((uintptr_t)va_arg(ap, void *), 16);
            break;
        case '%':
            uart_putc('%');
            break;
        case '\0':
            return;
        default:
            uart_putc('%');
            uart_putc(*fmt);
        }
    }
}

/* Two cores printing at once would interleave their lines letter by
   letter. But the lock is an exclusive load and store, which is only
   promised to work once the MMU is on - so until then, while there is
   one core anyway, it is not taken. */
static struct spinlock print_lock = SPINLOCK_FREE;
static int print_locking;

void uart_lock_output(void)
{
    print_locking = 1;
}

void printk(const char *fmt, ...)
{
    irqflags_t flags = 0;
    if (print_locking)
        flags = spin_lock_irq(&print_lock);
    va_list ap;
    va_start(ap, fmt);
    format(fmt, ap);
    va_end(ap);
    if (print_locking)
        spin_unlock_irq(&print_lock, flags);
}

/* printk goes to the serial port alone on this kernel: there is no
   console on the screen for the loading screen to turn off */
void printk_screen_output(int enabled)
{
    (void)enabled;
}
