#ifndef PROTONOS_KEYBOARD64_H
#define PROTONOS_KEYBOARD64_H

/* Instaluje handler IRQ1 i odmaskowuje przerwanie klawiatury */
void keyboard64_init(void);

/* Czeka na klawisz i zwraca go (usypia, gdy nic nie ma) */
char keyboard64_getchar(void);

/* Czy cokolwiek czeka w kolejce */
int keyboard64_pending(void);

#endif
