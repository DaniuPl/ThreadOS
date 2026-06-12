#ifndef PROTONOS_SERIAL_H
#define PROTONOS_SERIAL_H

void serial_init(void);
void serial_putchar(char c);
void serial_write(const char *s);

#endif
