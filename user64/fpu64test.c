/* fpu64test - liczby zmiennoprzecinkowe i rejestry SSE
 *
 * Jadro uruchamia dwie kopie naraz, kazda z inna liczba poczatkowa.
 * Obie licza w petli, oddajac po drodze procesor - i gdyby rejestry
 * zmiennoprzecinkowe nie byly zapisywane przy przelaczaniu zadan,
 * kazda dostawalaby wynik drugiej.
 *
 * Dzialanie dobrane tak, zeby bylo dokladne: v = v * 2 + 1 na
 * liczbach calkowitych mieszczacych sie w podwojnej precyzji nie
 * traci ani jednego bitu, wiec wynik da sie sprawdzic co do jednosci.
 * Po dwudziestu obrotach z 'a' wychodzi (a + 1) * 2^20 - 1.
 */

#include "usys64.h"

#define OBROTOW 20

static char out[128];

static int put(int n, const char *s)
{
    while (*s)
        out[n++] = *s++;
    return n;
}

static int putn(int n, uint64_t v)
{
    char tmp[24];
    utoa(v, tmp);
    return put(n, tmp);
}

void _start(int argc, char **argv)
{
    int64_t start = argc > 1 ? uatoi(argv[1]) : 1;
    if (start < 0)
        start = 1;

    double v = (double)start;
    for (int i = 0; i < OBROTOW; i++) {
        v = v * 2.0 + 1.0;
        sys_sleep(20);               /* tu wchodzi drugi proces */
    }

    /* To samo na liczbach calkowitych - wynik musi byc identyczny */
    uint64_t oczekiwane = (uint64_t)start;
    for (int i = 0; i < OBROTOW; i++)
        oczekiwane = oczekiwane * 2 + 1;

    uint64_t wynik = (uint64_t)v;

    int n = put(0, "[fpu64] start ");
    n = putn(n, (uint64_t)start);
    n = put(n, ": wyszlo ");
    n = putn(n, wynik);
    n = put(n, ", mialo ");
    n = putn(n, oczekiwane);
    n = put(n, wynik == oczekiwane ? " - zgadza sie\n"
                                   : " - NIE ZGADZA SIE\n");
    out[n] = 0;
    print(out);

    sys_exit(wynik == oczekiwane ? 0 : 1);
}
