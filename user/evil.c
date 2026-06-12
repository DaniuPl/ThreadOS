/* evil - niegrzeczny program: siega po pamiec jadra i ginie */

#include "usys.h"

void _start(void)
{
    print("[evil] czesc, zaraz przeczytam pamiec jadra pod 0x100000...\n");
    sys_sleep(100);

    volatile uint32_t probe = *(volatile uint32_t *)0x100000;
    (void)probe;

    /* Tu nie docieramy - jadro ubija proces page faultem */
    print("[evil] TEGO NIE POWINNO BYC WIDAC\n");
    sys_exit(1);
}
