/* grep - passes through from stdin only the lines containing a pattern
 * (argv[1]).
 * A pipeline filter: 'seq 20 | grep 1', 'cat file | grep word'.
 */

#include "usys.h"

#define LINE_MAX 256

static char line[LINE_MAX];
static int len;
static int matched;             /* was anything matched */

static void flush_line(const char *pattern)
{
    line[len] = '\0';
    if (ustrstr(line, pattern)) {
        sys_write(STDOUT, line, (uint32_t)len);
        sys_write(STDOUT, "\n", 1);
        matched = 1;
    }
    len = 0;
}

void _start(int argc, char **argv)
{
    if (argc < 2) {
        print("usage: grep <pattern>\n");
        sys_exit(1);
    }

    char buf[128];
    int n;
    while ((n = sys_read(STDIN, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n; i++) {
            if (buf[i] == '\n')
                flush_line(argv[1]);
            else if (len < LINE_MAX - 1)
                line[len++] = buf[i];
        }
    }
    if (len)            /* the last line with no newline */
        flush_line(argv[1]);

    sys_exit(matched ? 0 : 1);  /* like the real grep: 1 = no matches */
}
