/* shmtest - the shared memory writer
 *
 * It maps an shm region (key 42), writes a known value, then starts
 * 'shmread' - a separate process which maps the same key and reads the
 * value. Proof that two processes see the same memory.
 */

#include "usys.h"

void _start(void)
{
    volatile uint32_t *p = (volatile uint32_t *)sys_shm(42, 4096);
    if (!p) {
        print("[shmtest] brak pamieci wspoldzielonej\n");
        sys_exit(1);
    }
    p[0] = 12345;
    print("[shmtest] zapisano 12345 do shm, uruchamiam shmread\n");

    int32_t pid = sys_spawn("shmread", 0, 0);
    if (pid >= 0)
        sys_wait(pid);
    sys_exit(0);
}
