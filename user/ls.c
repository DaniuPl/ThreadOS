/* ls - prints the entries of the working directory. Directories end
 * with '/'.
 * The enumeration is provided by the kernel through the sys_readdir
 * syscall (index-based).
 */

#include "usys.h"

void _start(void)
{
    int found = 0;
    for (int i = 0;; i++) {
        char name[FS_NAME_MAX + 1];
        int32_t r = sys_readdir(i, name);
        if (r == -1)
            break;                 /* the end of the entries */
        print(name);
        if (r == -2) {             /* a directory */
            print("/\n");
        } else {
            print("  ");
            print_dec((uint32_t)r);
            print(" B\n");
        }
        found++;
    }
    if (!found)
        print("(pusto)\n");
    sys_exit(0);
}
