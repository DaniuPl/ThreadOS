/* head - przepuszcza tylko pierwsze N wierszy ze stdin (argv[1], domyslnie 10).
 * Po wypisaniu limitu zamyka sie - producent w potoku dostanie wtedy
 * przerwany potok i tez zakonczy zapis.
 */

#include "usys.h"

void _start(int argc, char **argv)
{
    int limit = argc > 1 ? uatoi(argv[1]) : 10;
    int lines = 0;
    char buf[128];
    int n;

    while (lines < limit && (n = sys_read(STDIN, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n && lines < limit; i++) {
            sys_write(STDOUT, &buf[i], 1);
            if (buf[i] == '\n')
                lines++;
        }
    }
    sys_exit(0);
}
