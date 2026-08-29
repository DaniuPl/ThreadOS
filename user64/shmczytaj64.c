/* shmczytaj64 - czytelnik pamieci wspoldzielonej
 *
 * Mapuje ten sam region (klucz 42), co shmpisz64, i wypisuje wartosc,
 * ktora zapisal tam INNY proces. Potem odpisuje w to samo miejsce,
 * zeby bylo widac, ze droga jest w obie strony.
 */

#include "usys64.h"

#define KLUCZ  42
#define WZORZEC 12345

static char out[96];

static void powiedz(const char *a, uint64_t v)
{
    int n = 0;
    while (*a)
        out[n++] = *a++;
    char tmp[24];
    utoa(v, tmp);
    for (int i = 0; tmp[i]; i++)
        out[n++] = tmp[i];
    out[n++] = '\n';
    out[n] = '\0';
    print(out);
}

void _start(void)
{
    volatile uint32_t *p = sys_shm(KLUCZ, 4096);
    if (!p) {
        print("[shmczytaj64] brak pamieci wspoldzielonej\n");
        sys_exit(1);
    }

    powiedz("[shmczytaj64] region pod adresem ", (uint64_t)(uintptr_t)p);
    powiedz("[shmczytaj64] wartosc=", p[0]);

    if (p[0] != WZORZEC) {
        print("[shmczytaj64] ZLE: to nie ta wartosc\n");
        sys_exit(1);
    }
    p[2] = WZORZEC + 1;              /* odpowiedz ta sama droga */
    print("[shmczytaj64] zgadza sie - dwa procesy, jedna pamiec\n");
    sys_exit(0);
}
