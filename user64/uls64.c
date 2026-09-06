/* uls64 - prints the contents of the working directory
 *
 * The kernel has 'fls' built in, because until recently there was no
 * other way. This one does the same, but is an ordinary program and
 * asks for the entries in turn - one per call, as long as it keeps
 * getting an answer.
 *
 * The working directory belongs to the task, so uls64 prints the one
 * the shell stood in when it started it.
 */

#include "usys64.h"

static char out[128];

static int put(int n, const char *s)
{
    while (*s)
        out[n++] = *s++;
    return n;
}

static int putn(int n, uint64_t v)
{
    char tmp[24];
    utoa(v, tmp);
    return put(n, tmp);
}

void _start(void)
{
    char path[128];
    struct dirent64 entry;

    if (sys_getcwd(path, sizeof(path)) < 0) {
        print("uls64: I do not know where I am\n");
        sys_exit(1);
    }

    int n = put(0, "uls64: ");
    n = put(n, path);
    n = put(n, "\n");
    out[n] = 0;
    print(out);

    int count = 0;
    for (int i = 0; sys_readdir(i, &entry) == 0; i++) {
        n = put(0, "  ");
        n = put(n, entry.name);
        if (entry.is_dir) {
            n = put(n, "/  (directory)");
        } else {
            n = put(n, "  (");
            n = putn(n, entry.size);
            n = put(n, " B)");
        }
        n = put(n, "\n");
        out[n] = 0;
        print(out);
        count++;
    }

    n = put(0, "uls64: entries ");
    n = putn(n, (uint64_t)count);
    n = put(n, "\n");
    out[n] = 0;
    print(out);
    sys_exit(0);
}
