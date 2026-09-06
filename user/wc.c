/* wc - counts bytes and lines from stdin until EOF and prints the
 * result to stdout.
 * A typical pipeline consumer: 'echo something | wc'.
 */

#include "usys.h"

void _start(void)
{
    uint32_t bytes = 0, lines = 0;
    char buf[128];
    int n;

    /* read(STDIN) returns 0 once the pipe is closed (EOF) */
    while ((n = sys_read(STDIN, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n; i++) {
            bytes++;
            if (buf[i] == '\n')
                lines++;
        }
    }

    print("wc: lines=");
    print_dec(lines);
    print(" bytes=");
    print_dec(bytes);
    print("\n");
    sys_exit(0);
}
