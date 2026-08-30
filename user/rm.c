/* rm - removes a file from the disk (sys_unlink). */

#include "usys.h"

void _start(int argc, char **argv)
{
    if (argc < 2) {
        print("uzycie: rm <plik>\n");
        sys_exit(1);
    }
    if (sys_unlink(argv[1]) < 0) {
        print("rm: nie ma pliku '");
        print(argv[1]);
        print("'\n");
        sys_exit(1);
    }
    sys_exit(0);
}
