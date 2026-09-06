/* memtest - proof that sbrk/malloc gives a process growing memory
 *
 * It allocates several blocks through malloc (built on sys_sbrk),
 * writes a pattern and checks whether it survived - that is, that the
 * pages really were mapped and are writable from ring 3.
 */

#include "usys.h"

#define BLOCKS    8
#define BLOCK_LEN 4096   /* one page each - it forces brk to grow */

void _start(void)
{
    print("[memtest] heap start: ");
    print_dec((uint32_t)sys_sbrk(0));
    print("\n");

    unsigned char *blk[BLOCKS];

    for (int b = 0; b < BLOCKS; b++) {
        blk[b] = (unsigned char *)malloc(BLOCK_LEN);
        if (!blk[b]) {
            print("[memtest] malloc returned NULL - an error!\n");
            sys_exit(1);
        }
        for (int i = 0; i < BLOCK_LEN; i++)
            blk[b][i] = (unsigned char)(b * 31 + i);
    }

    print("[memtest] end of the heap: ");
    print_dec((uint32_t)sys_sbrk(0));
    print("\n");

    /* Verifying the pattern - every byte has to be intact */
    for (int b = 0; b < BLOCKS; b++)
        for (int i = 0; i < BLOCK_LEN; i++)
            if (blk[b][i] != (unsigned char)(b * 31 + i)) {
                print("[memtest] ERROR: a damaged byte!\n");
                sys_exit(1);
            }

    print("[memtest] OK: ");
    print_dec(BLOCKS);
    print(" blocks of ");
    print_dec(BLOCK_LEN);
    print(" B written and verified\n");
    sys_exit(0);
}
