#ifndef PROTONOS_SMP_H
#define PROTONOS_SMP_H

#include <stdint.h>

/* Pozostale procesory
 *
 * Do tej pory system pracowal na jednym rdzeniu i nie wiedzial nawet,
 * ze sa inne. Tablica MADT w ACPI wylicza je wszystkie, a kazdy z nich
 * czeka na sygnal INIT-SIPI od procesora rozruchowego - dopoki go nie
 * dostanie, stoi.
 *
 * To jest pierwszy krok, nie caly SMP: obudzone procesory przechodza
 * w tryb chroniony, zglaszaja sie i staja. Rozdzielanie im pracy
 * wymaga wlasnego schedulera i zamkow w calym jadrze, a tego jeszcze
 * nie ma - i lepiej, zeby stalo, niz zeby psulo struktury, ktore
 * napisano dla jednego rdzenia.
 */

/* Czyta MADT i budzi pozostale procesory. Zwraca, ile ich w sumie
   jest (razem z rozruchowym); 1 = maszyna jednordzeniowa albo brak
   ACPI. Wolane po acpi_init i po starcie sterty. */
int smp_init(void);

/* Ile procesorow wylicza firmware i ile faktycznie sie zglosilo */
int smp_cpus(void);
int smp_started(void);

#endif
