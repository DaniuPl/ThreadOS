/* spin64 - a program with no handler, in an endless loop
 *
 * It cannot be ended from the inside and that is not the point: the
 * point is that it can be ended from the outside. Without signals a
 * program like this would take the console away forever.
 *
 * Every half second it says so, with a number that grows: a test
 * watching it can then tell a program that still runs from one that
 * stopped, which a single line printed at the start never could.
 *
 * Usage: run spin64          - only that
 *        run spin64 <file>   - first writes its mark into the file
 *                              through a writable mapping, and never
 *                              flushes it: when the program is killed,
 *                              the kernel has to
 *        run spin64 edge     - waits for Ctrl+C with a handler of its
 *                              own, standing at the very bottom of the
 *                              stack it has touched: the frame the
 *                              kernel puts there for the handler lands
 *                              in a page that does not exist yet, and
 *                              the handler checks that it did
 */

#include "usys64.h"

#define MARK "killed-but-kept"

static int same(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static void say(const char *a, const char *b, const char *c)
{
    print(a);
    print(b);
    print(c);
}

/* The lowest page of the stack touched before the wait */
static uint64_t edge;

/* The handler says where its frame landed rather than only that it
 * ran: a frame on the ordinary stack, high above the edge, would run it
 * just the same and prove nothing about growing the stack. */
static void at_the_edge(int sig)
{
    volatile unsigned char mine;
    uint64_t at = (uint64_t)(uintptr_t)&mine;
    (void)sig;
    if (at < edge && at >= edge - 4096) {
        print("[spin64] the handler ran below the edge of the stack\n");
        sys_exit(0);
    }
    print("[spin64] the handler's frame is not in the page below the edge\n");
    sys_exit(2);
}

/* A page of the stack far below anything the program has used gets
 * touched - from ring 3, so it is the ordinary stack growth - and the
 * stack pointer goes 64 bytes above its bottom. The page under it is
 * not there. A signal frame is nearly two hundred bytes, so the kernel
 * has to put most of it into that missing page: grow the stack, as a
 * push from the program would, rather than call the stack full.
 *
 * The waiting is a loop of bare system calls, with no call and no push
 * of its own - C code would touch the missing page itself, and the
 * question would be gone before the signal came. The line a test waits
 * for is written from there too, once the stack pointer has moved: a
 * Ctrl+C that comes after it can only find the program at the edge. */
static void wait_at_the_edge(void)
{
    static const char waiting[] =
        "[spin64] waiting at the edge of the stack - stop me with Ctrl+C\n";
    volatile unsigned char here;
    uint64_t page = (uint64_t)(uintptr_t)&here & ~0xFFFULL;
    volatile unsigned char *low =
        (volatile unsigned char *)(uintptr_t)(page - 32 * 4096);
    low[0] = 1;
    edge = (uint64_t)(uintptr_t)low;

    sys_signal(SIGINT, at_the_edge);

    uint64_t sp = (uint64_t)(uintptr_t)low + 64;
    __asm__ volatile("movq %0, %%rsp\n\t"
                     "movl %1, %%eax\n\t"
                     "movl %2, %%edi\n\t"
                     "syscall\n"
                     "1:\n\t"
                     "movl %3, %%eax\n\t"
                     "movl $500, %%edi\n\t"
                     "syscall\n\t"
                     "jmp 1b"
                     : : "r"(sp), "i"(SYS64_WRITE), "i"(FD_STDOUT),
                         "i"(SYS64_SLEEP), "S"(waiting),
                         "d"(sizeof(waiting) - 1)
                     : "rax", "rdi", "rcx", "r11", "memory");
    __builtin_unreachable();
}

static void mark_the_file(const char *name)
{
    uint64_t size = 0;
    unsigned char *p = sys_mmap_file_w(name, &size);
    if (!p || size < sizeof(MARK) - 1) {
        say("[spin64] cannot map '", name, "' for writing\n");
        sys_exit(1);
    }
    for (uint64_t i = 0; i < sizeof(MARK) - 1; i++)
        p[i] = (unsigned char)MARK[i];
    say("[spin64] marked '", name, "' and will not flush it\n");
}

void _start(int argc, char **argv)
{
    if (argc > 1 && same(argv[1], "edge"))
        wait_at_the_edge();
    if (argc > 1)
        mark_the_file(argv[1]);

    print("[spin64] spinning with no end - stop me with Ctrl+C\n");
    for (uint64_t n = 1;; n++) {
        char num[24];
        utoa(n, num);
        say("[spin64] tick ", num, "\n");
        sys_sleep(500);
    }
}
