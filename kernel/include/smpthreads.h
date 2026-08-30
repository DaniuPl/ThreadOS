#ifndef PROTONOS_SMPTHREADS_H
#define PROTONOS_SMPTHREADS_H

/* Watki jadra na wszystkich rdzeniach
 *
 * Rdzen wtorny ma juz wszystko, czego potrzeba, zeby prowadzic zadania:
 * tablice wyjatkow, wlasny zegar, wlasne zadanie bezczynne i zamek na
 * liscie zadan. Ta proba sprawdza, czy naprawde je prowadzi - i czy
 * robi to ROWNOLEGLE, a nie tylko na zmiane.
 *
 * Samo "watek sie policzyl" nie rozstrzygnelby niczego: przeplatanie
 * na jednym rdzeniu wyglada tak samo. Rozstrzygaja dwie rzeczy, ktorych
 * jeden rdzen nie potrafi udac - numer rdzenia zapisany przez sam watek
 * w trakcie liczenia i CZAS: ta sama praca puszczona rownolegle konczy
 * sie szybciej niz po kolei.
 */

/* Puszcza probe i wypisuje wynik. Wolane z shella obu jader. */
void smpthreads_test(void);

#endif
