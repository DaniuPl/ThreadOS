/* ucat64 - prints the file given in the argument
 *
 * The kernel's shell has 'cat' built in, because until recently there
 * was no other way. This one does the same, but is an ordinary program:
 * it gets the file name as an argument, opens it with a descriptor and
 * reads. The kernel knows nothing about it beyond that it is to start
 * it.
 */

#include "usys64.h"

static char buf[256];

void _start(int argc, char **argv)
{
    if (argc < 2) {
        print("ucat64: give a file name\n");
        sys_exit(1);
    }

    int64_t fd = sys_open(argv[1], 0);
    if (fd < 0) {
        print("ucat64: I cannot open '");
        print(argv[1]);
        print("'\n");
        sys_exit(1);
    }

    /* We read in portions to the end - the descriptor remembers where
       it stopped by itself, so the next read starts where the previous
       one finished */
    int64_t n;
    while ((n = sys_read((int)fd, buf, sizeof(buf))) > 0)
        sys_write(FD_STDOUT, buf, (uint64_t)n);

    sys_close((int)fd);
    sys_exit(0);
}
