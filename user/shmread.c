/* shmread - the shared memory reader
 *
 * It maps the same shm region (key 42) as shmtest and prints the value
 * another process wrote there. The same physical place in memory.
 */

#include "usys.h"

void _start(void)
{
    volatile uint32_t *p = (volatile uint32_t *)sys_shm(42, 4096);
    if (!p) {
        print("[shmread] there is no shared memory\n");
        sys_exit(1);
    }
    print("[shmread] value=");
    print_dec(p[0]);
    print("\n");
    sys_exit(0);
}
