/* mv - gives a file another name.
 *
 * It used to copy the whole thing and delete the original, because the
 * filesystem had no rename: moving four megabytes meant reading and
 * writing four megabytes to change sixty-four bytes of a directory
 * entry. Now there is a rename, and the blocks stay where they are.
 *
 * The copy is still here as the second try. A rename fails when the
 * target name is taken - and 'mv over an existing file' is a thing
 * people do - so in that case we fall back to what this program always
 * did, which overwrites.
 */

#include "usys.h"

void _start(int argc, char **argv)
{
    if (argc < 3) {
        print("usage: mv <source> <target>\n");
        sys_exit(1);
    }

    /* The cheap way first: one entry changed, no bytes moved. */
    if (sys_rename(argv[1], argv[2]) == 0)
        sys_exit(0);

    int src = sys_open(argv[1], O_READ);
    if (src < 0) {
        print("mv: there is no file '");
        print(argv[1]);
        print("'\n");
        sys_exit(1);
    }
    int dst = sys_open(argv[2], O_WRITE | O_CREATE | O_TRUNC);
    if (dst < 0) {
        print("mv: I cannot create '");
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
