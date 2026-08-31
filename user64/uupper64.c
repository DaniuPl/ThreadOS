/* uupper64 - copies input to output in capital letters
 *
 * It knows no file and no other program: it reads from number zero and
 * writes to number one. That on the other side there sits another
 * program rather than a keyboard and a screen is the business of
 * whoever started it - and the program has no way of noticing.
 *
 * It ends when a read returns zero, that is, when the last writer
 * disappears.
 */

#include "usys64.h"

static char buf[256];

void _start(void)
{
    int64_t n;

    while ((n = sys_read(FD_STDIN, buf, sizeof(buf))) > 0) {
        for (int64_t i = 0; i < n; i++)
            if (buf[i] >= 'a' && buf[i] <= 'z')
                buf[i] = (char)(buf[i] - 'a' + 'A');
        sys_write(FD_STDOUT, buf, (uint64_t)n);
    }
    sys_exit(0);
}
