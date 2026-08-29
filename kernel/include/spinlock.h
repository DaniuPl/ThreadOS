#ifndef PROTONOS_SPINLOCK_H
#define PROTONOS_SPINLOCK_H

#include <stdint.h>

/* Zamek wirujacy
 *
 * Pierwsza rzecz, ktorej system potrzebuje, gdy procesorow jest wiecej
 * niz jeden. Wylaczenie przerwan wystarczalo, dopoki rdzen byl jeden:
 * nikt inny nie mogl wejsc w srodek operacji. Teraz moze - i to
 * naprawde JEDNOCZESNIE, a nie tylko pozornie.
 *
 * Instrukcja xchg jest niepodzielna z definicji i na dodatek stanowi
 * barierę dla porzadku zapisow, wiec nie trzeba tu zadnych dodatkowych
 * ogrodzen: to, co rdzen zapisal przed zwolnieniem zamka, drugi rdzen
 * zobaczy po jego wzieciu.
 *
 * Czekamy wirujac, bo zamek trzymany jest przez kilka instrukcji -
 * usypianie kosztowaloby wiecej niz samo czekanie. Z tego samego
 * powodu pod zamkiem nie wolno robic niczego dlugiego i niczego, co
 * moze uspic zadanie.
 */

struct spinlock {
    volatile uint32_t zajety;
};

#define SPINLOCK_WOLNY { 0 }

static inline void spin_lock(struct spinlock *z)
{
    uint32_t bylo;
    do {
        bylo = 1;
        __asm__ volatile("xchgl %0, %1"
                         : "+r"(bylo), "+m"(z->zajety)
                         : : "memory");
        if (!bylo)
            break;
        /* Podpowiedz dla procesora, ze to petla czekania: oszczedza
           energie i nie zapycha magistrali odczytami */
        __asm__ volatile("pause" ::: "memory");
    } while (1);
}

static inline void spin_unlock(struct spinlock *z)
{
    uint32_t zero = 0;
    __asm__ volatile("xchgl %0, %1"
                     : "+r"(zero), "+m"(z->zajety)
                     : : "memory");
}

#endif
