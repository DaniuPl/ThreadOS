/* uecho64 - prints its arguments
 *
 * The simplest program one can write, and at the same time the only
 * proof that arguments really do arrive: it does nothing beyond showing
 * what it was given. Until now 'echo' had to be built into the shell,
 * because there was no way of passing a program a single word.
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
