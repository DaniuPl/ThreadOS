/* forktest - forking a process with copy on write
 *
 * The program takes 256 KiB of heap, fills it with a pattern and
 * forks. The child returns from sys_fork() with zero, the parent with
 * the child's pid - both at the same point in the code, with the same
 * memory.
 *
 * The memory is not copied, though: both address spaces point at the
 * same frames, only without write permission. That can be seen in the
 * RAM usage right after the fork - it grows by a few KiB (page tables),
 * not by 256. Only when the child overwrites the pattern does the
 * kernel make it private copies of the pages; the parent sees its
 * pattern untouched.
 */

#include "usys.h"

#define BUF_KIB 256
#define BUF     (BUF_KIB * 1024)
#define STEP    4096              /* one byte per page is enough */

static unsigned char pattern(uint32_t i) { return (unsigned char)(0xA5 ^ (i >> 12)); }

static int check(unsigned char *buf, int mine)
{
    for (uint32_t i = 0; i < BUF; i += STEP) {
        unsigned char want = mine ? pattern(i) : (unsigned char)0x5A;
        if (buf[i] != want)
            return 0;
    }
    return 1;
}

void _start(void)
{
    unsigned char *buf = sys_sbrk(BUF);
    if (buf == (void *)-1) {
        print("[forktest] no memory for the buffer\n");
        sys_exit(1);
    }
    for (uint32_t i = 0; i < BUF; i += STEP)
        buf[i] = pattern(i);

    struct sysinfo si;
    sys_sysinfo(&si);
    uint32_t before = si.used_kib;

    int32_t pid = sys_fork();
    if (pid < 0) {
        print("[forktest] the fork did not work\n");
        sys_exit(1);
    }

    if (pid == 0) {
        /* The child: let the parent measure memory before you start writing */
        sys_sleep(400);
        if (!check(buf, 1)) {
            print("[forktest] child: the parent's pattern did NOT arrive\n");
            sys_exit(1);
        }
        /* A KERNEL write to a page that is still waiting to be copied:
           the syscall fills a buffer in the middle of the heap. Without
           the WP bit in CR0 the kernel would write straight into the
           frame shared with the parent and quietly corrupt its
           memory. */
        char *slot = (char *)(buf + BUF / 2);
        sys_getcwd(slot);
        if (slot[0] != '/') {
            print("[forktest] child: the syscall did not write the buffer\n");
            sys_exit(1);
        }
        print("[forktest] child: the pattern was inherited, overwriting it\n");
        for (uint32_t i = 0; i < BUF; i += STEP)
            buf[i] = 0x5A;
        if (!check(buf, 0)) {
            print("[forktest] child: its own writes did not arrive\n");
            sys_exit(1);
        }
        sys_exit(7);
    }

    /* The parent */
    sys_sysinfo(&si);
    uint32_t grew = si.used_kib - before;
    print("[forktest] bufor ");
    print_dec(BUF_KIB);
    print(" KiB, RAM growth after the fork: ");
    print_dec(grew);
    print(" KiB\n");
    if (grew >= BUF_KIB) {
        print("[forktest] the memory was copied - that is not COW\n");
        sys_exit(1);
    }
    print("[forktest] copy on write works\n");

    int32_t code = sys_wait(pid);
    if (!check(buf, 1)) {
        print("[forktest] parent: the child overwrote MY pattern\n");
        sys_exit(1);
    }
    print("[forktest] parent: the pattern is untouched, child code=");
    print_dec((uint32_t)code);
    print("\n");

    /* The classic Unix pair: fork, and in the child jump into another
       program. The pid, the descriptors and the working directory stay -
       only the process image is exchanged, so the parent waits for the
       same pid. */
    int32_t pid2 = sys_fork();
    if (pid2 < 0) {
        print("[forktest] the second fork did not work\n");
        sys_exit(1);
    }
    if (pid2 == 0) {
        sys_exec("echo [forktest] after exec: another program, the same pid");
        print("[forktest] the exec did not work\n");    /* this should not be reached */
        sys_exit(1);
    }
    sys_wait(pid2);
    print("[forktest] the parent waited out pid ");
    print_dec((uint32_t)pid2);
    print(" po exec\n");
    sys_exit(0);
}
