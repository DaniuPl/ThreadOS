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

static volatile uint64_t value;
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

void _start(void)
{

    value = 100;

    int64_t child = sys_fork();
    if (child < 0) {
        print("[fork64] BLAD: fork sie nie udal\n");
        sys_exit(1);
    }

    if (child == 0) {
        /* Dziecko. Nadpisuje wartosc u siebie - rodzic nie ma prawa
           tego zobaczyc, bo dostal wlasna kopie strony. */
        value = 200;
        int n = put(0, "[fork64] dziecko: fork dal 0, moja wartosc to ");
        n = putn(n, value);
        buf[n++] = '\n';
        buf[n] = 0;
        print(buf);
        sys_exit(0);
    }

    /* Rodzic. Daje dziecku czas na zapis i sprawdza swoja pamiec. */
    int n = put(0, "[fork64] rodzic: fork dal numer dziecka ");
    n = putn(n, (uint64_t)child);
    buf[n++] = '\n';
    buf[n] = 0;
    print(buf);

    sys_sleep(300);

    n = put(0, value == 100 ? "[fork64] rodzic: nadal "
                              : "[fork64] rodzic: BLAD, teraz ");
    n = putn(n, value);
    n = put(n, value == 100 ? " - dziecko dostalo kopie\n"
                              : " - dziecko pisze po mojej pamieci\n");
    buf[n] = 0;
    print(buf);

    sys_exit(value == 100 ? 0 : 1);
}
