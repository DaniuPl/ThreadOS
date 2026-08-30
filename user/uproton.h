/* Co program moze powiedziec o bibliotece wspoldzielonej
 *
 * Wszystko tutaj musi byc policzone WEWNATRZ biblioteki i nie da sie
 * tego obejsc. Program nieprzemieszczalny, ktory bierze adres funkcji
 * z biblioteki, dostaje adres wlasnej zaslepki skoku - miejsca, przez
 * ktore skacze - a nie tego, gdzie funkcja naprawde lezy. Na pytanie
 * "gdzie jestes" odpowiedziec moze wiec tylko ona sama.
 */

#ifndef PROTONOS_UPROTON_H
#define PROTONOS_UPROTON_H

#include "uapi.h"
#include "usha256.h"
#include "urand.h"

/* Adres kodu biblioteki - ten sam we wszystkich procesach, bo to te
   same ramki fizyczne */
static inline uintptr_t proton_adres_kodu(void)
{
    return (uintptr_t)sha256;
}

/* Adres danych biblioteki. Adres wirtualny tez jest wszedzie ten sam,
   ale ramki pod nim juz nie - kazdy proces ma swoje. */
static inline uintptr_t proton_adres_danych(void)
{
    return (uintptr_t)urand_seed;
}

/* Licznik generatora losowego. Rosnie z kazdym pobraniem bajtow, wiec
   pokazuje, ile ten proces wzial z biblioteki - i jest dobrym probnikiem
   tego, czy dane naprawde sa prywatne. */
static inline uint32_t proton_licznik_losowosci(void)
{
    return urand_counter;
}

#endif
