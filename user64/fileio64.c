/* fileio64 - a file as seen from a program
 *
 * Until now only the kernel reached for the disk. This program does it
 * itself: it creates a file, writes to it, closes it, opens it again
 * and reads. It has no special rights while doing so - a descriptor is
 * an ordinary number in its own table, and the inode that number points
 * at is known only to the kernel.
 *
 * It checks itself: if the read handed back anything other than what
 * was written, it ends with an error rather than a message somebody has
 * to read.
 */

#include "usys64.h"

static const char BODY[] = "ProtonOS 64: a file written from ring 3\n";
static char buf[128];
static char out[160];

static int put(int n, const char *s)
{
    while (*s)
        out[n++] = *s++;
    return n;
}

static int putn(int n, int64_t v)
{
    char tmp[24];
    if (v < 0) {
        out[n++] = '-';
        v = -v;
    }
    utoa((uint64_t)v, tmp);
    return put(n, tmp);
}

static void err(const char *co, int64_t v)
{
    int n = put(0, "[fileio64] ERROR: ");
    n = put(n, co);
    n = put(n, " -> ");
    n = putn(n, v);
    out[n++] = '\n';
    out[n] = 0;
    print(out);
    sys_exit(1);
}

void _start(void)
{

    uint64_t len = ustrlen(BODY);

    print("[fileio64] creating a file and writing into it from ring 3\n");
    int64_t fd = sys_open("zring3", 1);
    if (fd < 0)
        err("open for writing", fd);

    int64_t n = sys_write((int)fd, BODY, len);
    if (n != (int64_t)len)
        err("write", n);
    sys_close((int)fd);

    /* The second open starts from the beginning of the file - the
       position the previous descriptor stood at disappeared along with
       it */
    print("[fileio64] opening it again and reading\n");
    fd = sys_open("zring3", 0);
    if (fd < 0)
        err("open for reading", fd);

    n = sys_read((int)fd, buf, sizeof(buf));
    sys_close((int)fd);
    if (n != (int64_t)len)
        err("read gave back another length", n);

    for (uint64_t i = 0; i < len; i++)
        if (buf[i] != BODY[i])
            err("the content does not match at position", (int64_t)i);

    int m = put(0, "[fileio64] written and read back match byte for byte (");
    m = putn(m, (int64_t)len);
    m = put(m, " B)\n");
    out[m] = 0;
    print(out);

    /* A file is to be missing only when it is not there */
    if (sys_open("no-such-file", 0) >= 0)
        err("open invented a file", 0);
    print("[fileio64] the kernel does not open a file that is not there\n");

    sys_exit(0);
}
