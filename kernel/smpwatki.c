/* Watki jadra na wszystkich rdzeniach - proba
 *
 * Zadanie utworzone przez task_create_wszedzie wolno poprowadzic
 * kazdemu rdzeniowi. Zwykle zadanie zostaje na rozruchowym i tak ma
 * byc: siega po sterowniki i system plikow, a te pisane byly dla
 * jednego rdzenia. Tutaj wolajacy bierze na siebie, ze jego watek
 * dotyka tylko tego, co zniesie dwa rdzenie naraz - a ten dotyka
 * wlasnej komorki w tablicy i niczego wiecej.
 *
 * Dowod jest dwuczesciowy, bo kazda czesc z osobna dalaby sie podrobic.
 * Numer rdzenia watek zapisuje sam, w trakcie liczenia - ale gdyby
 * scheduler tylko przerzucal go miedzy rdzeniami po kolei, maska
 * wygladalaby tak samo. Dlatego mierzymy jeszcze czas: ta sama praca
 * puszczona rownolegle ma skonczyc sie szybciej niz zrobiona po kolei.
 * Jednego rdzenia nie da sie o to oszukac.
 */

#include <stdint.h>
#include "include/smpwatki.h"
#include "include/smp.h"
#include "include/task.h"
#include "include/pit.h"
#include "include/printk.h"

#define WATKOW   4
#define KROKOW   100000000u

static volatile uint32_t maska[WATKOW];    /* rdzenie, ktore go prowadzily */
static volatile uint32_t wynik[WATKOW];
static volatile uint32_t skonczone;

/* Liniowy generator kongruencyjny - zawsze ten sam ciag dla danego
   numeru, wiec wynik nie zalezy od tego, kto go policzyl */
static uint32_t licz(uint32_t numer, volatile uint32_t *gdzie_bylem)
{
    uint32_t stan = numer * 2654435761u + 12345u;
    uint32_t suma = 0;
    for (uint32_t i = 0; i < KROKOW; i++) {
        stan = stan * 1103515245u + 12345u;
        suma += stan >> 16;
        /* Co jakis czas zapisz, na ktorym rdzeniu wlasnie jestesmy.
           Rzadko, bo odczyt numeru idzie przez lokalny APIC. */
        if (gdzie_bylem && (i & 0xFFFF) == 0)
            *gdzie_bylem |= 1u << smp_rdzen();
    }
    return suma;
}

static void watek(void *arg)
{
    uint32_t i = (uint32_t)(uintptr_t)arg;
    wynik[i] = licz(0, &maska[i]);
    __atomic_add_fetch(&skonczone, 1, __ATOMIC_SEQ_CST);
}

void smpwatki_test(void)
{
    for (int i = 0; i < WATKOW; i++) {
        maska[i] = 0;
        wynik[i] = 0;
    }
    skonczone = 0;

    /* --- ile trwa JEDNA porcja, policzona tu, w watku shella ---
       To jest miara odniesienia. Cztery porcje puszczone rownolegle
       maja zajac tyle samo czasu, co jedna; zrobione po kolei zajelyby
       cztery razy tyle. Miedzy jednym a drugim jest cala roznica. */
    uint32_t start = pit_ticks();
    uint32_t wzorzec = licz(0, (volatile uint32_t *)0);
    uint32_t czas_jednej = pit_ticks() - start;

    /* --- cztery porcje naraz, kazda we wlasnym watku --- */
    start = pit_ticks();
    for (uint32_t i = 0; i < WATKOW; i++)
        task_create_wszedzie("liczy", watek, (void *)(uintptr_t)i);
    while (__atomic_load_n(&skonczone, __ATOMIC_SEQ_CST) < WATKOW)
        task_sleep(10);
    uint32_t czas_czterech = pit_ticks() - start;

    uint32_t wszystkie_rdzenie = 0;
    int zgadza = 1;
    for (int i = 0; i < WATKOW; i++) {
        wszystkie_rdzenie |= maska[i];
        /* Kazdy watek liczyl te sama porcje, co watek odniesienia -
           wynik ma byc co do bitu ten sam, niezaleznie od tego, ktory
           rdzen go policzyl */
        if (wynik[i] != wzorzec)
            zgadza = 0;
        printk("  watek%d: rdzenie %x, wynik %u\n", i, maska[i], wynik[i]);
    }

    printk("Watkow: %u po %u krokow, rdzeni w maszynie: %d\n",
           (uint32_t)WATKOW, KROKOW, smp_cpus());
    printk("Rdzenie, ktore je prowadzily: %x\n", wszystkie_rdzenie);
    printk("Wynik: %s\n", zgadza ? "zgadza sie" : "NIE ZGADZA SIE");
    printk("Jedna porcja: %u tyknac, cztery naraz: %u tyknac\n",
           czas_jednej, czas_czterech);
    printk("Watki poza rdzeniem rozruchowym: %s\n",
           (wszystkie_rdzenie & ~1u) ? "tak" : "NIE");
}
