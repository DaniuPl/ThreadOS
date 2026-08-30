/* Proba zamkow: mapa ramek i sterta jadra pod obstrzalem wszystkich rdzeni
 *
 * Zamek chroni przed czyms, czego nie widac inaczej niz probujac.
 * Wygaszenie przerwan wystarczalo, dopoki rdzen byl jeden - nikt nie
 * mogl wejsc w srodek operacji. Teraz dwa rdzenie moga byc w tej samej
 * petli szukajacej wolnej ramki naprawde jednoczesnie, znalezc te sama
 * i oba uznac ja za swoja.
 *
 * Stad ksztalt tej proby. Nie pyta "czy sie nie wysypalo", bo na to
 * odpowiedz bywa przypadkowa. Kazdy przydzial dostaje wzorzec zalezny
 * od numeru jednostki i zaraz potem jest czytany: ramka wydana dwa razy
 * ma dwoch piszacych i wzorzec sie nie zgodzi. Na koniec liczymy jeszcze
 * ramki i bajty sterty - tyle samo przed co po, inaczej cos wycieklo
 * albo wrocilo dwa razy.
 *
 * Praca biegnie przez smp_praca, wiec na rdzeniu wtornym nie ma ani
 * przerwan, ani tablicy wyjatkow. Wolno tu wiec wolac tylko to, co nie
 * spi i nie czeka na sprzet: przydzial pamieci tak, dysk czy siec nie.
 */

#include <stdint.h>
#include "include/smpmem.h"
#include "include/smp.h"
#include "include/pmm.h"
#include "include/heap.h"
#include "include/printk.h"
#include "include/string.h"

#define UNITS   64
#define RETRIES   48
#define SLOW        (4096 / 4)      /* slow w ramce */

struct attempt {
    volatile uint32_t nerrors;       /* niezgodnosci wzorca */
    volatile uint32_t failed;   /* przydzialy, ktore sie nie udaly */
    volatile uint32_t allocs;
};

static uint32_t pattern(uint32_t number, uint32_t step)
{
    return number * 2654435761u + step * 40503u + 1u;
}

static void unit(uint32_t number, void *data)
{
    struct attempt *p = data;

    for (uint32_t i = 0; i < RETRIES; i++) {
        __atomic_add_fetch(&p->allocs, 1, __ATOMIC_RELAXED);

        /* --- ramka fizyczna --- */
        uint32_t frame = pmm_alloc_frame();
        if (!frame) {
            __atomic_add_fetch(&p->failed, 1, __ATOMIC_RELAXED);
        } else {
            uint32_t w = pattern(number, i);
            volatile uint32_t *s = (volatile uint32_t *)(uintptr_t)frame;
            for (uint32_t k = 0; k < SLOW; k++)
                s[k] = w + k;
            /* Drugi obieg czyta to samo. Gdyby ta sama ramka trafila
               tez do innego rdzenia, wpisalby w nia swoj wzorzec -
               i wlasnie tu by to wyszlo. */
            for (uint32_t k = 0; k < SLOW; k++)
                if (s[k] != w + k) {
                    __atomic_add_fetch(&p->nerrors, 1, __ATOMIC_RELAXED);
                    break;
                }
            pmm_free_frame(frame);
        }

        /* --- blok sterty --- */
        uint32_t ile = 32 + (number * 7 + i * 13) % 192;
        uint8_t *b = kmalloc(ile);
        if (!b) {
            __atomic_add_fetch(&p->failed, 1, __ATOMIC_RELAXED);
            continue;
        }
        uint8_t ch = (uint8_t)(number * 31 + i);
        for (uint32_t k = 0; k < ile; k++)
            b[k] = (uint8_t)(ch + k);
        for (uint32_t k = 0; k < ile; k++)
            if (b[k] != (uint8_t)(ch + k)) {
                __atomic_add_fetch(&p->nerrors, 1, __ATOMIC_RELAXED);
                break;
            }
        kfree(b);
    }
}

void smpmem_test(void)
{
    struct attempt p = { 0, 0, 0 };

    uint32_t frames_at_start = pmm_used_frames();
    uint32_t heap_before = heap_used_bytes();

    smp_work(unit, &p, UNITS);

    uint32_t frames_after = pmm_used_frames();
    uint32_t heap_after = heap_used_bytes();

    printk("Rdzeni: %d (obudzonych %d), jednostek %u po %u przydzialow\n",
           smp_cpus(), smp_started(), (uint32_t)UNITS,
           (uint32_t)RETRIES);
    printk("Udzial:");
    for (int i = 0; i < smp_cpus(); i++)
        printk(" rdzen%d=%u", i, smp_share(i));
    printk("\n");
    printk("Przydzialow: %u, nieudanych: %u\n", p.allocs, p.failed);
    printk("Wzorzec: %s (bledow %u)\n",
           p.nerrors ? "NIE ZGADZA SIE" : "zgadza sie", p.nerrors);
    printk("Ramki: %u -> %u, sterta: %u -> %u B\n",
           frames_at_start, frames_after, heap_before, heap_after);
    printk("Nic nie wycieklo: %s\n",
           (frames_at_start == frames_after && heap_before == heap_after)
               ? "tak" : "NIE");
    printk("Liczylo rdzeni: %d\n", smp_workers());
}
