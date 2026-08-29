#ifndef PROTONOS_KEYBOARD64_H
#define PROTONOS_KEYBOARD64_H

/* Instaluje handler IRQ1 i odmaskowuje przerwanie klawiatury */
void keyboard64_init(void);

/* Czeka na klawisz i zwraca go (usypia, gdy nic nie ma) */
char keyboard64_getchar(void);

/* Pulpit przechwytuje wszystkie klawisze (w trybie graficznym nie ma
   konsoli tekstowej, do ktorej mialyby trafic) */
void keyboard64_set_override(void (*handler)(char c));

/* Komu wyslac SIGINT po Ctrl+C (-1 = nikomu). Dotyczy konsoli, na
   ktorej pisze wolajace zadanie. */
void keyboard64_set_intr_target(int id);

/* Znak i przelaczenie konsoli z innego zrodla niz PS/2 (klawiatura USB) */
void keyboard64_inject(char c);
void keyboard64_inject_switch(int vc);

/* Odczyt dla programu w ring 3: blokuje do pierwszego znaku, zwraca
   ile ich wzial; -1 = czekanie przerwal sygnal (bez echa - echo robi
   czytajacy) */
int keyboard64_read(char *out, uint32_t len);

/* Czy cokolwiek czeka w kolejce */
int keyboard64_pending(void);

#endif
