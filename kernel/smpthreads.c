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
#include "include/smpthreads.h"
#include "include/smp.h"
#include "include/task.h"
#include "include/pit.h"
#include "include/printk.h"

#define THREADS   4
#define STEPS   100000000u

static volatile uint32_t mask[THREADS];    /* rdzenie, ktore go prowadzily */
static volatile uint32_t result[THREADS];
static volatile uint32_t finished;

/* Liniowy generator kongruencyjny - zawsze ten sam ciag dla danego
   numeru, wiec wynik nie zalezy od tego, kto go policzyl */
static uint32_t count(uint32_t number, volatile uint32_t *where_i_was)
{
    uint32_t stan = number * 2654435761u + 12345u;
    uint32_t sum = 0;
    for (uint32_t i = 0; i < STEPS; i++) {
        stan = stan * 1103515245u + 12345u;
        sum += stan >> 16;
        /* Co jakis czas zapisz, na ktorym rdzeniu wlasnie jestesmy.
           Rzadko, bo odczyt numeru idzie przez lokalny APIC. */
        if (where_i_was && (i & 0xFFFF) == 0)
            *where_i_was |= 1u << smp_core();
    }
    return sum;
}

static void thread(void *arg)
{
    uint32_t i = (uint32_t)(uintptr_t)arg;
    result[i] = count(0, &mask[i]);
    __atomic_add_fetch(&finished, 1, __ATOMIC_SEQ_CST);
}

void smpthreads_test(void)
{
    for (int i = 0; i < THREADS; i++) {
        mask[i] = 0;
        result[i] = 0;
    }
    finished = 0;

    /* --- ile trwa JEDNA porcja, policzona tu, w watku shella ---
       To jest miara odniesienia. Cztery porcje puszczone rownolegle
       maja zajac tyle samo czasu, co jedna; zrobione po kolei zajelyby
       cztery razy tyle. Miedzy jednym a drugim jest cala roznica. */
    uint32_t start = pit_ticks();
    uint32_t pattern = count(0, (volatile uint32_t *)0);
    uint32_t time_one = pit_ticks() - start;

    /* --- cztery porcje naraz, kazda we wlasnym watku --- */
    start = pit_ticks();
    for (uint32_t i = 0; i < THREADS; i++)
        task_create_anycore("liczy", thread, (void *)(uintptr_t)i);
    while (__atomic_load_n(&finished, __ATOMIC_SEQ_CST) < THREADS)
        task_sleep(10);
    uint32_t time_four = pit_ticks() - start;

    uint32_t all_cores = 0;
    int matches = 1;
    for (int i = 0; i < THREADS; i++) {
        all_cores |= mask[i];
        /* Kazdy watek liczyl te sama porcje, co watek odniesienia -
           wynik ma byc co do bitu ten sam, niezaleznie od tego, ktory
           rdzen go policzyl */
        if (result[i] != pattern)
            matches = 0;
        printk("  watek%d: rdzenie %x, wynik %u\n", i, mask[i], result[i]);
    }

    printk("Watkow: %u po %u krokow, rdzeni w maszynie: %d\n",
           (uint32_t)THREADS, STEPS, smp_cpus());
    printk("Rdzenie, ktore je prowadzily: %x\n", all_cores);
    printk("Wynik: %s\n", matches ? "zgadza sie" : "NIE ZGADZA SIE");
    printk("Jedna porcja: %u tyknac, cztery naraz: %u tyknac\n",
           time_one, time_four);
    printk("Watki poza rdzeniem rozruchowym: %s\n",
           (all_cores & ~1u) ? "tak" : "NIE");
}
