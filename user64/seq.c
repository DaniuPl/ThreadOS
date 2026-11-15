/* seq - prints the numbers 1..N (argv[1]), each on its own line.
 *
 * A convenient pipeline producer: 'seq 5 | wc', 'seq 9 | head -n 3' -
 * and a command whose output a person can predict line by line, which
 * is what the terminal's tests type.
 */

#include "usys64.h"
#include "compat64.h"

void _start(int argc, char **argv)
{
    int64_t n = argc > 1 ? uatoi(argv[1]) : 0;
    for (int64_t i = 1; i <= n; i++) {
        print_dec((uint32_t)i);
        print("\n");
    }
    sys_exit(n < 0 ? 1 : 0);
}
