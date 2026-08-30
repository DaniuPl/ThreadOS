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

#include "include/zamekjadra.h"
#include "include/spinlock.h"
#include "include/smp.h"

static struct spinlock zamek = SPINLOCK_WOLNY;
static volatile int wlasciciel = -1;
static volatile int glebokosc;

int jadro_trzymam(void)
{
    return wlasciciel == smp_rdzen();
}

void jadro_wejdz(void)
{
    int nr = smp_rdzen();
    if (wlasciciel == nr) {
        glebokosc++;
        return;
    }
    spin_lock(&zamek);
    wlasciciel = nr;
    glebokosc = 1;
}

void jadro_wyjdz(void)
{
    if (wlasciciel != smp_rdzen())
        return;
    if (--glebokosc > 0)
        return;
    wlasciciel = -1;
    spin_unlock(&zamek);
}

int jadro_zwolnij_wszystko(void)
{
    if (wlasciciel != smp_rdzen())
        return 0;
    int ile = glebokosc;
    glebokosc = 0;
    wlasciciel = -1;
    spin_unlock(&zamek);
    return ile;
}

void jadro_przywroc(int ile)
{
    if (ile <= 0)
        return;
    spin_lock(&zamek);
    wlasciciel = smp_rdzen();
    glebokosc = ile;
}
