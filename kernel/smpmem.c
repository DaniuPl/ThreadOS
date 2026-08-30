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

#define JEDNOSTEK   64
#define POWTORZEN   48
#define SLOW        (4096 / 4)      /* slow w ramce */

struct proba {
    volatile uint32_t bledow;       /* niezgodnosci wzorca */
    volatile uint32_t nieudanych;   /* przydzialy, ktore sie nie udaly */
    volatile uint32_t przydzialow;
};

static uint32_t wzorzec(uint32_t numer, uint32_t krok)
{
    return numer * 2654435761u + krok * 40503u + 1u;
}

static void jednostka(uint32_t numer, void *dane)
{
    struct proba *p = dane;

    for (uint32_t i = 0; i < POWTORZEN; i++) {
        __atomic_add_fetch(&p->przydzialow, 1, __ATOMIC_RELAXED);

        /* --- ramka fizyczna --- */
        uint32_t ramka = pmm_alloc_frame();
        if (!ramka) {
            __atomic_add_fetch(&p->nieudanych, 1, __ATOMIC_RELAXED);
        } else {
            uint32_t w = wzorzec(numer, i);
            volatile uint32_t *s = (volatile uint32_t *)(uintptr_t)ramka;
            for (uint32_t k = 0; k < SLOW; k++)
                s[k] = w + k;
            /* Drugi obieg czyta to samo. Gdyby ta sama ramka trafila
               tez do innego rdzenia, wpisalby w nia swoj wzorzec -
               i wlasnie tu by to wyszlo. */
            for (uint32_t k = 0; k < SLOW; k++)
                if (s[k] != w + k) {
                    __atomic_add_fetch(&p->bledow, 1, __ATOMIC_RELAXED);
                    break;
                }
            pmm_free_frame(ramka);
        }

        /* --- blok sterty --- */
        uint32_t ile = 32 + (numer * 7 + i * 13) % 192;
        uint8_t *b = kmalloc(ile);
        if (!b) {
            __atomic_add_fetch(&p->nieudanych, 1, __ATOMIC_RELAXED);
            continue;
        }
        uint8_t znak = (uint8_t)(numer * 31 + i);
        for (uint32_t k = 0; k < ile; k++)
            b[k] = (uint8_t)(znak + k);
        for (uint32_t k = 0; k < ile; k++)
            if (b[k] != (uint8_t)(znak + k)) {
                __atomic_add_fetch(&p->bledow, 1, __ATOMIC_RELAXED);
                break;
            }
        kfree(b);
    }
}

void smpmem_test(void)
{
    struct proba p = { 0, 0, 0 };

    uint32_t ramek_przed = pmm_used_frames();
    uint32_t sterty_przed = heap_used_bytes();

    smp_praca(jednostka, &p, JEDNOSTEK);

    uint32_t ramek_po = pmm_used_frames();
    uint32_t sterty_po = heap_used_bytes();

    printk("Rdzeni: %d (obudzonych %d), jednostek %u po %u przydzialow\n",
           smp_cpus(), smp_started(), (uint32_t)JEDNOSTEK,
           (uint32_t)POWTORZEN);
    printk("Udzial:");
    for (int i = 0; i < smp_cpus(); i++)
        printk(" rdzen%d=%u", i, smp_udzial(i));
    printk("\n");
    printk("Przydzialow: %u, nieudanych: %u\n", p.przydzialow, p.nieudanych);
    printk("Wzorzec: %s (bledow %u)\n",
           p.bledow ? "NIE ZGADZA SIE" : "zgadza sie", p.bledow);
    printk("Ramki: %u -> %u, sterta: %u -> %u B\n",
           ramek_przed, ramek_po, sterty_przed, sterty_po);
    printk("Nic nie wycieklo: %s\n",
           (ramek_przed == ramek_po && sterty_przed == sterty_po)
               ? "tak" : "NIE");
    printk("Liczylo rdzeni: %d\n", smp_pracujacych());
}
