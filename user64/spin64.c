/* spin64 - program bez procedury obslugi, w petli bez konca
 *
 * Nie da sie go zakonczyc od srodka i nie o to chodzi: chodzi o to,
 * ze da sie go zakonczyc od zewnatrz. Bez sygnalow program taki jak
 * ten zabieralby konsole na zawsze.
 */

#include "usys64.h"

void _start(void)
{

    print("[spin64] krece sie bez konca - zatrzymaj mnie Ctrl+C\n");
    for (;;)
        sys_sleep(50);
}
