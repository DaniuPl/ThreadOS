/* evil - a naughty program: it reaches for kernel memory and dies */

#include "usys.h"

void _start(void)
{
    print("[evil] hello, I am about to read kernel memory at 0x100000...\n");
    sys_sleep(100);

    volatile uint32_t probe = *(volatile uint32_t *)0x100000;
    (void)probe;

    /* We do not get here - the kernel kills the process with a page
   fault */
    print("[evil] THIS SHOULD NOT BE VISIBLE\n");
    sys_exit(1);
}
