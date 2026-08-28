/* uecho64 - wypisuje swoje argumenty
 *
 * Najprostszy program, jaki mozna napisac, i zarazem jedyny dowod,
 * ze argumenty naprawde dochodza: nie robi nic poza pokazaniem tego,
 * co dostal. Do tej pory 'echo' musialo byc wbudowane w shell, bo nie
 * bylo jak przekazac programowi ani jednego slowa.
 */

#include "usys64.h"

void _start(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        print(argv[i]);
        if (i + 1 < argc)
            print(" ");
    }
    print("\n");
    sys_exit(0);
}
