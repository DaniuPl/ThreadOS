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
 * Obudzone rdzenie nie prowadza wlasnych zadan - scheduler, lista
 * procesow i cala reszta jadra pisane byly dla jednego rdzenia i cicho
 * na to licza. Dostaja za to prace policzalna: wspolna kolejke
 * jednostek, z ktorej kazdy bierze kolejny numer pod zamkiem i liczy
 * go u siebie. To jest caly wspoldzielony stan miedzy rdzeniami -
 * jedna liczba i jeden zamek - i dlatego da sie o nim myslec.
 */

/* Czyta MADT i budzi pozostale procesory. Zwraca, ile ich w sumie
   jest (razem z rozruchowym); 1 = maszyna jednordzeniowa albo brak
   ACPI. Wolane po acpi_init i po starcie sterty. */
int smp_init(void);

/* Ile procesorow wylicza firmware i ile faktycznie sie zglosilo */
int smp_cpus(void);
int smp_started(void);

/* --- Praca rozdzielana na rdzenie -----------------------------------
 *
 * Zleca 'ile' jednostek pracy i wraca, gdy wszystkie sa policzone.
 * Kazda jednostka dostaje swoj numer; co z nim zrobi, jest sprawa
 * funkcji. Rdzen rozruchowy liczy razem z pozostalymi - inaczej na
 * maszynie jednordzeniowej nie policzylby nikt.
 *
 * Funkcja biegnie na rdzeniu bez przerwan i bez tablicy wyjatkow:
 * nie wolno jej usypiac, wolac jadra ani sie mylic w adresach. Ma
 * liczyc na tym, co dostala, i tyle. */
void smp_praca(void (*funkcja)(uint32_t numer, void *dane), void *dane,
               uint32_t ile);

/* Ile jednostek policzyl kazdy rdzen w ostatnim zleceniu (indeks 0 to
   rdzen rozruchowy) i ile rdzeni w ogole wzielo w nim udzial */
uint32_t smp_udzial(int rdzen);
int smp_pracujacych(void);

#endif
