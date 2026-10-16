#ifndef THREADOS_PRINTK_H
#define THREADOS_PRINTK_H

/* Formatted kernel printing - to the VGA screen and the serial port.
   Supports: %s %c %d %u %x %p %% */
void printk(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
