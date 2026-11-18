/* The PC's network stack on the ARM64 kernel
 *
 * kernel/net.c, tcp.c and dhcp.c - ARP, IP, ICMP, UDP, TCP and the DHCP
 * client - are built here unchanged. They were written for a kernel
 * that runs them one at a time: a section that must not be cut into is
 * irq_save() to irq_restore(), and anything that waits does so in
 * task_sleep(). On the PC a big lock does the rest; here it is this
 * file's: every way into the stack - a program's system call, the
 * thread that hands it the card's frames, its own threads (DHCP, TCP's
 * resends) - takes net_lock first, and task_sleep lets go of it while
 * the thread sleeps. Interrupts are off while it is held, so the holder
 * is never preempted on its core, and a core that wants the lock waits
 * only for code that does not wait.
 *
 * And what else the stack asks of a kernel (kernel/include/task.h,
 * pit.h, random.h): threads, the hundredths of a second since boot,
 * whether a socket's program is still there, numbers to start a TCP
 * connection's sequence at.
 */

#include <stdint.h>
#include "arm64.h"
#include "netglue.h"
#include "proc.h"
#include "thread.h"
#include "../kernel/include/heap.h"
#include "../kernel/include/pit.h"
#include "../kernel/include/random.h"
#include "../kernel/include/spinlock.h"
#include "../kernel/include/task.h"

static struct spinlock net_lock = SPINLOCK_FREE;
static struct thread *volatile holder;
static irqflags_t held_flags;

void net_enter(void)
{
    irqflags_t f = spin_lock_irq(&net_lock);
    holder = thread_current();
    held_flags = f;
}

void net_leave(void)
{
    irqflags_t f = held_flags;
    holder = 0;
    spin_unlock_irq(&net_lock, f);
}

/* --- kernel/include/task.h ----------------------------------------------- */

struct start {
    void (*fn)(void *);
    void *arg;
};

/* The stack's own threads run inside it, as a program's call does */
static void inside(void *a)
{
    struct start s = *(struct start *)a;
    kfree(a);
    net_enter();
    s.fn(s.arg);
    net_leave();
}

struct task *task_create(const char *name, void (*entry)(void *), void *arg)
{
    struct start *s = kmalloc(sizeof(*s));
    if (!s)
        return 0;
    s->fn = entry;
    s->arg = arg;
    struct thread *t = thread_create(name, inside, s);
    if (!t)
        kfree(s);
    return (struct task *)t;
}

void task_sleep(uint32_t ms)
{
    if (holder == thread_current()) {
        net_leave();
        thread_sleep_ms(ms);
        net_enter();
    } else {
        thread_sleep_ms(ms);
    }
}

/* A program's wait in the stack ends when the program does */
int task_killed(void)
{
    return thread_current()->interrupted;
}

/* A socket belongs to a program (its number): one that has ended has
   its sockets taken back */
int task_alive(int id)
{
    return proc_alive(id);
}

/* --- kernel/include/pit.h ------------------------------------------------ */

uint32_t pit_ticks(void)
{
    return (uint32_t)timer_ticks();
}

/* --- kernel/include/random.h ---------------------------------------------
 *
 * Numbers for TCP's initial sequence: unguessable enough that a stray
 * segment of an old connection does not land in a new one. The counter
 * runs at tens of megahertz, its low bits are the jitter of whatever
 * the machine was doing, and SplitMix64 spreads them over every bit.
 * Not keys: the kernel has no source fit for those here yet, and
 * SYS64_GETRANDOM says so by answering -1.
 */

static uint64_t state;

void random_bytes(void *out, uint32_t n)
{
    uint8_t *p = out;
    for (uint32_t i = 0; i < n; i++) {
        if (i % 8 == 0) {
            state += counter_now() + 0x9E3779B97F4A7C15UL;
            uint64_t z = state;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9UL;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBUL;
            state ^= z ^ (z >> 31);
        }
        p[i] = (uint8_t)(state >> (8 * (i % 8)));
    }
}
