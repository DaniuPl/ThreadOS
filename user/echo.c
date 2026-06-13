/* echo - wypisuje swoje argumenty (dowod, ze argc/argv dziala) */

#include "usys.h"

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
