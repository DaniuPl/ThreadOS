/* seq - wypisuje liczby 1..N (argv[1]), kazda w osobnym wierszu.
 * Wygodny producent potoku: 'seq 5 | wc', 'seq 9 | grep 3'.
 */

#include "usys.h"

void _start(int argc, char **argv)
{
    int n = argc > 1 ? uatoi(argv[1]) : 0;
    for (int i = 1; i <= n; i++) {
        print_dec((uint32_t)i);
        print("\n");
    }
    sys_exit(0);
}
