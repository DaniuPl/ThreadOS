/* rm - removes a file from the disk (sys_unlink). */

#include "usys.h"

void _start(int argc, char **argv)
{
    if (argc < 2) {
        print("uzycie: rm <plik>\n");
        sys_exit(1);
    }
    int32_t rc = sys_unlink(argv[1]);
    if (rc == -2) {
        /* Not allowed is a different answer from 'there is no such
           file', and a person acts differently on each */
        print("rm: brak uprawnien do '");
        print(argv[1]);
        print("' (sprobuj przez sudo)\n");
        sys_exit(2);
    }
    if (rc < 0) {
        print("rm: nie ma pliku '");
        print(argv[1]);
        print("'\n");
        sys_exit(1);
    }
    sys_exit(0);
}
