/* hello64 - pierwszy prawdziwy program ProtonOS w trybie dlugim
 *
 * Zbudowany osobno, wlozony do initrd, wczytany przez jadro dopiero
 * po starcie. Jadro nie wie o nim nic ponad to, co przeczyta z ELF-a:
 * gdzie zaczac i jakie prawa maja segmenty.
 */

#include "usys64.h"

static char line[80];

/* Skladanie napisu recznie, bo biblioteki tu nie ma zadnej -
   ani jednej funkcji poza tymi, ktore sami napiszemy */
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
    print("[hello64] czesc, jestem ELF-em 64-bitowym z initrd\n");

    /* Dowod, ze plik wczytany z initrd naprawde dziala w ring 3 */
    sys_report_ring();

    uint64_t start = sys_ticks();
    for (int i = 1; i <= 3; i++) {
        say("[hello64] licze: ", (uint64_t)i);
        sys_sleep(50);
    }
    say("[hello64] uplynelo tyknac: ", sys_ticks() - start);

    print("[hello64] koncze sie grzecznie\n");
    sys_exit(7);
}
