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
 * answer in x0. The same numbers mean the C library's layer for
 * ThreadOS can be the same on both machines. This is the first handful
 * of them; the rest come as programs need them.
 *
 * A program that faults ends there, and says so; the kernel and every
 * other program go on. A fault of the kernel's own still stops it
 * (exceptions.c).
 */

#include <stdint.h>
#include "arm64.h"
#include "proc.h"
#include "thread.h"
#include "uart.h"
#include "../kernel/include/string.h"
#include "../kernel/include/heap.h"
#include "../kernel/include/pmm.h"
#include "../kernel/include/initrd.h"
#include "../kernel/include/spinlock.h"
#include "../kernel64/syscall64.h"

#define PROC_MAX 32
#define BRK_MAX (64UL << 20)          /* the most a program's heap grows */
#define ARGS_MAX 16

void enter_el0(uint64_t entry, uint64_t sp, uint64_t x0, uint64_t x1)
    __attribute__((noreturn));

static struct uproc procs[PROC_MAX];
static struct spinlock lock = SPINLOCK_FREE;
static int next_pid = 1;

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
        ph->memsz > USER_TOP - USER_STACK - ph->vaddr)
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

/* The arguments as C's main has them, at the top of the stack: the
   strings, then the pointers to them, the stack pointer below on a
   16-byte line */
static int build_stack(struct uproc *p, const char *args)
{
    uint64_t top = USER_TOP;
    for (uint64_t va = top - USER_STACK; va < top; va += 4096) {
        uint32_t pa = pmm_alloc_frame();
        if (!pa)
            return -1;
        memset((void *)(uintptr_t)pa, 0, 4096);
        if (uspace_map(p->table, va, pa, UMAP_WRITE) < 0) {
            pmm_free_frame(pa);
            return -1;
        }
    }
    /* The top page, where the strings go, written through its frame:
       this table is not the one loaded */
    uint8_t *page = (uint8_t *)(uintptr_t)uspace_lookup(p->table,
                                                        top - 4096, 0);
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
        p->pid = next_pid++;
    }
    spin_unlock_irq(&lock, f);
    if (!p)
        return -4;
    for (int i = 0; name[i] && i < (int)sizeof(p->name) - 1; i++)
        p->name[i] = name[i];
    p->table = uspace_new();
    p->entry = eh->entry;
    int rc = p->table ? 0 : -3;
    const struct phdr *ph = (const struct phdr *)(file + eh->phoff);
    for (int i = 0; !rc && i < eh->phnum; i++)
        if (ph[i].type == PT_LOAD && load_segment(p, file, size, &ph[i]) < 0)
            rc = -2;
    if (!rc && build_stack(p, args) < 0)
        rc = -3;
    p->brk = p->brk_start;
    if (!rc) {
        p->thread = thread_create_program(p->name, program_main, p, p,
                                          (uint64_t)(uintptr_t)p->table);
        if (!p->thread)
            rc = -3;
    }
    if (rc) {
        if (p->table)
            uspace_free(p->table);
        p->pid = 0;
        return rc;
    }
    return p->pid;
}

int proc_wait(int pid)
{
    struct uproc *p = proc_of(pid);
    if (!p || pid <= 0)
        return -1;
    while (!p->done)
        thread_sleep_ms(5);
    /* Its thread is gone from every core before its table goes: a
       table still loaded somewhere is not one to give back */
    thread_join(p->thread);
    uspace_free(p->table);
    int code = p->code;
    p->pid = 0;
    return code;
}

/* --- from EL0 ----------------------------------------------------------- */

static void end_program(struct uproc *p, int code) __attribute__((noreturn));
static void end_program(struct uproc *p, int code)
{
    p->code = code;
    p->done = 1;
    thread_exit();
}

/* A range of the program's own memory, mapped - and writable, when the
   kernel is to write it */
static int user_range(struct uproc *p, uint64_t va, uint64_t len, int write)
{
    if (va < USER_BASE || len > USER_TOP - va)
        return 0;
    for (uint64_t a = va & ~0xFFFUL; a < va + len; a += 4096) {
        int fl;
        if (!uspace_lookup(p->table, a, &fl) ||
            (write && !(fl & UMAP_WRITE)))
            return 0;
    }
    return 1;
}

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

/* The heap: pages from the end of the program's file up, as it asks */
static int64_t sys_sbrk(struct uproc *p, int64_t delta)
{
    uint64_t old = p->brk;
    if (delta <= 0)
        return (int64_t)old;            /* shrinking is not given back yet */
    uint64_t want = old + (uint64_t)delta;
    if (want - p->brk_start > BRK_MAX)
        return -1;
    for (uint64_t va = (old + 0xFFF) & ~0xFFFUL; va < want; va += 4096) {
        uint32_t pa = pmm_alloc_frame();
        if (!pa)
            return -1;
        memset((void *)(uintptr_t)pa, 0, 4096);
        if (uspace_map(p->table, va, pa, UMAP_WRITE) < 0) {
            pmm_free_frame(pa);
            return -1;
        }
    }
    p->brk = want;
    return (int64_t)old;
}

void proc_syscall(struct trap_frame *f)
{
    struct uproc *p = thread_current()->proc;
    uint64_t *x = f->x;
    int64_t r;
    switch (x[8]) {
    case SYS64_WRITE:
        r = sys_write(p, x[0], x[1], x[2]);
        break;
    case SYS64_EXIT:
        end_program(p, (int)x[0]);
    case SYS64_SLEEP:
        thread_sleep_ms((unsigned)x[0]);
        r = 0;
        break;
    case SYS64_TICKS:
        r = (int64_t)timer_ticks();
        break;
    case SYS64_GETPID:
        r = p->pid;
        break;
    case SYS64_SBRK:
        r = sys_sbrk(p, (int64_t)x[0]);
        break;
    default:
        r = -1;                          /* not on this kernel yet */
        break;
    }
    x[0] = (uint64_t)r;
}

void proc_fault(struct trap_frame *f, const char *what, uint64_t addr,
                const char *why)
{
    struct uproc *p = thread_current()->proc;
    printk("[proc] %s (pid %d): %s 0x%lx (%s), at 0x%lx - ended\n",
           p->name, p->pid, what, addr, why, f->elr);
    end_program(p, PROC_KILLED);
}
