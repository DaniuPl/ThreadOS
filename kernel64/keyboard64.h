#ifndef PROTONOS_KEYBOARD64_H
#define PROTONOS_KEYBOARD64_H

/* Instaluje handler IRQ1 i odmaskowuje przerwanie klawiatury */
void keyboard64_init(void);

/* Czeka na klawisz i zwraca go (usypia, gdy nic nie ma) */
char keyboard64_getchar(void);

/* Komu wyslac SIGINT po Ctrl+C (-1 = nikomu) */
void keyboard64_set_intr_target(int id);

/* Odczyt dla programu w ring 3: blokuje do pierwszego znaku, zwraca
   ile ich wzial; -1 = czekanie przerwal sygnal (bez echa - echo robi
   czytajacy) */
int keyboard64_read(char *out, uint32_t len);

/* Czy cokolwiek czeka w kolejce */
int keyboard64_pending(void);

#endif
