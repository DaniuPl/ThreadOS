/* Wielki zamek jadra - jeden zamek na caly kod jadra
 *
 * Zagniezdzalny, bo musi byc: przerwanie potrafi trafic w srodek
 * wywolania systemowego, a rdzen, ktory juz zamek trzyma, nie moze na
 * siebie czekac. Licznik glebokosci i numer wlasciciela sa chronione
 * samym zamkiem - pisze do nich tylko ten, kto go ma.
 *
 * Odczyt 'wlasciciel' bez zamka jest bezpieczny i tak ma byc: gdy
 * trzyma go inny rdzen, przeczytamy jego numer, ktory nie jest naszym,
 * i pojdziemy czekac. Gdy nie trzyma go nikt, przeczytamy -1. W obu
 * przypadkach odpowiedz "czy to ja" jest poprawna.
 */

#include "include/biglock.h"
#include "include/spinlock.h"
#include "include/smp.h"

static struct spinlock lock = SPINLOCK_FREE;
static volatile int owner = -1;
static volatile int depth;

int biglock_held(void)
{
    return owner == smp_core();
}

void biglock_enter(void)
{
    int nr = smp_core();
    if (owner == nr) {
        depth++;
        return;
    }
    spin_lock(&lock);
    owner = nr;
    depth = 1;
}

void biglock_leave(void)
{
    if (owner != smp_core())
        return;
    if (--depth > 0)
        return;
    owner = -1;
    spin_unlock(&lock);
}

int biglock_release_all(void)
{
    if (owner != smp_core())
        return 0;
    int ile = depth;
    depth = 0;
    owner = -1;
    spin_unlock(&lock);
    return ile;
}

void biglock_restore(int ile)
{
    if (ile <= 0)
        return;
    spin_lock(&lock);
    owner = smp_core();
    depth = ile;
}
