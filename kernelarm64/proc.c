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
 * for both machines: musl runs here unchanged.
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
 * Processes: SYS64_FORK copies the calling thread into a program of its
 * own whose pages are its parent's, read-only on both sides until one
 * writes (mmu.c, uspace_fork and uspace_cow) - a web engine starts its
 * helpers from a process of hundreds of megabytes, which then execs at
 * once. SYS64_EXEC puts another program from the initrd in the place of
 * the caller's, SYS64_WAIT and SYS64_TRYWAIT collect a child's code,
 * SYS64_KILL ends a program. A program whose parent ended is the
 * kernel's to collect (proc_wait_orphans).
 *
 * Descriptors are the PC's (struct fd_entry): the console is no
 * descriptor at all, files are the initrd's (to read), and pipes, Unix
 * socket pairs, shared memory and /dev/null are kernel/ipc.c's - the
 * PC's code, built here on what this file gives it (ipchost.h).
 *
 * A program that faults ends there, and says so; the kernel and every
 * other program go on. A fault of the kernel's own still stops it
 * (exceptions.c).
 */

#include <stdint.h>
#include "arm64.h"
#include "proc.h"
#include "ipchost.h"
#include "netglue.h"
#include "rtc.h"
#include "thread.h"
#include "uart.h"
#include "../kernel/include/ipc.h"
#include "../kernel/include/net.h"
#include "../kernel/include/tcp.h"
#include "../kernel/include/string.h"
#include "../kernel/include/heap.h"
#include "../kernel/include/pmm.h"
#include "../kernel/include/initrd.h"
#include "../kernel/include/spinlock.h"
#include "../kernel64/syscall64.h"

#define PROC_MAX 64
#define ARGS_MAX 64                  /* words of a program's command line */
#define NAME_MAX 63
#define ROFILES  64                  /* files of the initrd ever opened */
#define CHUNK    4096                /* the most one read or write moves */

/* What SYS64_FUTEX answers with: minus Linux's numbers, as on the PC */
#define EINTR     4
#define EAGAIN    11
#define EFAULT    14
#define EINVAL    22
#define ETIMEDOUT 110

void enter_el0(uint64_t entry, uint64_t sp, uint64_t x0, uint64_t x1)
    __attribute__((noreturn));
void enter_el0_stack(uint64_t entry, uint64_t sp, uint64_t x0, uint64_t x1,
                     uint64_t kstack) __attribute__((noreturn));
void fork_return(struct trap_frame *f) __attribute__((noreturn));
void fp_save(uint64_t *state);

/* The table of programs and the count numbers come from: under 'lock'.
   A program's own table, threads and descriptors: under its p->lock.
   Taken in that order, then kernel/ipc.c's lock, then the scheduler's. */
static struct uproc procs[PROC_MAX];
static struct spinlock lock = SPINLOCK_FREE;
static int next_id = 1;

static int new_id(void)
{
    irqflags_t f = spin_lock_irq(&lock);
    int id = next_id++;
    spin_unlock_irq(&lock, f);
    return id;
}

static struct uproc *self(void)
{
    return thread_current()->proc;
}

static struct uproc *proc_of(int pid)
{
    for (int i = 0; i < PROC_MAX; i++)
        if (procs[i].pid == pid)
            return &procs[i];
    return 0;
}

/* A free slot with a number (the first thread's, too) and an empty
   table of descriptors */
static struct uproc *new_proc(void)
{
    struct fd_entry *fds = kmalloc(FD_MAX * sizeof(struct fd_entry));
    if (!fds)
        return 0;
    memset(fds, 0, FD_MAX * sizeof(struct fd_entry));
    irqflags_t f = spin_lock_irq(&lock);
    struct uproc *p = proc_of(0);
    if (p) {
        memset(p, 0, sizeof(*p));
        p->pid = next_id++;
        p->fds = fds;
        p->lock = (struct spinlock)SPINLOCK_FREE;
        p->mmap_next = MMAP_BASE;
    }
    spin_unlock_irq(&lock, f);
    if (!p)
        kfree(fds);
    return p;
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

/* A program's tables and where it starts, before it is anybody's */
struct image {
    uint64_t *table;
    uint64_t entry, sp, argv, brk_start;
    int argc;
};

/* Pages for [va, va + len), filled from the file where it reaches and
   zero past it. Every offset is checked against the file first: the
   listing is the file's word. */
static int load_segment(struct image *im, const uint8_t *file, uint32_t size,
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
        if (uspace_map(im->table, va, pa, flags) < 0) {
            /* Two segments on one page: a file this loader does not
               take, rather than a page that is both written and run */
            pmm_free_frame(pa);
            return -1;
        }
    }
    if (end > im->brk_start)
        im->brk_start = end;
    return 0;
}

/* The arguments as C's main has them, on the stack's top page - the one
   page of the stack made before the program runs: the strings, then
   the pointers to them, the stack pointer below on a 16-byte line.
   Written through its frame: the table is not the one loaded. */
static int build_stack(struct image *im, int argc, char *const *args)
{
    uint64_t top = USER_TOP;
    uint32_t need = (uint32_t)(argc + 1) * 8 + 32;
    for (int i = 0; i < argc; i++)
        need += (uint32_t)strlen(args[i]) + 1;
    if (need > 4096)
        return -1;
    uint32_t pa = pmm_alloc_frame();
    if (!pa)
        return -1;
    uint8_t *page = (uint8_t *)(uintptr_t)pa;
    memset(page, 0, 4096);
    if (uspace_map(im->table, top - 4096, pa, UMAP_WRITE) < 0) {
        pmm_free_frame(pa);
        return -1;
    }
    uint32_t at = 4096;
    uint64_t ptrs[ARGS_MAX + 1];
    for (int i = argc - 1; i >= 0; i--) {
        uint32_t len = (uint32_t)strlen(args[i]) + 1;
        at -= len;
        memcpy(page + at, args[i], len);
        ptrs[i] = top - 4096 + at;
    }
    ptrs[argc] = 0;
    at &= ~7u;
    at -= (uint32_t)(argc + 1) * 8;
    memcpy(page + at, ptrs, (size_t)(argc + 1) * 8);
    im->argv = top - 4096 + at;
    im->argc = argc;
    im->sp = (top - 4096 + at) & ~15UL;
    return 0;
}

/* 0, or -1 no such file, -2 not an AArch64 program this kernel runs,
   -3 no memory (or arguments too long for the page they go on) */
static int load_image(const char *name, int argc, char *const *args,
                      struct image *im)
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
    memset(im, 0, sizeof(*im));
    im->table = uspace_new();
    if (!im->table)
        return -3;
    im->entry = eh->entry;
    int rc = 0;
    const struct phdr *ph = (const struct phdr *)(file + eh->phoff);
    for (int i = 0; !rc && i < eh->phnum; i++)
        if (ph[i].type == PT_LOAD && load_segment(im, file, size, &ph[i]) < 0)
            rc = -2;
    if (!rc && build_stack(im, argc, args) < 0)
        rc = -3;
    if (rc)
        uspace_free(im->table);
    return rc;
}

/* --- programs: their start and their end ----------------------------- */

int proc_alive(int pid)
{
    irqflags_t f = spin_lock_irq(&lock);
    struct uproc *p = pid > 0 ? proc_of(pid) : 0;
    int alive = p && !p->done;
    spin_unlock_irq(&lock, f);
    return alive;
}

static void end_thread(struct uproc *p, int code) __attribute__((noreturn));
static void close_fds(struct uproc *p, int only_cloexec);

static void program_main(void *arg)
{
    struct uproc *p = arg;
    enter_el0(p->entry, p->sp, (uint64_t)p->argc, p->argv);
}

static void set_name(struct uproc *p, const char *name)
{
    int i = 0;
    for (; name[i] && i < (int)sizeof(p->name) - 1; i++)
        p->name[i] = name[i];
    p->name[i] = 0;
}

/* A program that never ran: everything it was given goes back */
static void discard_proc(struct uproc *p)
{
    close_fds(p, 0);
    if (p->table)
        uspace_free(p->table);
    kfree(p->fds);
    irqflags_t f = spin_lock_irq(&lock);
    p->pid = 0;
    spin_unlock_irq(&lock, f);
}

int proc_spawn(const char *name, const char *line)
{
    char words[16][64];
    char *args[16];
    int n = 0;
    for (const char *s = line; *s && n < 16; ) {
        while (*s == ' ')
            s++;
        if (!*s)
            break;
        int k = 0;
        while (*s && *s != ' ' && k < 63)
            words[n][k++] = *s++;
        while (*s && *s != ' ')
            s++;
        words[n][k] = 0;
        args[n] = words[n];
        n++;
    }
    struct image im;
    int rc = load_image(name, n, args, &im);
    if (rc)
        return rc;
    struct uproc *p = new_proc();
    if (!p) {
        uspace_free(im.table);
        return -4;
    }
    set_name(p, name);
    p->table = im.table;
    p->entry = im.entry;
    p->sp = im.sp;
    p->argv = im.argv;
    p->argc = im.argc;
    p->brk_start = p->brk = im.brk_start;
    struct thread *t = thread_create_program(p->name, program_main, p, p,
                                             (uint64_t)(uintptr_t)p->table,
                                             0);
    if (!t) {
        discard_proc(p);
        return -3;
    }
    t->tid = p->pid;
    p->threads[0] = t;
    p->live = 1;
    thread_wake(t);
    return p->pid;
}

/* Everything an ended program had: its threads' kernel memory, its
   pages, its table of descriptors (already closed), its slot */
static int reap_proc(struct uproc *p)
{
    while (!p->done)
        thread_sleep_ms(5);
    /* Its threads are gone from every core before its table goes: a
       table still loaded somewhere is not one to give back */
    for (int i = 0; i < PROC_THREADS; i++)
        if (p->threads[i])
            thread_join(p->threads[i]);
    uspace_free(p->table);
    kfree(p->fds);
    int code = p->code;
    irqflags_t f = spin_lock_irq(&lock);
    p->pid = 0;
    spin_unlock_irq(&lock, f);
    return code;
}

int proc_wait(int pid)
{
    irqflags_t f = spin_lock_irq(&lock);
    struct uproc *p = pid > 0 ? proc_of(pid) : 0;
    if (p && (p->reaping || p->parent))
        p = 0;
    if (p)
        p->reaping = 1;
    spin_unlock_irq(&lock, f);
    return p ? reap_proc(p) : -1;
}

void proc_wait_orphans(void)
{
    for (;;) {
        irqflags_t f = spin_lock_irq(&lock);
        struct uproc *p = 0;
        for (int i = 0; i < PROC_MAX && !p; i++)
            if (procs[i].pid && !procs[i].parent && !procs[i].reaping)
                p = &procs[i];
        if (p)
            p->reaping = 1;
        spin_unlock_irq(&lock, f);
        if (!p)
            return;
        char name[16];
        memcpy(name, p->name, sizeof(name));
        int pid = p->pid;
        int code = reap_proc(p);
        printk("[proc] %s (pid %d), whose parent ended, ended with code %d\n",
               name, pid, code);
    }
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
    struct uproc *p = self();
    if (!p || addr < USER_BASE || addr >= USER_TOP)
        return 0;
    irqflags_t f = spin_lock_irq(&p->lock);
    int ok = page_in_locked(p, addr);
    spin_unlock_irq(&p->lock, f);
    return ok;
}

/* A write to a page shared since a fork */
int proc_cow(uint64_t addr)
{
    struct uproc *p = self();
    if (!p || addr < USER_BASE || addr >= USER_TOP)
        return 0;
    irqflags_t f = spin_lock_irq(&p->lock);
    int r = uspace_cow(p->table, addr & ~0xFFFUL);
    spin_unlock_irq(&p->lock, f);
    return r == 1;
}

/* A range of the program's own memory - and writable, when the kernel
   is to write it. A page it may have and has not touched yet is made
   here, as its own touch would have made it; one shared since a fork
   is copied, as its own write would have copied it. */
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
        if (write && !(fl & UMAP_WRITE)) {
            if (!(fl & UMAP_COW) || !proc_cow(a) ||
                !uspace_lookup(p->table, a, &fl) || !(fl & UMAP_WRITE))
                return 0;
        }
    }
    return 1;
}

int ipc_user_readable(uint64_t va, uint64_t len)
{
    return user_range(self(), va, len, 0);
}

int ipc_user_writable(uint64_t va, uint64_t len)
{
    return user_range(self(), va, len, 1);
}

/* A string of the program's, up to 'room' with its zero: its length,
   or -1 */
static int copy_string(struct uproc *p, uint64_t va, char *out, int room)
{
    for (int i = 0; i < room; i++) {
        if ((i == 0 || ((va + i) & 0xFFF) == 0) &&
            !user_range(p, va + i, 1, 0))
            return -1;
        out[i] = *(const char *)(uintptr_t)(va + i);
        if (!out[i])
            return i;
    }
    return -1;
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
static uint64_t sys_mmap(struct uproc *p, uint64_t size)
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
    return base;
}

uint64_t ipc_map_area(uint64_t len)
{
    return sys_mmap(self(), len);
}

/* A page of shared memory (kernel/ipc.c holds the frame for it) */
int ipc_map_page(uint64_t va, uint32_t frame)
{
    struct uproc *p = self();
    irqflags_t f = spin_lock_irq(&p->lock);
    int r = uspace_map(p->table, va, frame, UMAP_WRITE | UMAP_SHARED);
    spin_unlock_irq(&p->lock, f);
    return r;
}

uint64_t ipc_unmap_page(uint64_t va)
{
    struct uproc *p = self();
    irqflags_t f = spin_lock_irq(&p->lock);
    uint64_t pa = uspace_unmap(p->table, va);
    spin_unlock_irq(&p->lock, f);
    return pa;
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

/* --- descriptors -------------------------------------------------------- */

/* The initrd's files that were ever opened: a descriptor's slot is a
   number here */
static struct { const uint8_t *data; uint32_t size; } rofiles[ROFILES];
static int nrofiles;

static int rofile(const uint8_t *data, uint32_t size)
{
    irqflags_t f = spin_lock_irq(&lock);
    int slot = -1;
    for (int i = 0; i < nrofiles && slot < 0; i++)
        if (rofiles[i].data == data)
            slot = i;
    if (slot < 0 && nrofiles < ROFILES) {
        rofiles[nrofiles].data = data;
        rofiles[nrofiles].size = size;
        slot = nrofiles++;
    }
    spin_unlock_irq(&lock, f);
    return slot;
}

/* One more holder of what an entry stands for - a copy in another
   number (dup2), another program (fork) or a message (kernel/ipc.c) -
   and one fewer. A pipe's end is a socket end used one way. */
static void hold(const struct fd_entry *e)
{
    if (e->kind == FDK_POTOK_R || e->kind == FDK_POTOK_W ||
        e->kind == FDK_USOCK)
        usock64_ref(e->slot);
    else if (e->kind == FDK_MEMFD)
        memfd64_ref(e->slot);
}

static void let_go(const struct fd_entry *e)
{
    if (e->kind == FDK_POTOK_R || e->kind == FDK_POTOK_W ||
        e->kind == FDK_USOCK)
        usock64_unref(e->slot);
    else if (e->kind == FDK_MEMFD)
        memfd64_unref(e->slot);
}

static struct fd_entry *fd_of(struct uproc *p, int64_t fd)
{
    if (fd < 0 || fd >= FD_MAX || p->fds[fd].kind == FDK_FREE)
        return 0;
    return &p->fds[fd];
}

/* The first free number from the third: zero, one and two mean the
   console, and become something else only through dup2 */
int ipc_fd_new(int kind, int slot)
{
    struct uproc *p = self();
    irqflags_t f = spin_lock_irq(&p->lock);
    int fd = -1;
    for (int i = FD_FIRST; i < FD_MAX && fd < 0; i++)
        if (p->fds[i].kind == FDK_FREE) {
            p->fds[i].kind = kind;
            p->fds[i].slot = slot;
            p->fds[i].pos = 0;
            p->fds[i].cloexec = 0;
            fd = i;
        }
    spin_unlock_irq(&p->lock, f);
    return fd;
}

int ipc_fd_kind(int fd)
{
    struct uproc *p = self();
    return fd < 0 || fd >= FD_MAX ? FDK_FREE : p->fds[fd].kind;
}

int ipc_fd_slot(int fd)
{
    struct fd_entry *e = fd_of(self(), fd);
    return e ? e->slot : -1;
}

int ipc_fd_copy(int fd, struct fd_entry *out)
{
    struct uproc *p = self();
    irqflags_t f = spin_lock_irq(&p->lock);
    struct fd_entry *e = fd_of(p, fd);
    if (e) {
        *out = *e;
        hold(out);
    }
    spin_unlock_irq(&p->lock, f);
    return e ? 0 : -1;
}

int ipc_fd_install(const struct fd_entry *e)
{
    struct uproc *p = self();
    irqflags_t f = spin_lock_irq(&p->lock);
    int fd = -1;
    for (int i = FD_FIRST; i < FD_MAX && fd < 0; i++)
        if (p->fds[i].kind == FDK_FREE) {
            p->fds[i] = *e;
            p->fds[i].cloexec = 0;       /* not the sender's flag */
            fd = i;
        }
    spin_unlock_irq(&p->lock, f);
    if (fd < 0)
        let_go(e);
    return fd;
}

void ipc_fd_drop(struct fd_entry *e)
{
    let_go(e);
}

/* Letting go happens outside the program's lock: a socket's last end
   wakes whoever waits on its peer */
int ipc_fd_close(int fd)
{
    struct uproc *p = self();
    irqflags_t f = spin_lock_irq(&p->lock);
    struct fd_entry *e = fd_of(p, fd), was = { 0, 0, 0, 0 };
    if (e) {
        was = *e;
        e->kind = FDK_FREE;
        e->cloexec = 0;
    }
    spin_unlock_irq(&p->lock, f);
    if (!e)
        return -1;
    let_go(&was);
    return 0;
}

/* Copying a number - how input and output are redirected. The
   destination is closed first: it is usually zero or one. */
static int64_t fd_dup2(struct uproc *p, int64_t old, int64_t fresh)
{
    if (fresh < 0 || fresh >= FD_MAX)
        return -1;
    irqflags_t f = spin_lock_irq(&p->lock);
    struct fd_entry *e = fd_of(p, old), was = { 0, 0, 0, 0 };
    if (e && old != fresh) {
        was = p->fds[fresh];
        p->fds[fresh] = *e;
        p->fds[fresh].cloexec = 0;    /* a copy is open across exec */
        hold(&p->fds[fresh]);
    }
    spin_unlock_irq(&p->lock, f);
    if (!e)
        return -1;
    let_go(&was);
    return fresh;
}

static int64_t fd_cloexec(struct uproc *p, int64_t fd, int64_t value)
{
    irqflags_t f = spin_lock_irq(&p->lock);
    struct fd_entry *e = fd_of(p, fd);
    int64_t r = -1;
    if (e) {
        if (value >= 0)
            e->cloexec = value ? 1 : 0;
        r = e->cloexec;
    }
    spin_unlock_irq(&p->lock, f);
    return r;
}

/* All of them as a program ends, or those marked as it execs: a parent
   learns that exec worked by a marked pipe closing (posix_spawn) */
static void close_fds(struct uproc *p, int only_cloexec)
{
    for (int i = 0; i < FD_MAX; i++) {
        irqflags_t f = spin_lock_irq(&p->lock);
        struct fd_entry was = p->fds[i];
        int go = was.kind != FDK_FREE && (!only_cloexec || was.cloexec);
        if (go) {
            p->fds[i].kind = FDK_FREE;
            p->fds[i].cloexec = 0;
        }
        spin_unlock_irq(&p->lock, f);
        if (go)
            let_go(&was);
    }
}

static uint32_t fd_pos(struct uproc *p, int64_t fd)
{
    struct fd_entry *e = fd_of(p, fd);
    return e ? e->pos : 0;
}

static void fd_advance(struct uproc *p, int64_t fd, uint32_t n)
{
    irqflags_t f = spin_lock_irq(&p->lock);
    struct fd_entry *e = fd_of(p, fd);
    if (e)
        e->pos += n;
    spin_unlock_irq(&p->lock, f);
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

/* kernel/ipc.c's sleep: until woken or 'ticks' hundredths of a second */
void ipc_block(uint32_t ticks)
{
    uint64_t until = ticks ? counter_now() +
                             (uint64_t)ticks * timer_freq() / HZ : ~0UL;
    thread_wait(&thread_current()->woken, until);
}

/* --- threads ----------------------------------------------------------- */

/* Threads that have ended are given back here, under p->lock - the
   kernel's memory of each, not the program's */
static void reap_threads(struct uproc *p)
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
    if (last) {
        /* Its pipes' readers see the end of the data now, not when it
           is collected; its children are the kernel's from now on */
        close_fds(p, 0);
        f = spin_lock_irq(&lock);
        for (int i = 0; i < PROC_MAX; i++)
            if (procs[i].pid && procs[i].parent == p->pid)
                procs[i].parent = 0;
        spin_unlock_irq(&lock, f);
        p->done = 1;
    }
    thread_exit();
}

/* Every thread of the program ends - the caller's own, if it is one of
   them, on its way out (proc_return) */
static void stop_program(struct uproc *p, int code)
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
}

/* SYS64_EXIT_GROUP: every thread ends - this one now, the others
   wherever they are (proc_return) */
static void end_program(struct uproc *p, int code) __attribute__((noreturn));
static void end_program(struct uproc *p, int code)
{
    stop_program(p, code);
    end_thread(p, code);
}

void proc_return(void)
{
    struct uproc *p = self();
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
    reap_threads(p);
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

/* --- processes ---------------------------------------------------------- */

static void fork_main(void *arg)
{
    struct trap_frame fr __attribute__((aligned(16)));
    memcpy(&fr, arg, sizeof(fr));
    kfree(arg);
    proc_return();
    fork_return(&fr);
}

/* The calling thread, copied into a program of its own: its registers
   as they were at the call (the answer 0), its FP/SIMD registers and
   thread pointer, its parent's pages shared until written, its parent's
   descriptors each held once more */
static int64_t sys_fork(struct uproc *p, struct trap_frame *f)
{
    struct uproc *c = new_proc();
    if (!c)
        return -1;
    c->parent = p->pid;
    set_name(c, p->name);
    struct trap_frame *fr = kmalloc(sizeof(*fr));
    c->table = fr ? uspace_new() : 0;
    if (!c->table) {
        kfree(fr);
        discard_proc(c);
        return -1;
    }
    *fr = *f;
    fr->x[0] = 0;

    irqflags_t fl = spin_lock_irq(&p->lock);
    int rc = uspace_fork(p->table, c->table);
    c->brk_start = p->brk_start;
    c->brk = p->brk;
    c->mmap_next = p->mmap_next;
    for (int i = 0; i < FD_MAX; i++) {
        c->fds[i] = p->fds[i];
        hold(&c->fds[i]);
    }
    spin_unlock_irq(&p->lock, fl);

    struct thread *t = 0;
    if (!rc)
        t = thread_create_program(c->name, fork_main, fr, c,
                                  (uint64_t)(uintptr_t)c->table,
                                  read_sysreg(tpidr_el0));
    if (!t) {
        kfree(fr);
        discard_proc(c);
        return -1;
    }
    /* This core's FP/SIMD registers are the caller's still: the kernel
       uses none */
    fp_save(t->fpstate);
    t->tid = c->pid;
    c->threads[0] = t;
    c->live = 1;
    thread_wake(t);
    return c->pid;
}

/* Another program from the initrd in the place of the caller's - its
   descriptors (those not marked close-on-exec) and its number kept.
   Only a program of one thread: what a fork's child is, the one that
   execs. */
static int64_t sys_exec(struct uproc *p, uint64_t uname, uint64_t len,
                        uint64_t uargv)
{
    char name[16];
    if (!len || len > 15 || !user_range(p, uname, len, 0) || p->live > 1)
        return -1;
    memcpy(name, (const void *)(uintptr_t)uname, len);
    name[len] = 0;
    char *buf = kmalloc(4096);
    if (!buf)
        return -1;
    char *args[ARGS_MAX];
    int argc = 0, used = 0, ok = 1;
    for (; uargv && ok; argc++) {
        uint64_t at = uargv + (uint64_t)argc * 8;
        if (argc == ARGS_MAX || !user_range(p, at, 8, 0)) {
            ok = 0;
            break;
        }
        uint64_t s = *(const uint64_t *)(uintptr_t)at;
        if (!s)
            break;
        int n = copy_string(p, s, buf + used, 4096 - used);
        if (n < 0) {
            ok = 0;
            break;
        }
        args[argc] = buf + used;
        used += n + 1;
    }
    if (ok && !argc) {
        memcpy(buf, name, len + 1);
        args[argc++] = buf;
    }
    struct image im;
    int rc = ok ? load_image(name, argc, args, &im) : -1;
    kfree(buf);
    if (rc)
        return -1;

    /* From here the old program is gone */
    struct thread *me = thread_current();
    irqflags_t f = spin_lock_irq(&p->lock);
    uint64_t *old = p->table;
    p->table = im.table;
    p->brk_start = p->brk = im.brk_start;
    p->mmap_next = MMAP_BASE;
    set_name(p, name);
    me->ttbr0 = (uint64_t)(uintptr_t)im.table;
    me->tpidr_el0 = 0;
    write_sysreg(tpidr_el0, 0);
    write_sysreg(ttbr0_el1, me->ttbr0);
    __asm__ volatile("isb; tlbi vmalle1; dsb nsh; isb" ::: "memory");
    spin_unlock_irq(&p->lock, f);
    uspace_free(old);
    close_fds(p, 1);
    proc_return();
    enter_el0_stack(im.entry, im.sp, (uint64_t)im.argc, im.argv,
                    thread_stack_top(me));
}

/* A child's code into the program's int, where it asked */
static void put_code(struct uproc *p, uint64_t at, int code)
{
    if (at && user_range(p, at, sizeof(int), 1))
        *(int *)(uintptr_t)at = code;
}

/* Any child that has ended: its number, its code collected */
static int64_t sys_wait(struct uproc *p, uint64_t ucode)
{
    struct thread *me = thread_current();
    for (;;) {
        irqflags_t f = spin_lock_irq(&lock);
        struct uproc *c = 0;
        int children = 0;
        for (int i = 0; i < PROC_MAX && !c; i++)
            if (procs[i].pid && procs[i].parent == p->pid &&
                !procs[i].reaping) {
                children++;
                if (procs[i].done)
                    c = &procs[i];
            }
        if (c)
            c->reaping = 1;
        spin_unlock_irq(&lock, f);
        if (c) {
            int pid = c->pid;
            put_code(p, ucode, reap_proc(c));
            return pid;
        }
        if (!children || me->interrupted)
            return -1;
        thread_wait(0, counter_now() + timer_freq() / 200);
    }
}

/* -3 while a thread of this program or a child by that number runs; 0
   once the child has ended, its code collected; -4 for nothing there
   (or a thread that has ended) */
static int64_t sys_trywait(struct uproc *p, int64_t id, uint64_t ucode)
{
    int64_t r = -4;
    irqflags_t f = spin_lock_irq(&p->lock);
    for (int i = 0; i < PROC_THREADS; i++) {
        struct thread *t = p->threads[i];
        if (t && t->tid == id && !t->ended)
            r = -3;
    }
    spin_unlock_irq(&p->lock, f);
    if (r == -3)
        return r;
    f = spin_lock_irq(&lock);
    struct uproc *c = id > 0 ? proc_of((int)id) : 0;
    if (c && (c->parent != p->pid || c->reaping))
        c = 0;
    else if (c && !c->done)
        r = -3;
    else if (c)
        c->reaping = 1;
    spin_unlock_irq(&lock, f);
    if (c && r != -3) {
        put_code(p, ucode, reap_proc(c));
        r = 0;
    }
    return r;
}

/* No signal handlers here yet: any signal but 0 ends the program, as an
   unhandled one would, with the code that says "killed" */
static int64_t sys_kill(int64_t pid, int64_t sig)
{
    irqflags_t f = spin_lock_irq(&lock);
    struct uproc *t = pid > 0 ? proc_of((int)pid) : 0;
    if (t && t->done)
        t = 0;
    spin_unlock_irq(&lock, f);
    if (!t)
        return -1;
    if (sig)
        stop_program(t, -1);
    return 0;
}

/* --- files, pipes, the console ------------------------------------------ */

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
    /* The initrd has no directories: "/name" and "./name" are "name" */
    const char *n = name;
    while (*n == '/' || (n[0] == '.' && n[1] == '/'))
        n += *n == '/' ? 1 : 2;
    uint32_t size;
    const uint8_t *data = *n ? initrd_find(n, &size) : 0;
    int slot = data ? rofile(data, size) : -1;
    return slot < 0 ? -1 : ipc_fd_new(FDK_FILE, slot);
}

static int64_t sys_read(struct uproc *p, int64_t fd, uint64_t buf,
                        uint64_t len)
{
    if (len > CHUNK)
        len = CHUNK;
    int kind = ipc_fd_kind((int)fd);
    if (kind == FDK_FREE)
        return fd == 0 ? 0 : -1;      /* no keyboard yet: the end */
    if (kind == FDK_NULL)
        return 0;
    if (!user_range(p, buf, len, 1))
        return -1;
    int slot = ipc_fd_slot((int)fd);
    void *to = (void *)(uintptr_t)buf;
    if (kind == FDK_POTOK_R || kind == FDK_USOCK) {
        long n = usock64_read(slot, to, (uint32_t)len);
        return n < 0 ? -1 : n;
    }
    if (kind == FDK_MEMFD) {
        long n = memfd64_rw(slot, fd_pos(p, fd), to, (uint32_t)len, 0);
        if (n > 0)
            fd_advance(p, fd, (uint32_t)n);
        return n < 0 ? -1 : n;
    }
    if (kind != FDK_FILE)
        return -1;                     /* a pipe's write end */
    uint32_t pos = fd_pos(p, fd);
    uint32_t size = rofiles[slot].size;
    uint64_t n = pos < size ? size - pos : 0;
    if (n > len)
        n = len;
    memcpy(to, rofiles[slot].data + pos, n);
    fd_advance(p, fd, (uint32_t)n);
    return (int64_t)n;
}

static int64_t sys_write(struct uproc *p, int64_t fd, uint64_t buf,
                         uint64_t len)
{
    if (len > CHUNK)
        len = CHUNK;
    if (!user_range(p, buf, len, 0))
        return -1;
    const void *from = (const void *)(uintptr_t)buf;
    int kind = ipc_fd_kind((int)fd);
    if (kind == FDK_FREE) {
        if (fd != 1 && fd != 2)
            return -1;
        /* Through printk, a piece at a time, so a program's line and
           the kernel's do not cut into each other */
        char piece[257];
        for (uint64_t done = 0; done < len; ) {
            uint64_t n = len - done < 256 ? len - done : 256;
            memcpy(piece, (const char *)from + done, n);
            piece[n] = 0;
            printk("%s", piece);
            done += n;
        }
        return (int64_t)len;
    }
    int slot = ipc_fd_slot((int)fd);
    if (kind == FDK_POTOK_W || kind == FDK_USOCK) {
        long n = usock64_write(slot, from, (uint32_t)len);
        return n < 0 ? -1 : n;
    }
    if (kind == FDK_NULL)
        return (int64_t)len;           /* taken, and gone */
    if (kind == FDK_MEMFD) {
        long n = memfd64_rw(slot, fd_pos(p, fd), (void *)(uintptr_t)buf,
                            (uint32_t)len, 1);
        if (n > 0)
            fd_advance(p, fd, (uint32_t)n);
        return n < 0 ? -1 : n;
    }
    return -1;                         /* the initrd's files only read */
}

static int64_t sys_pipe(struct uproc *p, uint64_t out)
{
    if (!user_range(p, out, 2 * sizeof(int), 1))
        return -1;
    int r, w;
    if (usock64_pipe(&r, &w) < 0)
        return -1;
    int rfd = ipc_fd_new(FDK_POTOK_R, r);
    int wfd = rfd >= 0 ? ipc_fd_new(FDK_POTOK_W, w) : -1;
    if (wfd < 0) {
        if (rfd >= 0)
            ipc_fd_close(rfd);
        else
            usock64_unref(r);
        usock64_unref(w);
        return -1;
    }
    ((int *)(uintptr_t)out)[0] = rfd;
    ((int *)(uintptr_t)out)[1] = wfd;
    return 0;
}

/* What the C library asks of every descriptor (the PC's struct): its
   kind, a file's or memfd's size and position, a socket's bytes
   waiting and its type */
static int64_t sys_fdinfo(struct uproc *p, int64_t fd, uint64_t out)
{
    struct { int32_t kind; uint32_t size, pos; } info = { 0, 0, 0 };
    if (fd < 0 || fd >= FD_MAX || !user_range(p, out, sizeof(info), 1))
        return -1;
    info.kind = ipc_fd_kind((int)fd);
    int slot = ipc_fd_slot((int)fd);
    if (info.kind == FDK_FILE) {
        info.size = rofiles[slot].size;
        info.pos = fd_pos(p, fd);
    } else if (info.kind == FDK_MEMFD) {
        info.size = (uint32_t)memfd64_size(slot);
        info.pos = fd_pos(p, fd);
    } else if (info.kind == FDK_USOCK) {
        int type;
        usock64_info(slot, &info.size, &type);
        info.pos = (uint32_t)type;
    }
    memcpy((void *)(uintptr_t)out, &info, sizeof(info));
    return 0;
}

static int64_t sys_seek(struct uproc *p, int64_t fd, uint64_t pos)
{
    irqflags_t f = spin_lock_irq(&p->lock);
    struct fd_entry *e = fd_of(p, fd);
    int ok = e && (e->kind == FDK_FILE || e->kind == FDK_MEMFD);
    if (ok)
        e->pos = (uint32_t)pos;
    spin_unlock_irq(&p->lock, f);
    return ok ? 0 : -1;
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

/* --- the network -----------------------------------------------------------
 *
 * The PC's calls over the PC's stack (kernel/net.c, tcp.c), each inside
 * the stack's lock (netglue.c). A socket belongs to the program that
 * opened it - its number is the owner the stack checks every call
 * against, so a socket's number passed to another program is nothing
 * there.
 */

/* The peer of a UDP or TCP call, and its timeout, in one description */
struct udp_info {
    int sock;
    uint32_t ip;
    uint16_t port;
    uint16_t _padding;
    uint32_t timeout;
};

/* SYS64_TCP_ADDR's answer */
struct tcp_addr_info {
    uint32_t remote_ip;
    uint16_t remote_port;
    uint16_t local_port;
};

static int get_info(struct uproc *p, uint64_t at, struct udp_info *info)
{
    if (!user_range(p, at, sizeof(*info), 0))
        return -1;
    memcpy(info, (const void *)(uintptr_t)at, sizeof(*info));
    return 0;
}

static int64_t net_call(struct uproc *p, uint64_t n, const uint64_t *x)
{
    int owner = p->pid;
    struct udp_info info;
    int64_t r = -1;
    switch (n) {
    case SYS64_NETINFO: {
        /* Four addresses: own, mask, gateway, DNS */
        if (!user_range(p, x[0], 4 * sizeof(uint32_t), 1))
            return -1;
        uint32_t a[4];
        net_enter();
        a[0] = net_ip();
        a[1] = net_mask();
        a[2] = net_gw();
        a[3] = net_dns();
        r = net_configured();
        net_leave();
        memcpy((void *)(uintptr_t)x[0], a, sizeof(a));
        return r;
    }
    case SYS64_UDP_OPEN:
        net_enter();
        r = net_udp_open((uint16_t)x[0], owner);
        net_leave();
        return r;
    case SYS64_UDP_CLOSE:
        net_enter();
        net_udp_close((int)x[0], owner);
        net_leave();
        return 0;
    case SYS64_UDP_SEND:
        if (get_info(p, x[0], &info) < 0 || x[2] > 1400 ||
            !user_range(p, x[1], x[2], 0))
            return -1;
        net_enter();
        r = net_udp_send(info.sock, owner, info.ip, info.port,
                         (const void *)(uintptr_t)x[1], (uint16_t)x[2]);
        net_leave();
        return r;
    case SYS64_UDP_RECV: {
        uint64_t len = x[2] > 1400 ? 1400 : x[2];
        if (get_info(p, x[0], &info) < 0 || !user_range(p, x[0],
                                                       sizeof(info), 1) ||
            !user_range(p, x[1], len, 1))
            return -1;
        uint32_t from_ip = 0;
        uint16_t from_port = 0;
        net_enter();
        int got = net_udp_recv(info.sock, owner, &from_ip, &from_port,
                               (void *)(uintptr_t)x[1], (uint16_t)len,
                               info.timeout);
        net_leave();
        if (got > 0) {
            /* Who sent it, through the same description */
            info.ip = from_ip;
            info.port = from_port;
            memcpy((void *)(uintptr_t)x[0], &info, sizeof(info));
        }
        return got;
    }
    case SYS64_UDP_POLL:
        net_enter();
        r = net_udp_pending((int)x[0], owner);
        net_leave();
        return r;
    case SYS64_UDP_PORT:
        net_enter();
        r = net_udp_pending((int)x[0], owner) < 0 ? 0
                                                  : net_udp_port((int)x[0]);
        net_leave();
        return r;
    case SYS64_TCP_CONNECT:
        if (get_info(p, x[0], &info) < 0)
            return -1;
        net_enter();
        r = tcp_connect(info.ip, info.port, owner, info.timeout);
        net_leave();
        return r;
    case SYS64_TCP_START:
        if (get_info(p, x[0], &info) < 0)
            return -1;
        net_enter();
        r = tcp_connect_start(info.ip, info.port, owner);
        net_leave();
        return r;
    case SYS64_TCP_SEND:
        if (x[2] > 1400 || !user_range(p, x[1], x[2], 0))
            return -1;
        net_enter();
        r = tcp_send((int)x[0], owner, (const void *)(uintptr_t)x[1],
                     (uint16_t)x[2], 5000);
        net_leave();
        return r;
    case SYS64_TCP_RECV:
    case SYS64_TCP_RECVT: {
        /* A stream may answer with less than was asked: a big buffer is
           cut down, not refused (TLS asks for a whole record) */
        uint64_t cap = n == SYS64_TCP_RECV ? 4096 : 1400;
        uint64_t len = x[2] > cap ? cap : x[2];
        if ((n == SYS64_TCP_RECVT && x[2] > cap) ||
            !user_range(p, x[1], len, 1))
            return -1;
        net_enter();
        r = tcp_recv((int)x[0], owner, (void *)(uintptr_t)x[1],
                     (uint16_t)len,
                     n == SYS64_TCP_RECV ? 5000 : (uint32_t)x[3]);
        net_leave();
        return r;
    }
    case SYS64_TCP_CLOSE:
        net_enter();
        tcp_close((int)x[0], owner);
        net_leave();
        return 0;
    case SYS64_TCP_LISTEN:
        net_enter();
        r = tcp_listen((uint16_t)x[0], owner);
        net_leave();
        return r;
    case SYS64_TCP_ACCEPT:
        net_enter();
        r = tcp_accept((int)x[0], owner, (uint32_t)x[1]);
        net_leave();
        return r;
    case SYS64_TCP_POLL:
        net_enter();
        r = tcp_poll((int)x[0], owner);
        net_leave();
        return r;
    case SYS64_TCP_ADDR: {
        struct tcp_addr_info ai;
        if (!user_range(p, x[1], sizeof(ai), 1))
            return -1;
        net_enter();
        r = tcp_addresses((int)x[0], owner, &ai.remote_ip, &ai.remote_port,
                          &ai.local_port);
        net_leave();
        if (r < 0)
            return -1;
        memcpy((void *)(uintptr_t)x[1], &ai, sizeof(ai));
        return 0;
    }
    }
    return -1;
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
        r = sys_write(p, (int64_t)x[0], x[1], x[2]);
        break;
    case SYS64_READ:
        r = sys_read(p, (int64_t)x[0], x[1], x[2]);
        break;
    case SYS64_OPEN:
        r = sys_open(p, x[0], x[1], x[2]);
        break;
    case SYS64_CLOSE:
        r = ipc_fd_close((int)x[0]);
        break;
    case SYS64_SEEK:
        r = sys_seek(p, (int64_t)x[0], x[1]);
        break;
    case SYS64_FDINFO:
        r = sys_fdinfo(p, (int64_t)x[0], x[1]);
        break;
    case SYS64_FDINODE:
        /* A number that is the same for the same file */
        r = ipc_fd_kind((int)x[0]) == FDK_FILE ? ipc_fd_slot((int)x[0]) + 1
                                               : -1;
        break;
    case SYS64_FDCTL:
        r = fd_cloexec(p, (int64_t)x[0], (int64_t)x[1]);
        break;
    case SYS64_DUP2:
        r = fd_dup2(p, (int64_t)x[0], (int64_t)x[1]);
        break;
    case SYS64_PIPE:
        r = sys_pipe(p, x[0]);
        break;
    case SYS64_FTRUNCATE:
        r = ipc_fd_kind((int)x[0]) == FDK_MEMFD
                ? memfd64_resize(ipc_fd_slot((int)x[0]), x[1])
                : ipc_fd_kind((int)x[0]) == FDK_FILE ? -2 : -1;
        break;
    case SYS64_IPC:
        r = ipc64_call(x[0], x[1], x[2], x[3]);
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
    case SYS64_FORK:
        r = sys_fork(p, f);
        break;
    case SYS64_EXEC:
        r = sys_exec(p, x[0], x[1], x[2]);
        break;
    case SYS64_WAIT:
        r = sys_wait(p, x[0]);
        break;
    case SYS64_TRYWAIT:
        r = sys_trywait(p, (int64_t)x[0], x[1]);
        break;
    case SYS64_KILL:
        r = sys_kill((int64_t)x[0], (int64_t)x[1]);
        break;
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
        r = (int64_t)sys_mmap(p, x[0]);
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
    case SYS64_NETINFO:
    case SYS64_UDP_OPEN:
    case SYS64_UDP_CLOSE:
    case SYS64_UDP_SEND:
    case SYS64_UDP_RECV:
    case SYS64_UDP_POLL:
    case SYS64_UDP_PORT:
    case SYS64_TCP_CONNECT:
    case SYS64_TCP_START:
    case SYS64_TCP_SEND:
    case SYS64_TCP_RECV:
    case SYS64_TCP_RECVT:
    case SYS64_TCP_CLOSE:
    case SYS64_TCP_LISTEN:
    case SYS64_TCP_ACCEPT:
    case SYS64_TCP_POLL:
    case SYS64_TCP_ADDR:
        r = net_call(p, x[8], x);
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
    struct uproc *p = self();
    printk("[proc] %s (pid %d): %s 0x%lx (%s), at 0x%lx - ended\n",
           p->name, p->pid, what, addr, why, f->elr);
    /* A fault ends the whole program, every thread of it */
    end_program(p, PROC_KILLED);
}
