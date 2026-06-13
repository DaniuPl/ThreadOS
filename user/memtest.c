/* memtest - dowod, ze sbrk/malloc daje procesowi rosnaca pamiec
 *
 * Alokuje kilka blokow przez malloc (oparte o sys_sbrk), zapisuje
 * wzorzec i sprawdza, czy przetrwal - czyli ze strony zostaly
 * naprawde zmapowane i sa zapisywalne z ring 3.
 */

#include "usys.h"

#define BLOCKS    8
#define BLOCK_LEN 4096   /* po jednej stronie kazdy - wymusza wzrost brk */

static void print_dec(uint32_t v)
{
    char buf[12];
    int n = 0;
    do {
        buf[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        sys_write(STDOUT, &buf[--n], 1);
}

void _start(void)
{
    print("[memtest] start sterty: ");
    print_dec((uint32_t)sys_sbrk(0));
    print("\n");

    unsigned char *blk[BLOCKS];

    for (int b = 0; b < BLOCKS; b++) {
        blk[b] = (unsigned char *)malloc(BLOCK_LEN);
        if (!blk[b]) {
            print("[memtest] malloc zwrocil NULL - blad!\n");
            sys_exit(1);
        }
        for (int i = 0; i < BLOCK_LEN; i++)
            blk[b][i] = (unsigned char)(b * 31 + i);
    }

    print("[memtest] koniec sterty:  ");
    print_dec((uint32_t)sys_sbrk(0));
    print("\n");

    /* Weryfikacja wzorca - kazdy bajt musi byc nienaruszony */
    for (int b = 0; b < BLOCKS; b++)
        for (int i = 0; i < BLOCK_LEN; i++)
            if (blk[b][i] != (unsigned char)(b * 31 + i)) {
                print("[memtest] BLAD: uszkodzony bajt!\n");
                sys_exit(1);
            }

    print("[memtest] OK: ");
    print_dec(BLOCKS);
    print(" blokow po ");
    print_dec(BLOCK_LEN);
    print(" B zapisanych i zweryfikowanych\n");
    sys_exit(0);
}
