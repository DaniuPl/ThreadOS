/* evil - a naughty program: it reaches for kernel memory and dies */

#include "usys.h"

void _start(void)
{
    print("[evil] czesc, zaraz przeczytam pamiec jadra pod 0x100000...\n");
    sys_sleep(100);

    volatile uint32_t probe = *(volatile uint32_t *)0x100000;
    (void)probe;

    /* We do not get here - the kernel kills the process with a page
   fault */
    print("[evil] TEGO NIE POWINNO BYC WIDAC\n");
    sys_exit(1);
}
