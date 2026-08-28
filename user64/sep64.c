/* sep64 - dowod, ze kazdy proces ma wlasna przestrzen adresowa
 *
 * Jadro uruchamia dwie kopie tego programu naraz. Obie sa tym samym
 * plikiem, zaladowanym pod ten sam adres wirtualny, i obie pisza pod
 * ten sam adres swojej zmiennej. Rozni je jedno: liczba, ktora
 * dostaly od jadra w rejestrze RDI.
 *
 * Gdyby przestrzen byla wspolna, druga kopia nadpisalaby pierwszej
 * jej wartosc - i pierwsza by to zauwazyla. Program wpisuje swoja
 * liczbe, oddaje procesor na chwile drugiemu, a potem sprawdza, czy
 * jego pamiec jest taka, jak ja zostawil.
 */

#include "usys64.h"

static volatile uint64_t marker;     /* ten sam adres w obu procesach */
static char buf[96];

static int put(int n, const char *s)
{
    while (*s)
        buf[n++] = *s++;
    return n;
}

static int putn(int n, uint64_t v)
{
    char tmp[24];
    utoa(v, tmp);
    return put(n, tmp);
}

static int puth(int n, uint64_t v)
{
    char tmp[24];
    uhtoa(v, tmp);
    return put(n, tmp);
}

void _start(long id)
{
    uint64_t mine = 1000 + (uint64_t)id;
    marker = mine;

    int n = put(0, "[sep64] proces ");
    n = putn(n, (uint64_t)id);
    n = put(n, ": adres ");
    n = puth(n, (uint64_t)(uintptr_t)&marker);
    n = put(n, ", wpisalem ");
    n = putn(n, mine);
    buf[n++] = '\n';
    buf[n] = 0;
    print(buf);

    /* Chwila dla drugiego procesu - niech napisze swoje */
    sys_sleep(200);

    uint64_t now = marker;
    n = put(0, "[sep64] proces ");
    n = putn(n, (uint64_t)id);
    n = put(n, now == mine ? ": nadal " : ": BLAD, teraz ");
    n = putn(n, now);
    n = put(n, now == mine ? " - moja pamiec nietknieta\n"
                           : " - ktos mi ja nadpisal\n");
    buf[n] = 0;
    print(buf);

    sys_exit(now == mine ? 0 : 1);
}
