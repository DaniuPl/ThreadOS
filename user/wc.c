/* wc - liczy bajty i wiersze ze stdin do EOF, wypisuje wynik na stdout.
 * Typowy konsument potoku: 'echo cos | wc'.
 */

#include "usys.h"

static void print_dec(uint32_t v)
{
    char buf[12];
    int n = 0;
    do {
        buf[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        sys_write(STDOUT, &buf[--n], 1);
}

void _start(void)
{
    uint32_t bytes = 0, lines = 0;
    char buf[128];
    int n;

    /* read(STDIN) zwraca 0, gdy potok zostanie zamkniety (EOF) */
    while ((n = sys_read(STDIN, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n; i++) {
            bytes++;
            if (buf[i] == '\n')
                lines++;
        }
    }

    print("wc: wierszy=");
    print_dec(lines);
    print(" bajtow=");
    print_dec(bytes);
    print("\n");
    sys_exit(0);
}
