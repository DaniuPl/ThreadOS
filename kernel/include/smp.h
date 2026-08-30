#ifndef PROTONOS_SMP_H
#define PROTONOS_SMP_H

#include <stdint.h>

/* Ile rdzeni system obsluguje. Tablice indeksowane numerem rdzenia
   leza w kilku plikach, wiec liczba musi byc widoczna dla wszystkich. */
#define SMP_CORES 8

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

/* Numer rdzenia, na ktorym biegnie wolajacy.
 *
 * Pyta o to cale jadro i pyta bez przerwy: slowo 'current' znaczy
 * "zadanie tego rdzenia", wiec kazde jego uzycie to jedno takie
 * pytanie. Odpowiedz musi byc wiec tania, a odczyt z lokalnego APIC
 * tani nie jest - to rejestr sprzetowy poza pamiecia podreczna,
 * setki cykli za kazdym razem.
 *
 * Numer bierzemy wiec z rejestru zadania procesora: kazdy rdzen ma
 * WLASNY segment stanu zadania i wlasny selektor, a instrukcja str
 * czyta go z samego procesora, bez jednego dostepu do pamieci.
 * Zanim rdzen zaladuje swoj segment, str daje zero - i to jest
 * poprawna odpowiedz, bo do tej chwili biegnie tylko rdzen
 * rozruchowy. */
int smp_core(void);

/* To samo pytanie zadane lokalnemu APIC. Kosztuje odczyt rejestru
   sprzetowego, ale jest jedyna odpowiedzia, jaka rdzen ma, zanim
   zaladuje swoj segment stanu zadania - i to wlasnie po to sluzy. */
int smp_core_apic(void);

/* Tykniecia wlasnego zegara danego rdzenia. Rdzen rozruchowy chodzi
   z PIT i tu ma zero; wtorne licza swoje zegary w APIC. */
uint32_t smp_ticks(int core);

/* Obsluga tykniecia zegara APIC (wektor 48) - wola to isr_handler */
void smp_timer_tick(void);

/* Obsluga pobudki od innego rdzenia (wektor 49). Nie robi nic poza
   potwierdzeniem: cala jej tresc to fakt, ze przyszla i wyrwala
   procesor z instrukcji hlt. */
void smp_wakeup_recv(void);

/* Wykonuje to, co zostalo ze zlecenia smp_praca (albo nic, gdy nie ma
   zlecenia). Wola to petla bezczynna rdzenia. */
void smp_run_work(int core);

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
void smp_work(void (*fn)(uint32_t number, void *data), void *data,
               uint32_t ile);

/* Ile jednostek policzyl kazdy rdzen w ostatnim zleceniu (indeks 0 to
   rdzen rozruchowy) i ile rdzeni w ogole wzielo w nim udzial */
uint32_t smp_share(int core);
int smp_workers(void);

#endif
