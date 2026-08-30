/* mv - moves a file: it copies src -> dst, then removes src.
 * (PFS has no separate rename, so we make a copy and delete the
 * original.)
 */

#include "usys.h"

void _start(int argc, char **argv)
{
    if (argc < 3) {
        print("uzycie: mv <zrodlo> <cel>\n");
        sys_exit(1);
    }

    int src = sys_open(argv[1], O_READ);
    if (src < 0) {
        print("mv: nie ma pliku '");
        print(argv[1]);
        print("'\n");
        sys_exit(1);
    }
    int dst = sys_open(argv[2], O_WRITE | O_CREATE | O_TRUNC);
    if (dst < 0) {
        print("mv: nie moge utworzyc '");
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
    sys_unlink(argv[1]);            /* remove the original */
    sys_exit(0);
}
