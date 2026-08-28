/* fork64 - powielanie procesu widziane od strony programu
 *
 * Program wpisuje liczbe do swojej zmiennej i powiela sie. Od tej
 * chwili sa dwa procesy z ta sama pamiecia - ale KOPIA, nie ta sama.
 * Dziecko wpisuje swoja liczbe, rodzic czeka i sprawdza, czy jego
 * wlasna przetrwala.
 *
 * Rodzic i dziecko wracaja z fork() w to samo miejsce, z tym samym
 * stanem procesora - rozni ich jedna wartosc: dziecko dostaje zero,
 * rodzic numer dziecka.
 */

#include "usys64.h"

static volatile uint64_t wartosc;
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

void _start(long id)
{
    (void)id;

    wartosc = 100;

    int64_t dziecko = sys_fork();
    if (dziecko < 0) {
        print("[fork64] BLAD: fork sie nie udal\n");
        sys_exit(1);
    }

    if (dziecko == 0) {
        /* Dziecko. Nadpisuje wartosc u siebie - rodzic nie ma prawa
           tego zobaczyc, bo dostal wlasna kopie strony. */
        wartosc = 200;
        int n = put(0, "[fork64] dziecko: fork dal 0, moja wartosc to ");
        n = putn(n, wartosc);
        buf[n++] = '\n';
        buf[n] = 0;
        print(buf);
        sys_exit(0);
    }

    /* Rodzic. Daje dziecku czas na zapis i sprawdza swoja pamiec. */
    int n = put(0, "[fork64] rodzic: fork dal numer dziecka ");
    n = putn(n, (uint64_t)dziecko);
    buf[n++] = '\n';
    buf[n] = 0;
    print(buf);

    sys_sleep(300);

    n = put(0, wartosc == 100 ? "[fork64] rodzic: nadal "
                              : "[fork64] rodzic: BLAD, teraz ");
    n = putn(n, wartosc);
    n = put(n, wartosc == 100 ? " - dziecko dostalo kopie\n"
                              : " - dziecko pisze po mojej pamieci\n");
    buf[n] = 0;
    print(buf);

    sys_exit(wartosc == 100 ? 0 : 1);
}
