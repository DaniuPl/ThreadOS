/* cat - prints a file from disk to the console (a pure fd model: read
 * the file -> write to fd 1). Started from ush: 'cat <file>'.
 */

#include "usys.h"

void _start(int argc, char **argv)
{
    if (argc < 2) {
        print("usage: cat <file>\n");
        sys_exit(1);
    }

    int fd = sys_open(argv[1], O_READ);
    if (fd < 0) {
        print("cat: I cannot open '");
        print(argv[1]);
        print("'\n");
        sys_exit(1);
    }

    char buf[256];
    int n;
    while ((n = sys_read(fd, buf, sizeof(buf))) > 0)
        sys_write(STDOUT, buf, (uint32_t)n);

    sys_close(fd);
    sys_exit(0);
}
