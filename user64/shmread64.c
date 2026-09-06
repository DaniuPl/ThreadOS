/* shmread64 - the shared memory reader
 *
 * It maps the same region (key 42) as shmwrite64 and prints the value
 * ANOTHER process wrote there. Then it writes back into the same place,
 * so that it can be seen the road goes both ways.
 */

#include "usys64.h"

#define KEY  42
#define PATTERN 12345

static char out[96];

static void report(const char *a, uint64_t v)
{
    int n = 0;
    while (*a)
        out[n++] = *a++;
    char tmp[24];
    utoa(v, tmp);
    for (int i = 0; tmp[i]; i++)
        out[n++] = tmp[i];
    out[n++] = '\n';
    out[n] = '\0';
    print(out);
}

void _start(void)
{
    volatile uint32_t *p = sys_shm(KEY, 4096);
    if (!p) {
        print("[shmread64] there is no shared memory\n");
        sys_exit(1);
    }

    report("[shmread64] the region at address ", (uint64_t)(uintptr_t)p);
    report("[shmread64] value=", p[0]);

    if (p[0] != PATTERN) {
        print("[shmread64] WRONG: that is not the value\n");
        sys_exit(1);
    }
    p[2] = PATTERN + 1;              /* the answer by the same road */
    print("[shmread64] it matches - two processes, one memory\n");
    sys_exit(0);
}
