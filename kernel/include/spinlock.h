#ifndef PROTONOS_SPINLOCK_H
#define PROTONOS_SPINLOCK_H

#include <stdint.h>
#include "io.h"

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
    volatile uint32_t taken;
};

#define SPINLOCK_FREE { 0 }

static inline void spin_lock(struct spinlock *z)
{
    uint32_t was;
    do {
        was = 1;
        __asm__ volatile("xchgl %0, %1"
                         : "+r"(was), "+m"(z->taken)
                         : : "memory");
        if (!was)
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
                     : "+r"(zero), "+m"(z->taken)
                     : : "memory");
}

/* Zamek brany razem z wygaszeniem przerwan - i tak wyglada prawie
 * kazde uzycie w jadrze.
 *
 * Dwie rzeczy chronia przed dwiema roznymi. Zamek chroni przed INNYM
 * RDZENIEM, ktory naprawde biegnie jednoczesnie. Wygaszenie przerwan
 * chroni przed SAMYM SOBA: struktury jadra dotyka i watek, i obsluga
 * przerwania, wiec timer, ktory wszedlby w srodek sekcji trzymajacej
 * zamek, czekalby na zamek trzymany przez przerwany wlasnie kod - i to
 * na tym samym rdzeniu, ktory jako jedyny moglby go zwolnic.
 *
 * Kolejnosc nie jest dowolna: przerwania gasna PRZED wzieciem zamka
 * i wracaja PO jego zwolnieniu. Odwrotnie zostawialoby okno, w ktorym
 * przerwanie zastaje zamek juz wziety.
 */
static inline irqflags_t spin_lock_irq(struct spinlock *z)
{
    irqflags_t flags = irq_save();
    spin_lock(z);
    return flags;
}

static inline void spin_unlock_irq(struct spinlock *z, irqflags_t flags)
{
    spin_unlock(z);
    irq_restore(flags);
}

#endif
