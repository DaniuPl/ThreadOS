/* fork64 - duplicating a process as seen from a program
 *
 * The program writes a number into its variable and duplicates itself.
 * From this moment there are two processes with the same memory - but a
 * COPY, not the same one. The child writes its own number, the parent
 * waits for it to end and checks whether its own survived.
 *
 * The parent and the child return from fork() to the same place, with
 * the same processor state - one value tells them apart: the child gets
 * zero, the parent the child's number.
 *
 * The second half is the write the program does not make itself. The
 * child has the KERNEL fill a page it still shares with its parent - a
 * read() from a pipe. A write from ring 3 splits the page through the
 * program's page fault. The kernel's write has no such second chance
 * to count on: the page has to be split when the system call checks
 * its buffer, or on the kernel's own fault - write protection holds in
 * ring 0 too. A kernel that did neither would turn the read away or
 * stop on the fault, and on a core where ring 0 writes through a
 * read-only page the bytes would land in the frame the parent sees.
 * The child reports the first, the parent the last.
 */

#include "usys64.h"

#define PAGE 4096
#define MSG "written by the kernel"
#define MSG_LEN (sizeof(MSG) - 1)
#define PARENTS 'P'                 /* what the parent fills its page with */

static volatile uint64_t value;
static char buf[128];

/* A page of its own, so that the only write that splits it after the
   fork is the kernel's: sharing it with 'value' or 'buf' would let the
   child's own writes split it first, and the kernel would then write
   into a page that is private already. */
static char shared[PAGE] __attribute__((aligned(PAGE)));

static int put(int n, const char *s)
{
    while (*s)
        buf[n++] = *s++;
    return n;
}

static int putn(int n, uint64_t v)
{
    char tmp[24];
    utoa(v, tmp);
    return put(n, tmp);
}

static void say(int n)
{
    buf[n++] = '\n';
    buf[n] = 0;
    print(buf);
}

/* The child's side of the kernel write. Returns 0 when the kernel's
   bytes arrived and the rest of the page is still what the parent
   wrote before the fork. */
static int kernel_writes_child_page(void)
{
    int fd[2];
    if (sys_pipe(fd) < 0) {
        print("[fork64] child: ERROR, no pipe\n");
        return 1;
    }
    if (sys_write(fd[1], MSG, MSG_LEN) != (int64_t)MSG_LEN) {
        print("[fork64] child: ERROR, the pipe did not take the message\n");
        return 1;
    }
    /* The first touch of 'shared' since the fork, and it is the
       kernel's */
    if (sys_read(fd[0], shared, MSG_LEN) != (int64_t)MSG_LEN) {
        print("[fork64] child: ERROR, the read did not fill my page\n");
        return 1;
    }
    sys_close(fd[0]);
    sys_close(fd[1]);

    for (uint64_t i = 0; i < MSG_LEN; i++)
        if (shared[i] != MSG[i]) {
            print("[fork64] child: ERROR, the kernel's bytes are not in my page\n");
            return 1;
        }
    for (uint64_t i = MSG_LEN; i < PAGE; i++)
        if (shared[i] != PARENTS) {
            print("[fork64] child: ERROR, the rest of my copy is not the parent's\n");
            return 1;
        }
    print("[fork64] child: the kernel read '" MSG "' into my copy of the "
          "shared page, and the rest of it is the parent's\n");
    return 0;
}

void _start(void)
{
    value = 100;
    for (int i = 0; i < PAGE; i++)
        shared[i] = PARENTS;

    int64_t child = sys_fork();
    if (child < 0) {
        print("[fork64] ERROR: the fork did not work\n");
        sys_exit(1);
    }

    if (child == 0) {
        /* The child. Before it writes anything, it reads what it
           inherited: the copy has to hold the parent's number. */
        int n = put(0, "[fork64] child: the fork gave 0, I inherited ");
        n = putn(n, value);
        say(n);
        int bad = value != 100;

        /* It overwrites the value in its own copy - the parent has no
           way of seeing that, because it got its own copy of the
           page */
        value = 200;
        n = put(0, "[fork64] child: my value is ");
        n = putn(n, value);
        say(n);

        bad |= kernel_writes_child_page();
        sys_exit(bad);
    }

    /* The parent. It waits for the child to end - only then is
       everything the child could have written behind it - and checks
       its own memory. */
    int n = put(0, "[fork64] parent: the fork gave the child number ");
    n = putn(n, (uint64_t)child);
    say(n);

    int code = -1;
    int64_t waited = sys_wait(&code);
    n = put(0, "[fork64] parent: waited for child ");
    n = putn(n, (uint64_t)waited);
    n = put(n, code == 0 ? ", which ended with code 0"
                         : ", which ended with an error");
    say(n);
    int bad = waited != child || code != 0;

    n = put(0, value == 100 ? "[fork64] parent: still "
                              : "[fork64] parent: ERROR, now ");
    n = putn(n, value);
    n = put(n, value == 100 ? " - the child got a copy"
                              : " - the child is writing over my memory");
    say(n);
    bad |= value != 100;

    int kept = 1;
    for (int i = 0; i < PAGE; i++)
        if (shared[i] != PARENTS)
            kept = 0;
    print(kept ? "[fork64] parent: my page is as I left it - the kernel "
                 "wrote into the child's copy\n"
               : "[fork64] parent: ERROR, the kernel's write in the child "
                 "reached my page\n");
    bad |= !kept;

    sys_exit(bad);
}
