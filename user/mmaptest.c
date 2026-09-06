/* mmaptest - checks the mmap syscall: it maps a fresh area of memory,
 * writes a pattern, verifies it and prints the result. Proof that mmap
 * gives a process new, writable, zeroed pages.
 */

#include "usys.h"

#define LEN (3 * 4096)   /* 3 pages - it forces several mappings */

void _start(void)
{
    unsigned char *p = (unsigned char *)sys_mmap(LEN);
    if (!p) {
        print("[mmaptest] mmap returned NULL - an error!\n");
        sys_exit(1);
    }

    /* fresh memory is to be zeroed */
    for (int i = 0; i < LEN; i++)
        if (p[i] != 0) {
            print("[mmaptest] the memory is not zeroed!\n");
            sys_exit(1);
        }

    for (int i = 0; i < LEN; i++)
        p[i] = (unsigned char)(i * 7 + 1);
    for (int i = 0; i < LEN; i++)
        if (p[i] != (unsigned char)(i * 7 + 1)) {
            print("[mmaptest] ERROR: a damaged byte!\n");
            sys_exit(1);
        }

    print("[mmaptest] OK: 3 pages through mmap written and verified\n");
    sys_exit(0);
}
