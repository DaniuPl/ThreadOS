/* spin - dlugo zyjacy proces: drukuje znacznik i spi w petli.
 * Sluzy do demonstracji 'kill': uruchamiany w tle ('spin &'),
 * konczy sie dopiero po zabiciu (kill <pid>).
 */

#include "usys.h"

void _start(void)
{
    for (;;) {
        print("[spin] tik\n");
        sys_sleep(300);
    }
}
