/* spin64 - a program with no handler, in an endless loop
 *
 * It cannot be ended from the inside and that is not the point: the
 * point is that it can be ended from the outside. Without signals a
 * program like this would take the console away forever.
 */

#include "usys64.h"

void _start(void)
{

    print("[spin64] spinning with no end - stop me with Ctrl+C\n");
    for (;;)
        sys_sleep(50);
}
