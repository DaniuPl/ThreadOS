/* shmwrite64 - the shared memory writer
 *
 * It maps the region with key 42, writes a known value and its own
 * process number into it, then starts 'shmread64' - a separate process
 * with its own address space. That one maps the same key and reads what
 * was written here.
 *
 * The proof lies in there being two processes: each sees the region at
 * its own address, and the bytes are the same, because the physical
 * frames are the same.
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
    /* First a region on the side, with a different key. It is needed
       for nothing but one thing: it shifts our mapping area, so we get
       the SHARED region at a different address than the reader. Where
       something is visible is decided by the address space, not by the
       memory - and only different addresses show that this really is
       sharing and not an accidental agreement of layouts. */
    sys_shm(7, 4096);

    volatile uint32_t *p = sys_shm(KEY, 4096);
    if (!p) {
        print("[shmwrite64] there is no shared memory\n");
        sys_exit(1);
    }

    p[0] = PATTERN;
    p[2] = 0;                        /* room for the reader's answer */
    report("[shmwrite64] the region at address ", (uint64_t)(uintptr_t)p);
    report("[shmwrite64] I wrote ", PATTERN);

    /* A child with its own address space - fork and exec, the way a
       shell does it */
    int64_t pid = sys_fork();
    if (pid == 0) {
        char *argv[2] = {"shmread64", 0};
        sys_exec("shmread64", argv);
        print("[shmwrite64] there is no 'shmread64'\n");
        sys_exit(1);
    }
    if (pid > 0) {
        int code = 0;
        sys_wait(&code);
    }

    /* The reader bounced the value back through the same place - if it
       arrived, it can be seen here now */
    if (p[2] != PATTERN + 1) {
        report("[shmwrite64] WRONG: the reader did not answer, it is ", p[2]);
        sys_exit(1);
    }
    print("[shmwrite64] the reader answered through the same memory\n");
    sys_exit(0);
}
