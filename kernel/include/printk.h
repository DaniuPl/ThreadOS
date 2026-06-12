#ifndef PROTONOS_PRINTK_H
#define PROTONOS_PRINTK_H

/* Formatowane wypisywanie jądra - na ekran VGA i port szeregowy.
   Obsługuje: %s %c %d %u %x %p %% */
void printk(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
