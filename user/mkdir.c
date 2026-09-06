/* mkdir - tworzy katalog (sys_mkdir). */

#include "usys.h"

void _start(int argc, char **argv)
{
    if (argc < 2) {
        print("usage: mkdir <directory>\n");
        sys_exit(1);
    }
    if (sys_mkdir(argv[1]) < 0) {
        print("mkdir: I cannot create '");
        print(argv[1]);
        print("'\n");
        sys_exit(1);
    }
    sys_exit(0);
}
