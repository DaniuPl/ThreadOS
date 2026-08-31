/* cp - copies a file (src -> dst) through file descriptors. */

#include "usys.h"

void _start(int argc, char **argv)
{
    if (argc < 3) {
        print("uzycie: cp <zrodlo> <cel>\n");
        sys_exit(1);
    }

    int src = sys_open(argv[1], O_READ);
    if (src < 0) {
        print("cp: nie moge otworzyc '");
        print(argv[1]);
        print("'\n");
        sys_exit(1);
    }
    int dst = sys_open(argv[2], O_WRITE | O_CREATE | O_TRUNC);
    if (dst == -2) {
        print("cp: brak uprawnien do '");
        print(argv[2]);
        print("' (sprobuj przez sudo)\n");
        sys_exit(2);
    }
    if (dst < 0) {
        print("cp: nie moge utworzyc '");
        print(argv[2]);
        print("'\n");
        sys_exit(1);
    }

    char buf[256];
    int n;
    while ((n = sys_read(src, buf, sizeof(buf))) > 0)
        sys_write(dst, buf, (uint32_t)n);

    sys_close(src);
    sys_close(dst);
    sys_exit(0);
}
