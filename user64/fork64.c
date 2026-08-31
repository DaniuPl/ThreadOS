/* fork64 - duplicating a process as seen from a program
 *
 * The program writes a number into its variable and duplicates itself.
 * From this moment there are two processes with the same memory - but a
 * COPY, not the same one. The child writes its own number, the parent
 * waits and checks whether its own survived.
 *
 * The parent and the child return from fork() to the same place, with
 * the same processor state - one value tells them apart: the child gets
 * zero, the parent the child's number.
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
        /* The child. It overwrites the value in its own copy - the parent
           has no way of seeing that, because it got its own copy of the
           page. */
        value = 200;
        int n = put(0, "[fork64] dziecko: fork dal 0, moja wartosc to ");
        n = putn(n, value);
        buf[n++] = '\n';
        buf[n] = 0;
        print(buf);
        sys_exit(0);
    }

    /* The parent. It gives the child time to write and checks its own
   memory. */
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
