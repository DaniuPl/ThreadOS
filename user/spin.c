/* spin - a long-lived process: it prints a marker and sleeps in a loop.
 * It serves to demonstrate 'kill': started in the background
 * ('spin &'), it ends only once killed (kill <pid>).
 */

#include "usys.h"

void _start(void)
{
    for (;;) {
        print("[spin] tick\n");
        sys_sleep(300);
    }
}
