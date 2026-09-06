/* hello64 - the first real ProtonOS program in long mode
 *
 * Built separately, put into the initrd, read by the kernel only after
 * startup. The kernel knows nothing about it beyond what it reads from
 * the ELF: where to start and what permissions the segments have.
 */

#include "usys64.h"

static char line[80];

/* Assembling the string by hand, because there is no library here -
   not a single function beyond the ones we write ourselves */
static void say(const char *prefix, uint64_t value)
{
    char num[24];
    int n = 0;

    for (const char *p = prefix; *p; p++)
        line[n++] = *p;
    utoa(value, num);
    for (int k = 0; num[k]; k++)
        line[n++] = num[k];
    line[n++] = '\n';
    line[n] = 0;
    print(line);
}

void _start(void)
{
    print("[hello64] hello, I am a 64-bit ELF from the initrd\n");

    /* Proof that a file read from the initrd really runs in ring 3 */
    sys_report_ring();

    uint64_t start = sys_ticks();
    for (int i = 1; i <= 3; i++) {
        say("[hello64] counting: ", (uint64_t)i);
        sys_sleep(50);
    }
    say("[hello64] ticks gone by: ", sys_ticks() - start);

    print("[hello64] ending politely\n");
    sys_exit(7);
}
