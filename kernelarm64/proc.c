/* Programs on the ARM64 kernel
 *
 * A program is an AArch64 ELF file in the initrd, loaded into an
 * address space of its own (mmu.c) and run at EL0 by a thread of the
 * kernel's that never comes back from entering it: from then on the
 * thread is the program, and comes into the kernel only through an
 * exception - a system call (SVC), an interrupt, or a fault.
 *
 * System calls take the PC's numbers (kernel64/syscall64.h) and Linux's
 * convention on AArch64: the number in x8, the arguments in x0-x5, the
 * answer in x0. The same numbers and the same answers mean the C
 * library's layer for ThreadOS (libc/overlay/src/threados) is one file
 * for both machines: musl runs here unchanged, on the calls it needs -
 * memory, threads and futexes, the clocks, the console, and the
 * initrd's files to read.
 *
 * Memory: the file is loaded at the start, the rest is made a page at a
 * time on the program's first touch (proc_page_in): the heap
 * (SYS64_SBRK), what SYS64_MMAP hands out, and the stack below its top
 * page. SYS64_MMAP only moves a mark, as on the PC; a C library asks
 * for far more than it touches.
 *
 * Threads: SYS64_CLONE makes another thread in the same tables, with
 * its own stack and thread pointer (TPIDR_EL0); SYS64_EXIT ends one,
 * zeroing and waking the word pthread_join waits on; SYS64_EXIT_GROUP
 * ends them all - wherever each is: waiting is cut short (thread.c,
 * thread_interrupt), a thread in the kernel ends on its way out, and a
 * thread computing at EL0 ends at the next tick of the timer
 * (proc_return).
 *
 * A program that faults ends there, and says so; the kernel and every
 * other program go on. A fault of the kernel's own still stops it
 * (exceptions.c).
 */

#include <stdint.h>
#include "arm64.h"
#include "proc.h"
#include "rtc.h"
#include "thread.h"
#include "uart.h"
#include "../kernel/include/string.h"
#include "../kernel/include/heap.h"
#include "../kernel/include/pmm.h"
#include "../kernel/include/initrd.h"
#include "../kernel/include/spinlock.h"
#include "../kernel64/syscall64.h"

#define PROC_MAX 32
#define ARGS_MAX 16
#define FD_MAX   256                 /* FD_MAX in kernel64/task64.c */
#define NAME_MAX 63

/* What SYS64_FUTEX answers with: minus Linux's numbers, as on the PC */
#define EINTR     4
#define EAGAIN    11
#define EFAULT    14
#define EINVAL    22
#define ETIMEDOUT 110

void enter_el0(uint64_t entry, uint64_t sp, uint64_t x0, uint64_t x1)
    __attribute__((noreturn));

static struct uproc procs[PROC_MAX];
static struct spinlock lock = SPINLOCK_FREE;
static int next_id = 1;

/* A program's number and a thread's come from one count, as on the PC:
   the first thread's number is the program's */
static int new_id(void)
{
    irqflags_t f = spin_lock_irq(&lock);
    int id = next_id++;
    spin_unlock_irq(&lock, f);
    return id;
}

/* --- the ELF file ---------------------------------------------------- */

struct ehdr {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};

struct phdr {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
};

#define PT_LOAD    1
#define PF_X       1
#define PF_W       2
#define EM_AARCH64 183
#define ET_EXEC    2

static struct uproc *proc_of(int pid)
{
    for (int i = 0; i < PROC_MAX; i++)
        if (procs[i].pid == pid)
            return &procs[i];
    return 0;
}

/* Pages for [va, va + len), filled from the file where it reaches and
   zero past it. Every offset is checked against the file first: the
   listing is the file's word. */
static int load_segment(struct uproc *p, const uint8_t *file, uint32_t size,
                        const struct phdr *ph)
{
    if (ph->memsz < ph->filesz || ph->offset > size ||
        ph->filesz > size - ph->offset || ph->vaddr < USER_BASE ||
        ph->vaddr >= MMAP_BASE - BRK_MAX ||
        ph->memsz > MMAP_BASE - BRK_MAX - ph->vaddr)
        return -1;
    int flags = (ph->flags & PF_W ? UMAP_WRITE : 0) |
                (ph->flags & PF_X ? UMAP_EXEC : 0);
    uint64_t start = ph->vaddr & ~0xFFFUL;
    uint64_t end = (ph->vaddr + ph->memsz + 0xFFF) & ~0xFFFUL;
    for (uint64_t va = start; va < end; va += 4096) {
        uint32_t pa = pmm_alloc_frame();
        if (!pa)
            return -1;
        uint8_t *page = (uint8_t *)(uintptr_t)pa;
        memset(page, 0, 4096);
        /* What of the file falls on this page */
        uint64_t lo = va > ph->vaddr ? va : ph->vaddr;
        uint64_t hi = va + 4096 < ph->vaddr + ph->filesz
                      ? va + 4096 : ph->vaddr + ph->filesz;
        if (hi > lo)
            memcpy(page + (lo - va), file + ph->offset + (lo - ph->vaddr),
                   hi - lo);
        if (flags & UMAP_EXEC)
            cache_sync_code(pa, 4096);
        if (uspace_map(p->table, va, pa, flags) < 0) {
            /* Two segments on one page: a file this loader does not
               take, rather than a page that is both written and run */
            pmm_free_frame(pa);
            return -1;
        }
        if (end > p->brk_start)
            p->brk_start = end;
    }
    return 0;
}

/* The arguments as C's main has them, on the stack's top page - the one
   page of the stack made before the program runs: the strings, then
   the pointers to them, the stack pointer below on a 16-byte line */
static int build_stack(struct uproc *p, const char *args)
{
    uint64_t top = USER_TOP;
    uint32_t pa = pmm_alloc_frame();
    if (!pa)
        return -1;
    uint8_t *page = (uint8_t *)(uintptr_t)pa;
    memset(page, 0, 4096);
    if (uspace_map(p->table, top - 4096, pa, UMAP_WRITE) < 0) {
        pmm_free_frame(pa);
        return -1;
    }
    /* Written through its frame: this table is not the one loaded */
    char words[ARGS_MAX][64];
    int n = 0;
    for (const char *s = args; *s && n < ARGS_MAX; ) {
        while (*s == ' ')
            s++;
        if (!*s)
            break;
        int k = 0;
        while (*s && *s != ' ' && k < 63)
            words[n][k++] = *s++;
        while (*s && *s != ' ')
            s++;
        words[n++][k] = 0;
    }
    uint32_t at = 4096;
    uint64_t ptrs[ARGS_MAX + 1];
    for (int i = n - 1; i >= 0; i--) {
        uint32_t len = (uint32_t)strlen(words[i]) + 1;
        at -= len;
        memcpy(page + at, words[i], len);
        ptrs[i] = top - 4096 + at;
    }
    ptrs[n] = 0;
    at &= ~7u;
    at -= (uint32_t)(n + 1) * 8;
    memcpy(page + at, ptrs, (size_t)(n + 1) * 8);
    p->argv = top - 4096 + at;
    p->argc = n;
    p->sp = (top - 4096 + at) & ~15UL;
    return 0;
}

static void end_thread(struct uproc *p, int code) __attribute__((noreturn));

static void program_main(void *arg)
{
    struct uproc *p = arg;
    enter_el0(p->entry, p->sp, (uint64_t)p->argc, p->argv);
}

int proc_spawn(const char *name, const char *args)
{
    uint32_t size;
    const uint8_t *file = initrd_find(name, &size);
    if (!file)
        return -1;
    const struct ehdr *eh = (const struct ehdr *)file;
    if (size < sizeof(*eh) || memcmp(eh->ident, "\177ELF", 4) ||
        eh->ident[4] != 2 || eh->ident[5] != 1 ||
        eh->machine != EM_AARCH64 || eh->type != ET_EXEC ||
        eh->phentsize != sizeof(struct phdr) || eh->phoff > size ||
        (uint64_t)eh->phnum * sizeof(struct phdr) > size - eh->phoff ||
        eh->entry < USER_BASE || eh->entry >= USER_TOP)
        return -2;

    irqflags_t f = spin_lock_irq(&lock);
    struct uproc *p = proc_of(0);
    if (p) {
        memset(p, 0, sizeof(*p));
        p->pid = next_id++;
    }
    spin_unlock_irq(&lock, f);
    if (!p)
        return -4;
    p->lock = (struct spinlock)SPINLOCK_FREE;
    for (int i = 0; name[i] && i < (int)sizeof(p->name) - 1; i++)
        p->name[i] = name[i];
    p->table = uspace_new();
    p->entry = eh->entry;
    p->mmap_next = MMAP_BASE;
    int rc = p->table ? 0 : -3;
    const struct phdr *ph = (const struct phdr *)(file + eh->phoff);
    for (int i = 0; !rc && i < eh->phnum; i++)
        if (ph[i].type == PT_LOAD && load_segment(p, file, size, &ph[i]) < 0)
            rc = -2;
    if (!rc && build_stack(p, args) < 0)
        rc = -3;
    p->brk = p->brk_start;
    struct thread *t = 0;
    if (!rc) {
        t = thread_create_program(p->name, program_main, p, p,
                                  (uint64_t)(uintptr_t)p->table, 0);
        if (!t)
            rc = -3;
    }
    if (rc) {
        if (p->table)
            uspace_free(p->table);
        p->pid = 0;
        return rc;
    }
    t->tid = p->pid;
    p->threads[0] = t;
    p->live = 1;
    thread_wake(t);
    return p->pid;
}

int proc_wait(int pid)
{
    struct uproc *p = proc_of(pid);
    if (!p || pid <= 0)
        return -1;
    while (!p->done)
        thread_sleep_ms(5);
    /* Its threads are gone from every core before its table goes: a
       table still loaded somewhere is not one to give back */
    for (int i = 0; i < PROC_THREADS; i++)
        if (p->threads[i])
            thread_join(p->threads[i]);
    uspace_free(p->table);
    int code = p->code;
    p->pid = 0;
    return code;
}

/* --- memory ----------------------------------------------------------- */

/* Where a page is the program's for the touching: its heap, what it
   was handed by SYS64_MMAP, its stack */
static int lazy(const struct uproc *p, uint64_t va)
{
    return (va >= p->brk_start && va < p->brk) ||
           (va >= MMAP_BASE && va < p->mmap_next) ||
           (va >= USER_TOP - USER_STACK && va < USER_TOP);
}

/* With p->lock held. Another thread may have made it a moment ago. */
static int page_in_locked(struct uproc *p, uint64_t va)
{
    va &= ~0xFFFUL;
    if (uspace_lookup(p->table, va, 0))
        return 1;
    if (!lazy(p, va))
        return 0;
    uint32_t pa = pmm_alloc_frame();
    if (!pa)
        return 0;
    memset((void *)(uintptr_t)pa, 0, 4096);
    if (uspace_map(p->table, va, pa, UMAP_WRITE) < 0) {
        pmm_free_frame(pa);
        return 0;
    }
    return 1;
}

int proc_page_in(uint64_t addr, int write)
{
    (void)write;                 /* every page made so is writable */
    struct uproc *p = thread_current()->proc;
    irqflags_t f = spin_lock_irq(&p->lock);
    int ok = page_in_locked(p, addr);
    spin_unlock_irq(&p->lock, f);
    return ok;
}

/* A range of the program's own memory - and writable, when the kernel
   is to write it. A page it may have and has not touched yet is made
   here, as its own touch would have made it. */
static int user_range(struct uproc *p, uint64_t va, uint64_t len, int write)
{
    if (va < USER_BASE || len > USER_TOP - va)
        return 0;
    for (uint64_t a = va & ~0xFFFUL; a < va + len; a += 4096) {
        int fl;
        if (!uspace_lookup(p->table, a, &fl)) {
            if (!proc_page_in(a, write) || !uspace_lookup(p->table, a, &fl))
                return 0;
        }
        if (write && !(fl & UMAP_WRITE))
            return 0;
    }
    return 1;
}

/* The heap: the mark moves, the pages come when touched */
static int64_t sys_sbrk(struct uproc *p, int64_t delta)
{
    irqflags_t f = spin_lock_irq(&p->lock);
    uint64_t old = p->brk;
    int64_t r = (int64_t)old;
    if (delta > 0) {
        if ((uint64_t)delta > BRK_MAX - (old - p->brk_start))
            r = -1;
        else
            p->brk = old + (uint64_t)delta;
    }                            /* shrinking is not given back yet */
    spin_unlock_irq(&p->lock, f);
    return r;
}

/* An address range of its own, as on the PC: 0 when there is none */
static int64_t sys_mmap(struct uproc *p, uint64_t size)
{
    if (!size || size > MMAP_TOP - MMAP_BASE)
        return 0;
    size = (size + 0xFFF) & ~0xFFFUL;
    irqflags_t f = spin_lock_irq(&p->lock);
    uint64_t base = p->mmap_next;
    if (size > MMAP_TOP - base)
        base = 0;
    else
        p->mmap_next = base + size;
    spin_unlock_irq(&p->lock, f);
    return (int64_t)base;
}

/* Pages the program gives back (madvise's MADV_DONTNEED): gone, and
   zeros when touched again. Only where pages are made by touching -
   the program's file is not given back so. */
static int64_t sys_discard(struct uproc *p, uint64_t va, uint64_t len)
{
    if ((va & 0xFFF) || len > USER_TOP || va + len < va)
        return -1;
    irqflags_t f = spin_lock_irq(&p->lock);
    int ok = 1;
    for (uint64_t a = va; a < va + len; a += 4096)
        if (!lazy(p, a))
            ok = 0;
    if (ok)
        for (uint64_t a = va; a < va + len; a += 4096) {
            uint64_t pa = uspace_unmap(p->table, a);
            if (pa)
                pmm_free_frame((uint32_t)pa);
        }
    spin_unlock_irq(&p->lock, f);
    return ok ? 0 : -1;
}

/* --- futexes -----------------------------------------------------------
 *
 * A waiting thread is on a list, in its own kernel stack's frame, with
 * the program's tables and the address as its key. Who wakes it takes
 * it off the list and sets 'woken' under the list's lock, then makes it
 * ready (thread_wake): set before the waiter has gone to sleep, the
 * flag keeps it from going (thread_wait looks at it under the
 * scheduler's lock).
 */

struct waiter {
    uint64_t *table;
    uint64_t addr;
    struct thread *t;
    volatile int woken;
    struct waiter *next;
};

static struct waiter *waiters;
static struct spinlock futex_lock = SPINLOCK_FREE;

static int64_t futex_wait(struct uproc *p, uint64_t addr, uint32_t val,
                          uint32_t ms)
{
    if ((addr & 3) || !user_range(p, addr, 4, 0))
        return -EFAULT;
    struct thread *me = thread_current();
    struct waiter w = { p->table, addr, me, 0, 0 };
    irqflags_t f = spin_lock_irq(&futex_lock);
    if (*(volatile uint32_t *)(uintptr_t)addr != val) {
        spin_unlock_irq(&futex_lock, f);
        return -EAGAIN;
    }
    w.next = waiters;
    waiters = &w;
    spin_unlock_irq(&futex_lock, f);

    uint64_t until = ms ? counter_now() + (uint64_t)ms * timer_freq() / 1000
                        : ~0UL;
    while (!w.woken && !me->interrupted && counter_now() < until)
        thread_wait(&w.woken, until);

    f = spin_lock_irq(&futex_lock);
    int woken = w.woken;
    if (!woken)
        for (struct waiter **pp = &waiters; *pp; pp = &(*pp)->next)
            if (*pp == &w) {
                *pp = w.next;
                break;
            }
    spin_unlock_irq(&futex_lock, f);
    return woken ? 0 : me->interrupted ? -EINTR : -ETIMEDOUT;
}

/* Wakes up to n of those waiting on addr; those after them, up to
   'move', wait on addr2 from now on. How many woke. */
static int futex_wake(struct uproc *p, uint64_t addr, int n, int move,
                      uint64_t addr2)
{
    int woke = 0, moved = 0;
    irqflags_t f = spin_lock_irq(&futex_lock);
    for (struct waiter **pp = &waiters; *pp; ) {
        struct waiter *w = *pp;
        if (w->table != p->table || w->addr != addr) {
            pp = &w->next;
            continue;
        }
        if (woke < n) {
            /* The waiter may be gone the moment it is woken - its
               frame with it - so nothing of it is read after */
            struct thread *t = w->t;
            *pp = w->next;
            w->woken = 1;
            thread_wake(t);
            woke++;
            continue;
        }
        if (moved < move) {
            w->addr = addr2;
            moved++;
        }
        pp = &w->next;
    }
    spin_unlock_irq(&futex_lock, f);
    return woke;
}

static int64_t sys_futex(struct uproc *p, uint64_t addr, uint64_t op,
                         uint64_t a3, uint64_t a4)
{
    if (op == FUTEX64_OP_WAIT)
        return futex_wait(p, addr, (uint32_t)a3, (uint32_t)a4);
    if (addr & 3)
        return -EINVAL;
    if (op == FUTEX64_OP_WAKE)
        return futex_wake(p, addr, a3 > 0x7FFFFFFF ? 0x7FFFFFFF : (int)a3,
                          0, 0);
    if (op == FUTEX64_OP_REQUEUE) {
        if (a4 & 3)
            return -EINVAL;
        return futex_wake(p, addr, (int)(a3 & 0x7FFFFFFF),
                          (int)((a3 >> 32) & 0x7FFFFFFF), a4);
    }
    return -EINVAL;
}

/* --- threads ----------------------------------------------------------- */

/* Threads that have ended are given back here, under p->lock - the
   kernel's memory of each, not the program's */
static void reap(struct uproc *p)
{
    for (int i = 0; i < PROC_THREADS; i++) {
        struct thread *t = p->threads[i];
        if (t && t->ended && thread_try_join(t))
            p->threads[i] = 0;
    }
}

static void end_thread(struct uproc *p, int code)
{
    struct thread *me = thread_current();
    /* pthread_join waits on this word: zero, and woken */
    if (me->clear_tid && !p->exiting && user_range(p, me->clear_tid, 4, 1)) {
        *(volatile uint32_t *)(uintptr_t)me->clear_tid = 0;
        futex_wake(p, me->clear_tid, 1, 0, 0);
    }
    irqflags_t f = spin_lock_irq(&p->lock);
    me->ended = 1;
    int last = --p->live == 0;
    if (last && !p->exiting)
        p->code = code;
    spin_unlock_irq(&p->lock, f);
    if (last)
        p->done = 1;
    thread_exit();
}

/* SYS64_EXIT_GROUP: every thread ends - this one now, the others
   wherever they are (proc_return) */
static void end_program(struct uproc *p, int code) __attribute__((noreturn));
static void end_program(struct uproc *p, int code)
{
    struct thread *me = thread_current();
    irqflags_t f = spin_lock_irq(&p->lock);
    if (!p->exiting) {
        p->code = code;
        p->exiting = 1;
    }
    for (int i = 0; i < PROC_THREADS; i++) {
        struct thread *t = p->threads[i];
        if (t && t != me && !t->ended)
            thread_interrupt(t);
    }
    spin_unlock_irq(&p->lock, f);
    end_thread(p, code);
}

void proc_return(void)
{
    struct uproc *p = thread_current()->proc;
    if (p && p->exiting)
        end_thread(p, p->code);
}

struct start {
    uint64_t entry, sp;
};

static void clone_main(void *arg)
{
    struct start s = *(struct start *)arg;
    kfree(arg);
    proc_return();               /* its program ended before it began */
    enter_el0(s.entry, s.sp, 0, 0);
}

/* struct clone64, kernel64/proc64.h: the library has the stack and the
   thread block ready; the thread starts at entry with them */
struct clone_args {
    uint64_t entry, stack, tls, parent_tid, clear_tid;
};

static int64_t sys_clone(struct uproc *p, uint64_t a)
{
    struct clone_args c;
    if (!user_range(p, a, sizeof(c), 0))
        return -1;
    memcpy(&c, (const void *)(uintptr_t)a, sizeof(c));
    int fl;
    if (!uspace_lookup(p->table, c.entry, &fl) || !(fl & UMAP_EXEC) ||
        c.stack < USER_BASE || c.stack > USER_TOP || (c.stack & 15) ||
        (c.clear_tid & 3) || (c.parent_tid & 3) ||
        (c.parent_tid && !user_range(p, c.parent_tid, 4, 1)))
        return -1;
    struct start *s = kmalloc(sizeof(*s));
    if (!s)
        return -1;
    s->entry = c.entry;
    s->sp = c.stack;
    int tid = new_id();

    irqflags_t f = spin_lock_irq(&p->lock);
    reap(p);
    int slot = -1;
    for (int i = 0; i < PROC_THREADS && slot < 0; i++)
        if (!p->threads[i])
            slot = i;
    struct thread *t = 0;
    if (slot >= 0 && !p->exiting)
        t = thread_create_program(p->name, clone_main, s, p,
                                  (uint64_t)(uintptr_t)p->table, c.tls);
    if (t) {
        t->tid = tid;
        t->clear_tid = c.clear_tid;
        p->threads[slot] = t;
        p->live++;
    }
    spin_unlock_irq(&p->lock, f);
    if (!t) {
        kfree(s);
        return -1;
    }
    /* Its number is where the library looks for it before it runs, as
       Linux's CLONE_PARENT_SETTID: it is held until now */
    if (c.parent_tid)
        *(volatile int32_t *)(uintptr_t)c.parent_tid = tid;
    thread_wake(t);
    return tid;
}

/* -3 while a thread of this program by that number runs (the C
   library's question: may the stack of a detached thread that ended
   be used again?), -1 otherwise */
static int64_t sys_trywait(struct uproc *p, int64_t id)
{
    int64_t r = -1;
    irqflags_t f = spin_lock_irq(&p->lock);
    for (int i = 0; i < PROC_THREADS; i++) {
        struct thread *t = p->threads[i];
        if (t && t->tid == id && !t->ended)
            r = -3;
    }
    spin_unlock_irq(&p->lock, f);
    return r;
}

/* --- files ------------------------------------------------------------
 *
 * The initrd's, to read: there is no disk yet. A path is the file's
 * name, with a "/" or "./" before it taken off - the initrd has no
 * directories.
 */

static struct ufile *file_of(struct uproc *p, int64_t fd)
{
    if (fd < FD_FIRST || fd >= FD_FIRST + PROC_FILES)
        return 0;
    struct ufile *u = &p->files[fd - FD_FIRST];
    return u->data ? u : 0;
}

static int64_t sys_open(struct uproc *p, uint64_t path, uint64_t len,
                        uint64_t how)
{
    char name[NAME_MAX + 1];
    if (!len || len > NAME_MAX || !user_range(p, path, len, 0))
        return -1;
    memcpy(name, (const void *)(uintptr_t)path, len);
    name[len] = 0;
    if (how)
        return -2;               /* creating, truncating: not allowed */
    const char *n = name;
    while (*n == '/' || (n[0] == '.' && n[1] == '/'))
        n += *n == '/' ? 1 : 2;
    uint32_t size;
    const uint8_t *data = *n ? initrd_find(n, &size) : 0;
    if (!data)
        return -1;
    irqflags_t f = spin_lock_irq(&p->lock);
    int64_t fd = -1;
    for (int i = 0; i < PROC_FILES && fd < 0; i++)
        if (!p->files[i].data) {
            p->files[i].data = data;
            p->files[i].size = size;
            p->files[i].pos = 0;
            fd = FD_FIRST + i;
        }
    spin_unlock_irq(&p->lock, f);
    return fd;
}

static int64_t sys_read(struct uproc *p, int64_t fd, uint64_t buf,
                        uint64_t len)
{
    if (fd == 0)
        return 0;                /* no keyboard yet: the end of input */
    struct ufile *u = file_of(p, fd);
    if (!u)
        return -1;
    uint32_t pos = u->pos;
    uint64_t n = pos < u->size ? u->size - pos : 0;
    if (n > len)
        n = len;
    if (!user_range(p, buf, n, 1))
        return -1;
    memcpy((void *)(uintptr_t)buf, u->data + pos, n);
    u->pos = pos + (uint32_t)n;
    return (int64_t)n;
}

/* --- the console ------------------------------------------------------- */

static int64_t sys_write(struct uproc *p, uint64_t fd, uint64_t buf,
                         uint64_t len)
{
    if (fd != 1 && fd != 2)
        return -1;
    if (!user_range(p, buf, len, 0))
        return -1;
    /* Through printk, a piece at a time, so a program's line and the
       kernel's do not cut into each other */
    char piece[257];
    for (uint64_t done = 0; done < len; ) {
        uint64_t n = len - done < 256 ? len - done : 256;
        memcpy(piece, (const void *)(uintptr_t)(buf + done), n);
        piece[n] = 0;
        printk("%s", piece);
        done += n;
    }
    return (int64_t)len;
}

/* --- the clocks --------------------------------------------------------- */

/* The PC's struct rtc_time (kernel/include/rtc.h): what the C library
   builds the wall clock from */
struct tos_time {
    uint16_t year;
    uint8_t month, day, hour, min, sec;
} __attribute__((packed));

static int64_t sys_time(struct uproc *p, uint64_t at)
{
    if (!rtc_present() || !user_range(p, at, sizeof(struct tos_time), 1))
        return -1;
    struct rtc_time t;
    rtc_split(rtc_seconds(), &t);
    struct tos_time *o = (struct tos_time *)(uintptr_t)at;
    o->year = (uint16_t)t.year;
    o->month = (uint8_t)t.month;
    o->day = (uint8_t)t.day;
    o->hour = (uint8_t)t.hour;
    o->min = (uint8_t)t.min;
    o->sec = (uint8_t)t.sec;
    return 0;
}

static void sys_sleep(uint64_t ms)
{
    if (!ms) {
        thread_yield();          /* "give up my turn" */
        return;
    }
    struct thread *me = thread_current();
    uint64_t until = counter_now() + ms * timer_freq() / 1000;
    while (counter_now() < until && !me->interrupted)
        thread_wait(0, until);
}

/* --- the calls ---------------------------------------------------------- */

void proc_syscall(struct trap_frame *f)
{
    struct thread *me = thread_current();
    struct uproc *p = me->proc;
    uint64_t *x = f->x;
    int64_t r;
    switch (x[8]) {
    case SYS64_WRITE:
        r = sys_write(p, x[0], x[1], x[2]);
        break;
    case SYS64_READ:
        r = sys_read(p, (int64_t)x[0], x[1], x[2]);
        break;
    case SYS64_OPEN:
        r = sys_open(p, x[0], x[1], x[2]);
        break;
    case SYS64_CLOSE: {
        struct ufile *u = file_of(p, (int64_t)x[0]);
        if (u)
            u->data = 0;
        r = u ? 0 : -1;
        break;
    }
    case SYS64_SEEK: {
        struct ufile *u = file_of(p, (int64_t)x[0]);
        if (u)
            u->pos = (uint32_t)x[1];
        r = u ? (int64_t)u->pos : -1;
        break;
    }
    case SYS64_FDINFO: {
        /* What the C library asks of every descriptor: kind 1 a file,
           0 nothing (0, 1 and 2 are the console) */
        struct { int32_t kind; uint32_t size, pos; } info = { 0, 0, 0 };
        struct ufile *u = file_of(p, (int64_t)x[0]);
        if ((int64_t)x[0] < 0 || x[0] >= FD_MAX ||
            !user_range(p, x[1], sizeof(info), 1)) {
            r = -1;
            break;
        }
        if (u) {
            info.kind = 1;
            info.size = u->size;
            info.pos = u->pos;
        }
        memcpy((void *)(uintptr_t)x[1], &info, sizeof(info));
        r = 0;
        break;
    }
    case SYS64_FDINODE: {
        /* A number that is the same for the same file: where it is */
        struct ufile *u = file_of(p, (int64_t)x[0]);
        r = u ? (int64_t)(((uintptr_t)u->data >> 4) & 0x7FFFFFFF) : -1;
        break;
    }
    case SYS64_FDCTL:
        r = 0;                   /* no exec here for it to matter to */
        break;
    case SYS64_GETCWD:
        if (x[1] < 2 || !user_range(p, x[0], 2, 1)) {
            r = -1;
            break;
        }
        memcpy((void *)(uintptr_t)x[0], "/", 2);
        r = 1;
        break;
    case SYS64_CHDIR: {
        char d[3] = { 0, 0, 0 };
        if (x[1] && x[1] <= 2 && user_range(p, x[0], x[1], 0))
            memcpy(d, (const void *)(uintptr_t)x[0], x[1]);
        r = !strcmp(d, "/") || !strcmp(d, ".") ? 0 : -1;
        break;
    }
    case SYS64_EXIT:
        end_thread(p, (int)x[0]);
    case SYS64_EXIT_GROUP:
        end_program(p, (int)x[0]);
    case SYS64_SLEEP:
        sys_sleep(x[0]);
        r = 0;
        break;
    case SYS64_TICKS:
        r = (int64_t)timer_ticks();
        break;
    case SYS64_TIME:
        r = sys_time(p, x[0]);
        break;
    case SYS64_GETPID:
        r = me->tid;
        break;
    case SYS64_GETTGID:
        r = p->pid;
        break;
    case SYS64_NCORES:
        r = smp_online();
        break;
    case SYS64_CORE:
        r = (int64_t)cpu_this()->id;
        break;
    case SYS64_LEVEL:
        r = 1;                   /* a user, not the administrator */
        break;
    case SYS64_SIGNAL:
        r = 0;                   /* kept, though none is sent here yet */
        break;
    case SYS64_SBRK:
        r = sys_sbrk(p, (int64_t)x[0]);
        break;
    case SYS64_MMAP:
        r = sys_mmap(p, x[0]);
        break;
    case SYS64_DISCARD:
        r = sys_discard(p, x[0], x[1]);
        break;
    case SYS64_FUTEX:
        r = sys_futex(p, x[0], x[1], x[2], x[3]);
        break;
    case SYS64_CLONE:
        r = sys_clone(p, x[0]);
        break;
    case SYS64_TRYWAIT:
        r = sys_trywait(p, (int64_t)x[0]);
        break;
    default:
        r = -1;                          /* not on this kernel yet */
        break;
    }
    x[0] = (uint64_t)r;
    proc_return();
}

void proc_fault(struct trap_frame *f, const char *what, uint64_t addr,
                const char *why)
{
    struct uproc *p = thread_current()->proc;
    printk("[proc] %s (pid %d): %s 0x%lx (%s), at 0x%lx - ended\n",
           p->name, p->pid, what, addr, why, f->elr);
    /* A fault ends the whole program, every thread of it */
    end_program(p, PROC_KILLED);
}
