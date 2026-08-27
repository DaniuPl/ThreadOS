/* ptree - drzewo procesow: fork, wait na dowolne dziecko, sieroty
 *
 * Rodzic rozwidla sie trzy razy. Kazde dziecko spi inna chwile
 * i konczy sie wlasnym kodem, wiec koncza sie w odwrotnej kolejnosci,
 * niz powstaly. Rodzic zbiera je przez sys_waitany() - nie musi
 * zgadywac, kto skonczy pierwszy - a na koniec sprawdza, ze gdy dzieci
 * juz nie ma, waitany mowi o tym wprost (-1), zamiast czekac w
 * nieskonczonosc.
 *
 * Druga czesc sprawdza sieroty: dziecko rozwidla wnuka i ginie
 * natychmiast, zostawiajac go samego. Wnuk zostaje przygarniety przez
 * dziadka, wiec jego kod wyjscia odbiera ten sam waitany - inaczej
 * przepadlby, a wnuk nie mialby komu zdac raportu.
 */

#include "usys.h"

#define KIDS 3

void _start(void)
{
    int32_t pid[KIDS];

    for (int i = 0; i < KIDS; i++) {
        int32_t p = sys_fork();
        if (p < 0) {
            print("[ptree] fork sie nie udal\n");
            sys_exit(1);
        }
        if (p == 0) {
            /* Ostatnie dziecko spi najkrocej - skonczy jako pierwsze */
            sys_sleep((uint32_t)(KIDS - i) * 300);
            sys_exit(20 + i);
        }
        pid[i] = p;
    }

    print("[ptree] rodzic ma pid ");
    print_dec((uint32_t)sys_getpid());
    print(", dzieci: ");
    for (int i = 0; i < KIDS; i++) {
        print_dec((uint32_t)pid[i]);
        print(i == KIDS - 1 ? "\n" : " ");
    }

    int got = 0, wrong = 0;
    for (;;) {
        int32_t code = -1;
        int32_t who = sys_waitany(&code);
        if (who < 0)
            break;                      /* nie ma juz dzieci */

        /* Kod wyjscia musi pasowac do tego, kim jest dziecko */
        int idx = -1;
        for (int i = 0; i < KIDS; i++)
            if (pid[i] == who)
                idx = i;
        if (idx < 0 || code != 20 + idx)
            wrong++;

        print("[ptree] skonczylo dziecko ");
        print_dec((uint32_t)who);
        print(" z kodem ");
        print_dec((uint32_t)code);
        print("\n");
        got++;
    }

    if (got != KIDS || wrong) {
        print("[ptree] zle: zebranych dzieci ");
        print_dec((uint32_t)got);
        print(", niezgodnych kodow ");
        print_dec((uint32_t)wrong);
        print("\n");
        sys_exit(1);
    }
    print("[ptree] zebrane wszystkie dzieci, waitany zglosil koniec\n");

    /* Sierota: dziecko robi wnuka i od razu ginie */
    int32_t mid = sys_fork();
    if (mid < 0) {
        print("[ptree] fork sieroty sie nie udal\n");
        sys_exit(1);
    }
    if (mid == 0) {
        int32_t grand = sys_fork();
        if (grand == 0) {
            sys_sleep(500);         /* przezyje wlasnego rodzica */
            sys_exit(99);
        }
        print("[ptree] dziecko robi wnuka ");
        print_dec((uint32_t)grand);
        print(" i konczy sie\n");
        sys_exit(30);
    }

    int seen_child = 0, seen_orphan = 0;
    for (;;) {
        int32_t code = -1;
        int32_t who = sys_waitany(&code);
        if (who < 0)
            break;
        if (code == 30 && who == mid)
            seen_child = 1;
        else if (code == 99)
            seen_orphan = 1;         /* wnuka przygarnal dziadek */
        print("[ptree] odebrane od ");
        print_dec((uint32_t)who);
        print(" z kodem ");
        print_dec((uint32_t)code);
        print("\n");
    }
    if (!seen_child || !seen_orphan) {
        print("[ptree] zle: sierota nie trafila pod opieke dziadka\n");
        sys_exit(1);
    }
    print("[ptree] sierota przygarnieta przez dziadka\n");
    sys_exit(0);
}
