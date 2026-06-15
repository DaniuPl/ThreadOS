/* mmaptest - sprawdza syscall mmap: mapuje swiezy obszar pamieci,
 * zapisuje wzorzec, weryfikuje i wypisuje wynik. Dowod, ze mmap daje
 * procesowi nowe, zapisywalne, wyzerowane strony.
 */

#include "usys.h"

#define LEN (3 * 4096)   /* 3 strony - wymusza kilka mapowan */

void _start(void)
{
    unsigned char *p = (unsigned char *)sys_mmap(LEN);
    if (!p) {
        print("[mmaptest] mmap zwrocil NULL - blad!\n");
        sys_exit(1);
    }

    /* swieza pamiec ma byc wyzerowana */
    for (int i = 0; i < LEN; i++)
        if (p[i] != 0) {
            print("[mmaptest] pamiec nie jest wyzerowana!\n");
            sys_exit(1);
        }

    for (int i = 0; i < LEN; i++)
        p[i] = (unsigned char)(i * 7 + 1);
    for (int i = 0; i < LEN; i++)
        if (p[i] != (unsigned char)(i * 7 + 1)) {
            print("[mmaptest] BLAD: uszkodzony bajt!\n");
            sys_exit(1);
        }

    print("[mmaptest] OK: 3 strony przez mmap zapisane i zweryfikowane\n");
    sys_exit(0);
}
