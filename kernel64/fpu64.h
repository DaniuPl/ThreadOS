#ifndef PROTONOS_FPU64_H
#define PROTONOS_FPU64_H

#include <stdint.h>

/* Jednostka zmiennoprzecinkowa i rejestry SSE
 *
 * Na x86-64 SSE nie jest dodatkiem, tylko czescia konwencji wywolan -
 * kompilator uzywa go bez pytania, bo kazdy procesor w tym trybie go
 * ma. Procesor jednak startuje z wylaczona obsluga i kazda taka
 * instrukcja konczy sie wyjatkiem, dopoki system nie powie wprost, ze
 * umie te rejestry obsluzyc. "Umie" znaczy tu jedno: potrafi je
 * zapisac i odtworzyc przy przelaczaniu zadan.
 */

/* Wlacza SSE w rejestrach sterujacych. 1 = wlaczone. */
int fpu64_init(void);

/* Miejsce na stan jednego zadania (512 B, wyrownane do 16).
   Zwraca NULL przy braku pamieci. */
void *fpu64_alloc(void);
void fpu64_free(void *area);

/* Zapisuje stan biezacego zadania i wczytuje stan nastepnego */
void fpu64_save(void *area);
void fpu64_load(const void *area);

int fpu64_enabled(void);

#endif
