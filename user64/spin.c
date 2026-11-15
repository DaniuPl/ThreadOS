/* spin - a long-lived process that keeps talking: it prints a marker
 * and sleeps, in a loop, until somebody stops it.
 *
 * spin64 says one line and goes quiet; this one says a line every 300
 * ms, so a terminal can be seen showing output while the program is
 * still running - and seen to stop showing it once it is killed.
 */

#include "usys64.h"

void _start(void)
{
    for (;;) {
        print("[spin] tick\n");
        sys_sleep(300);
    }
}
