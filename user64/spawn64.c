/* spawn64 - klasyczny sposob uruchamiania programow: fork, exec, wait
 *
 * Program powiela sie, dziecko podmienia swoj obraz na inny program,
 * a rodzic czeka na jego koniec i odbiera kod wyjscia.
 *
 * Zadna z tych trzech rzeczy nie wymaga od jadra niczego ponad to, co
 * juz umie: fork daje drugi proces, exec podmienia jego pamiec, a wait
 * odbiera slad, ktory po nim zostal. Dopiero razem daja to, co robi
 * kazdy shell na swiecie.
 */

#include "usys64.h"

static char buf[96];

static int put(int n, const char *s)
{
    while (*s)
        buf[n++] = *s++;
    return n;
}

static int putn(int n, int64_t v)
{
    char tmp[24];
    utoa((uint64_t)v, tmp);
    return put(n, tmp);
}

void _start(long id)
{
    (void)id;

    print("[spawn64] powielam sie i uruchamiam w dziecku 'hello64'\n");

    int64_t dziecko = sys_fork();
    if (dziecko < 0) {
        print("[spawn64] BLAD: fork sie nie udal\n");
        sys_exit(1);
    }

    if (dziecko == 0) {
        /* Dziecko przestaje byc soba: od nastepnej instrukcji dziala
           juz zupelnie inny program. Jesli exec wroci, to znaczy, ze
           sie nie udal. */
        sys_exec("hello64");
        print("[spawn64] BLAD: exec wrocil\n");
        sys_exit(1);
    }

    int kod = -1;
    int64_t kto = sys_wait(&kod);

    int n = put(0, "[spawn64] doczekalem dziecka ");
    n = putn(n, kto);
    n = put(n, ", kod wyjscia ");
    n = putn(n, kod);
    buf[n++] = '\n';
    buf[n] = 0;
    print(buf);

    /* hello64 konczy sie siodemka - jesli to widzimy, przeszla cala
       droga: fork, exec, wait */
    int ok = (kto == dziecko && kod == 7);
    print(ok ? "[spawn64] fork + exec + wait dzialaja\n"
             : "[spawn64] BLAD: nie ten numer albo nie ten kod\n");

    /* Nie ma juz na co czekac - drugie wait musi to powiedziec */
    int64_t brak = sys_wait(0);
    if (brak != -1) {
        print("[spawn64] BLAD: wait wymyslil sobie dziecko\n");
        ok = 0;
    }

    sys_exit(ok ? 0 : 1);
}
