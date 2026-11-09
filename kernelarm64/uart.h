#ifndef THREADOS_ARM64_UART_H
#define THREADOS_ARM64_UART_H

#include <stdint.h>
#include "../kernel/include/printk.h"

/* The PL011 serial port of QEMU's virt machine - for now the only way
   this kernel says anything. printk (kernel/include/printk.h) is
   defined in uart.c on top of it, so the shared code that calls printk
   builds here unchanged; it takes %s %c %d %u %x %p and, for 64-bit
   values, %lu %lx. */
void uart_putc(char c);
void uart_puts(const char *s);
/* From now on printk takes a lock: called once the MMU is on */
void uart_lock_output(void);

#endif
