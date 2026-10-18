/* Kernel threads in long mode
 *
 * The list is circular, and the main thread - the one that came
 * straight from the boot - never sleeps and plays the part of the idle
 * thread. Thanks to that the scheduler always has somebody to pick and
 * need not know the case "nobody is ready".
 *
 * Finished threads stay on the list as zombies and disappear at the
 * next switch - we never clean up the one on whose stack we are
 * standing.
 */

#include <stddef.h>
#include <stdint.h>
#include "task64.h"
#include "paging64.h"
#include "syscall64.h"
#include "elf64.h"
#include "fpu64.h"
#include "../kernel/include/fs.h"
#include "../kernel/include/user.h"
#include "pipe64.h"
#include "proc64.h"
#include "../kernel/include/spinlock.h"
#include "../kernel/include/smp.h"
#include "../kernel/include/biglock.h"
#include "../kernel/include/heap.h"
#include "../kernel/include/pit.h"
#include "../kernel/include/printk.h"
#include "../kernel/include/string.h"
#include "../kernel/include/io.h"

#define STACK_SIZE 16384
#define NAME_MAX   16

/* Open file descriptors.
 *
 * The first three numbers are taken from the outset and refer to no
 * file: zero is the keyboard, one is the console, two is left for
 * errors. The rest are files on disk - each with its own position in
 * the file, because two processes may read the same file at different
 * paces. */
#define FD_MAX   8
#define FD_FIRST 3

/* The kinds live in the header - programs and syscalls know about them
   too. A free number 0, 1 or 2 means "the default input and output":
   the keyboard and the console. Only a redirection puts something else
   there. */
struct fd_entry {
    int kind;
    int slot;                    /* a TFS inode or a pipe number */
    uint32_t pos;                /* where this particular reader stands */
};

enum task_state { TASK_READY, TASK_RUNNING, TASK_SLEEPING, TASK_ZOMBIE };

struct task64 {
    uint64_t rsp;                /* the top of the stack while the thread is not running */
    void *stack;                 /* NULL for the main thread - its stack comes from the boot */
    void (*entry)(void *);
    void *arg;
    enum task_state state;
    uint32_t wake_at;
    uint64_t space;              /* its own address space (0 = the kernel's) */
    uint64_t kstack_top;         /* the kernel stack for entries from ring 3 */
    int id;
    int parent;                  /* who created it - for waiting on its end */
    int sig_pending;             /* a signal awaiting delivery */
    uint64_t sig_handler;        /* the program's handler (0 = kill) */
    uint64_t sig_restorer;       /* the return from the handler into the kernel */
    struct fd_entry fds[FD_MAX];
    void *fpu;                   /* the floating-point register state */
    uint64_t fsbase;             /* the FS segment base: a C library's thread pointer */
    uint64_t brk_base;           /* the start of the heap (past the program image) */
    uint64_t brk;                /* how far the program has asked for it */
    uint64_t mmap;               /* the next free address for mappings */
    struct file_map {
        uint64_t start, end;  /* the address range (0 = a free entry) */
        uint32_t size;        /* how many bytes the file itself has */
        uint32_t nblocks_of;
        uint32_t *blocks;         /* the TFS block numbers, in order */
        uint32_t write;          /* 1 = changes go back to the file */
    } maps[MMAP_FILES];
    int cwd;                     /* the working directory (an inode, FS_ROOT = /) */
    int vc;                      /* the virtual console it writes to */
    /* What this task may do: USER_ADMIN or USER_PLAIN. A child gets
       what its parent had - the level travels with the process. */
    int level;
    /* Where this task may be run.
       'anycore' = 1 means any core may take it. The default is zero
       and that is how it should be: a task reaching for the disk, the
       network or anything else written for a single core stays where
       it was. 'idle_for_core' >= 0 means this is THIS core's idle
       task - it stands on that core's stack and nobody else runs it. */
    int anycore;
    /* 'nolock' = 1 means the thread does not take the big kernel lock:
       whoever asked for it took on the promise that the thread touches
       only things that survive two cores at once. A ring 3 process
       does NOT have this - its kernel side is ordinary kernel code and
       is to be protected like any other. */
    int nolock;
    int idle_for_core;
    char name[NAME_MAX];
    void *raw;                   /* what kmalloc gave, before aligning */
    struct task64 *next;
};

extern void context_switch64(uint64_t *old_rsp, uint64_t new_rsp);

/* --- What belongs to a core ------------------------------------------
 *
 * Through this kernel's whole history the current task was ONE global
 * variable - and that is exactly why more than one core had no way to
 * run tasks. Now there are as many of them as there are cores, and
 * which one is mine the processor says itself (the number in its local
 * APIC).
 */
/* A task structure gets a cache line to itself: its first words - the
   stack pointer and the state - are written on every context switch,
   by whichever core is running it, and two structures sharing a line
   means two cores writing into one line. See kernel/task.c for what
   that cost and why it came and went with unrelated edits. */
#define MSR_FS_BASE 0xC0000100

static void write_fsbase(uint64_t v)
{
    __asm__ volatile("wrmsr" : : "c"(MSR_FS_BASE), "a"((uint32_t)v),
                     "d"((uint32_t)(v >> 32)));
}

static struct task64 *task_alloc(void)
{
    char *raw = kmalloc(sizeof(struct task64) + 64);
    if (!raw)
        return (struct task64 *)0;
    uintptr_t at = ((uintptr_t)raw + 63) & ~(uintptr_t)63;
    struct task64 *t = (struct task64 *)at;
    memset(t, 0, sizeof(*t));        /* a field nobody set is a zero, not junk */
    t->raw = raw;
    return t;
}

static void task_free(struct task64 *t)
{
    if (t)
        kfree(t->raw);
}

/* One cache line per core.
 *
 * These two pointers are written on every context switch, by the core
 * they belong to. Four cores' worth of them side by side is four cores
 * writing into ONE cache line - and then the line spends its time
 * travelling between them instead of the cores doing work. It cost
 * about ten times the running time of four kernel threads, and which
 * way it fell depended on where the line boundary happened to land,
 * so unrelated code growing elsewhere in the kernel would make the
 * machine slow or fast. Sixty-four bytes per core is the cheapest
 * thing this kernel buys. */
#define CACHELINE 64
struct core {
    struct task64 *running;
    struct task64 *idle;
    char pad[CACHELINE - 2 * sizeof(struct task64 *)];
} __attribute__((aligned(CACHELINE)));
static struct core cores[SMP_CORES];

#define current   (cores[smp_core()].running)
#define idle_task (cores[smp_core()].idle)

/* The task list is one for the whole machine, so the lock is one too.
   It is held ACROSS a context switch and released by whoever arrives
   on the other side - only then is the previous task's stack really
   free. */
/* And the lock on a line of its own, for the same reason. */
static struct spinlock lock __attribute__((aligned(CACHELINE)))
    = SPINLOCK_FREE;

static int next_id;
static int scheduler_up;

static void set_name(struct task64 *t, const char *name)
{
    int i = 0;
    for (; name[i] && i < NAME_MAX - 1; i++)
        t->name[i] = name[i];
    t->name[i] = '\0';
}

/* The first code of every new thread - context_switch64 "returns" here */
static void trampoline(void)
{
    /* The task list lock is still held by whoever switched us in here -
       and it is up to us to release it. A new task does not return
       from a context switch the way all the others do, so it has to do
       the same thing here. */
    spin_unlock(&lock);
    /* A new task starts in kernel code, so it takes the big lock.
     *
     * The only one that does not is a task any core may run - because
     * whoever asked for it has already taken on that its thread
     * touches only things that survive two cores at once. */
    if (!current->nolock)
        biglock_enter();
    __asm__ volatile("sti");
    current->entry(current->arg);
    task64_exit();
}

/* The idle thread: it does nothing and never sleeps.
 *
 * It exists so that the scheduler always has somebody to pick. Without
 * it the scheduler would have to handle the case "nobody is ready" -
 * and the only thing it could do then would be to wait for an
 * interrupt inside its own call. Had that call come from the timer
 * interrupt handler, the next tick would step into it, and so on
 * without end. */
static void idle_loop(void *arg)
{
    (void)arg;
    for (;;)
        __asm__ volatile("hlt");
}

void scheduler64_init(void)
{
    struct task64 *main_task = task_alloc();
    main_task->stack = NULL;
    main_task->state = TASK_RUNNING;
    main_task->id = next_id++;
    main_task->wake_at = 0;
    main_task->space = 0;
    main_task->kstack_top = 0;
    main_task->parent = -1;
    main_task->sig_pending = 0;
    main_task->sig_handler = 0;
    main_task->sig_restorer = 0;
    for (int i = 0; i < FD_MAX; i++)
        main_task->fds[i].kind = FDK_FREE;
    main_task->brk_base = main_task->brk = 0;
    main_task->mmap = USER64_MMAP_BASE;
    for (int i = 0; i < MMAP_FILES; i++)
        main_task->maps[i].start = main_task->maps[i].end = 0;
    main_task->fpu = fpu64_alloc();
    main_task->cwd = FS_ROOT;
    main_task->level = USER_ADMIN;
    main_task->vc = 0;
    set_name(main_task, "main");
    main_task->anycore = 0;
    main_task->nolock = 0;
    main_task->idle_for_core = -1;
    main_task->next = main_task;
    cores[0].running = main_task;

    cores[0].idle = task64_create("idle", idle_loop, 0);
    cores[0].idle->idle_for_core = 0;
    scheduler_up = 1;

    /* The main thread is finishing the boot, so from now on it is an
       ordinary task inside kernel code */
    biglock_enter();
}

int task_scheduler_ready(void)
{
    return scheduler_up;
}

struct task64 *task64_create(const char *name, void (*entry)(void *),
                             void *arg)
{
    struct task64 *t = task_alloc();
    if (!t)
        return NULL;
    t->stack = kmalloc(STACK_SIZE);
    if (!t->stack) {
        task_free(t);
        return NULL;
    }
    t->entry = entry;
    t->arg = arg;
    t->state = TASK_READY;
    t->wake_at = 0;
    t->space = 0;
    t->kstack_top = 0;
    t->parent = current ? current->id : -1;
    t->sig_pending = 0;
    t->sig_handler = 0;
    t->sig_restorer = 0;
    for (int i = 0; i < FD_MAX; i++)
        t->fds[i].kind = FDK_FREE;
    t->brk_base = t->brk = 0;
    t->mmap = USER64_MMAP_BASE;
    for (int i = 0; i < MMAP_FILES; i++)
        /* The whole entry, not only its ends: 'blocks' is a pointer
           that gets freed when the mapping goes, and a pointer left
           unset is whatever was in that memory before - a free of
           something that was never allocated. It waited a long time
           for a heap that handed out a block with rubbish in it. */
        {
            t->maps[i].start = t->maps[i].end = 0;
            t->maps[i].size = 0;
            t->maps[i].nblocks_of = 0;
            t->maps[i].blocks = (void *)0;
            t->maps[i].write = 0;
        }
    t->fpu = fpu64_alloc();
    t->cwd = current ? current->cwd : FS_ROOT;
    /* A thread standing in the kernel's own address space is the kernel
       shell as far as this is concerned - the shell is not a task of
       its own, so its level lives beside it. */
    t->level = (current && current->space &&
                current->space != paging64_kernel_space())
                   ? current->level : user_shell_level();
    t->vc = current ? current->vc : 0;
    t->id = next_id++;
    t->anycore = 0;            /* until somebody says otherwise */
    t->nolock = 0;
    t->idle_for_core = -1;
    set_name(t, name);

    /* We lay the stack out so that context_switch64 takes six zeros off
       it and "returns" into the trampoline. One spare word at the top
       maintains the stack alignment the calling convention requires. */
    uint64_t *sp = (uint64_t *)((uint8_t *)t->stack + STACK_SIZE);
    *--sp = 0;                            /* alignment to 16 bytes */
    *--sp = (uint64_t)(uintptr_t)trampoline;   /* the return address */
    for (int i = 0; i < 6; i++)
        *--sp = 0;                        /* rbp, rbx, r12-r15 */
    t->rsp = (uint64_t)(uintptr_t)sp;

    irqflags_t flags = spin_lock_irq(&lock);
    t->next = current->next;
    current->next = t;
    spin_unlock_irq(&lock, flags);
    return t;
}

/* A thread of the current process: the SAME address space, its own
   kernel stack, and everything else the process has - the working
   directory, the console, the descriptors it inherits from the parent.
   The space is freed by whichever of them ends last.

   The right to run on any core comes along: a thread is the same
   program, and since a changed mapping now reaches the other cores
   (kernel/tlb.c), two cores standing in one space is no longer a
   thing to be afraid of. */
struct task64 *task64_create_thread(const char *name, void (*entry)(void *),
                                    void *arg, uint64_t space)
{
    struct task64 *t = task64_create(name, entry, arg);
    if (!t)
        return NULL;
    irqflags_t flags = irq_save();
    t->space = space;
    t->brk_base = current->brk_base;
    t->brk = current->brk;
    t->mmap = current->mmap;
    t->cwd = current->cwd;
    t->level = current->level;
    t->vc = current->vc;
    t->anycore = current->anycore;
    for (int i = 0; i < FD_MAX; i++)
        t->fds[i] = current->fds[i];
    irq_restore(flags);
    return t;
}

/* A task that EVERY core may run. An ordinary one stays on the boot
   core: the rest of the kernel is written for a single core. */
struct task64 *task64_create_anycore(const char *name,
                                      void (*entry)(void *), void *arg)
{
    struct task64 *t = task64_create(name, entry, arg);
    if (t) {
        t->anycore = 1;
        t->nolock = 1;
    }
    return t;
}

/* Marks the CURRENT task as one that any core may run. A ring 3
   process uses this: its kernel side stays ordinary kernel code under
   the big lock, but the computing it does in ring 3 may go in parallel
   with others. */
void task_set_anycore(void)
{
    if (current)
        current->anycore = 1;
}

/* The trace of a finished task.
 *
 * The task itself leaves the list at the next switch, but its parent
 * may want to learn the exit code after the fact - and need not get
 * round to asking before the child finishes. So this one entry is left
 * behind, which the parent collects when it waits.
 *
 * When nobody comes for it, the oldest entry gives way to a newer one:
 * this is a trace, not a queue that may be filled up. */
#define EXIT_SLOTS 16

struct exit_rec {
    int used;
    int id;
    int parent;
    int code;
};

static struct exit_rec finished[EXIT_SLOTS];
static uint32_t exit_next;

static void record_exit(struct task64 *t, int code)
{
    for (int i = 0; i < EXIT_SLOTS; i++) {
        if (!finished[i].used) {
            finished[i].used = 1;
            finished[i].id = t->id;
            finished[i].parent = t->parent;
            finished[i].code = code;
            return;
        }
    }
    /* Everything taken - we overwrite in turn, so that one forgetful
       parent does not block the register forever */
    struct exit_rec *r = &finished[exit_next++ % EXIT_SLOTS];
    r->used = 1;
    r->id = t->id;
    r->parent = t->parent;
    r->code = code;
}

/* Does the task still have live children? Without this, waiting for a
   non-existent child would last forever. */
static int has_children(int id)
{
    struct task64 *t = current;
    do {
        if (t != current && t->parent == id && t->state != TASK_ZOMBIE)
            return 1;
        t = t->next;
    } while (t != current);
    return 0;
}

/* Waits for any child of its own to end. Returns its number and writes
   the exit code; -1 means "there is nothing to wait for" or "the wait
   was interrupted by a signal".
 *
 * The second of those matters here as much as the first. A signal is
 * delivered ONLY on the return to ring 3, and this loop never returns
 * there - so without this check a signal sent to a waiting process
 * would wait for delivery as long as the wait lasts. When the child
 * does not finish by itself, that means: forever. */
int task64_wait(int *code)
{
    for (;;) {
        irqflags_t flags = irq_save();
        for (int i = 0; i < EXIT_SLOTS; i++) {
            if (finished[i].used && finished[i].parent == current->id) {
                int id = finished[i].id;
                if (code)
                    *code = finished[i].code;
                finished[i].used = 0;
                irq_restore(flags);
                return id;
            }
        }
        int alive = has_children(current->id);
        int signalled = current->sig_pending != 0;
        irq_restore(flags);

        if (!alive || signalled)
            return -1;
        task64_sleep(10);
    }
}

/* Who really holds the console: the deepest live descendant of a task.
 *
 * A shell waits for its child, and that one may wait for its own -
 * Ctrl+C is to hit whatever at the end of that chain is actually
 * running, not the shell that is merely waiting. Otherwise
 * interrupting a program would kill the shell and leave the program
 * alive.
 *
 * The descent terminates on its own - a child gets a higher number
 * than its parent, so 'best' grows at every step - but the bound stays
 * anyway. A loop looking for the end of a loop is the last place where
 * one wants to rely on reasoning. */
int task64_foreground(int root)
{
    irqflags_t flags = irq_save();
    int best = root;

    for (int depth = 0; depth < 64; depth++) {
        struct task64 *t = current;
        int found = 0;
        do {
            if (t->parent == best && t->id > best &&
                t->state != TASK_ZOMBIE) {
                best = t->id;
                found = 1;
                break;
            }
            t = t->next;
        } while (t != current);
        if (!found)
            break;
    }
    irq_restore(flags);
    return best;
}

/* Reaps finished threads - never the one on whose stack we stand */
static void reap_zombies(void)
{
    struct task64 *t = current;
    while (t->next != current) {
        if (t->next->state == TASK_ZOMBIE) {
            struct task64 *z = t->next;
            t->next = z->next;
            fpu64_free(z->fpu);
            kfree(z->stack);
            task_free(z);
        } else {
            t = t->next;
        }
    }
}

/* Walks the list, waking those whose sleep is over, and returns the
   first ready one other than the current. We take the idle thread only
   when there is nobody else - otherwise it would eat half the CPU time
   doing nothing. */
/* May core 'nr' take this task?
 *
 * The boot core runs everything, as it always has. A secondary core -
 * only what somebody explicitly allowed, because the rest of the
 * kernel is written for a single core and quietly counts on it. An
 * idle task belongs to its own core and to nobody else. */
static int may_run(const struct task64 *t, int nr)
{
    if (t->idle_for_core >= 0)
        return t->idle_for_core == nr;
    return nr == 0 || t->anycore;
}

static struct task64 *pick_ready(int nr)
{
    uint32_t now = pit_ticks();
    struct task64 *ja = cores[nr].running;
    struct task64 *my_idle = cores[nr].idle;
    struct task64 *t = ja->next;
    struct task64 *found = NULL;

    while (t != ja) {
        if (t->state == TASK_SLEEPING && now >= t->wake_at)
            t->state = TASK_READY;
        if (t->state == TASK_READY && !found && t != my_idle &&
            may_run(t, nr))
            found = t;
        t = t->next;
    }
    if (!found && my_idle && my_idle != ja &&
        my_idle->state == TASK_READY)
        found = my_idle;
    return found;
}

/* Sets on the processor what belongs to the chosen task: its address
   space and the stack the kernel is to enter on an interrupt or a
   system call from ring 3.
 *
 * We reload CR3 only when it really changes - every reload throws away
 * the whole address translation buffer, so doing it while switching
 * between kernel threads would be a pure loss. */
static void apply_context(struct task64 *t, int nr)
{
    uint64_t want = t->space ? t->space : paging64_kernel_space();
    if (paging64_current_space() != want)
        paging64_switch(want);
    /* The kernel stack for entries from ring 3 - in THIS core's task
       state segment, because each has its own */
    (void)nr;
    if (t->kstack_top)
        syscall64_set_kernel_stack(t->kstack_top);
}

/* Gives up the task's address space and returns to the kernel's.
   Called when a task dies - and it dies while still standing on its
   own CR3, so it has to step off it first. */
/* Is anybody else still standing in this space?
 *
 * Threads share one, so the last one out turns the lights off. Freeing
 * it on the first thread's death would pull the tables out from under
 * the others - and they are still running in them. */
static int space_in_use(uint64_t space, struct task64 *except)
{
    struct task64 *t = current;
    do {
        if (t != except && t->space == space && t->state != TASK_ZOMBIE)
            return 1;
        t = t->next;
    } while (t != current);
    return 0;
}

static void drop_space(struct task64 *t)
{
    if (!t->space)
        return;
    uint64_t doomed = t->space;
    int last = !space_in_use(doomed, t);
    t->space = 0;
    t->kstack_top = 0;
    paging64_switch(paging64_kernel_space());
    if (last)
        paging64_free_space(doomed);
}

/* Takes ownership of an address space and moves onto it at once.
   Called by a thread that has just loaded a program. */
void task64_enter_space(uint64_t space, uint64_t kstack_top)
{
    irqflags_t flags = irq_save();
    current->space = space;
    current->kstack_top = kstack_top;
    apply_context(current, smp_core());
    irq_restore(flags);
}

/* Replaces the address space with a new one and hands back the old -
   for the caller to free, but only AFTER the processor has stepped off
   it. */
uint64_t task64_swap_space(uint64_t space, uint64_t kstack_top)
{
    irqflags_t flags = irq_save();
    uint64_t old = current->space;
    current->space = space;
    if (kstack_top)              /* zero = leave the kernel stack unchanged */
        current->kstack_top = kstack_top;
    apply_context(current, smp_core());
    irq_restore(flags);
    return old;
}

void task64_rename(const char *name)
{
    irqflags_t flags = irq_save();
    set_name(current, name);
    irq_restore(flags);
}

/* Picks the next thread and switches context. Call with IF=0.
 *
 * We take the task list lock without touching IF: if the timer stepped
 * into the middle of a section holding that lock, it would wait for it
 * forever, because only the code it just interrupted could release it.
 * We hold it ACROSS the switch, and it is released by whoever arrives
 * on the other side - only then is the previous task's stack really
 * free.
 */
static void switch_to(int new_state)
{
    int nr = smp_core();
    if (!cores[nr].running)
        return;

    /* We give the big kernel lock up FOR THE DURATION OF THE SWITCH -
       holding it across a context change would close the kernel for
       everybody until this task wakes again. The depth lives on THIS
       task's stack, so each one gets exactly its own back. */
    int depth = biglock_release_all();

    spin_lock(&lock);
    /* We change the current task's state already under the lock,
       together with picking the next one: setting it earlier would
       leave a window in which the task is marked as sleeping while its
       stack is still in use. */
    if (new_state >= 0)
        cores[nr].running->state = (enum task_state)new_state;
    reap_zombies();

    struct task64 *next = pick_ready(nr);

    /* There is nobody else - we stay, provided we can carry on
       ourselves. The idle thread makes the case "there is nobody and
       we cannot" simply not arise. */
    if (!next)
        next = cores[nr].running;

    if (next == cores[nr].running) {
        if (new_state < 0)
            next->state = TASK_RUNNING;
        spin_unlock(&lock);
        biglock_restore(depth);
        return;
    }

    struct task64 *prev = cores[nr].running;
    if (prev->state == TASK_RUNNING)
        prev->state = TASK_READY;
    next->state = TASK_RUNNING;
    cores[nr].running = next;

    /* The floating-point registers are as much part of a task's state
       as the rest - with the difference that the processor saves them
       with one instruction into a 512-byte area. Without this two
       programs computing at the same time would overwrite each other's
       results without knowing it. */
    fpu64_save(prev->fpu);
    fpu64_load(next->fpu);
    /* And the FS base, which a C library points at its thread's own
       block (errno and the rest live there). Only the program sets it,
       through SYS64_SET_FS, so the kernel's copy is always the current
       one and there is nothing to read back from the processor. */
    if (prev->fsbase != next->fsbase)
        write_fsbase(next->fsbase);

    apply_context(next, nr);
    context_switch64(&prev->rsp, next->rsp);
    spin_unlock(&lock);
    /* From this moment on it is the task that came back here that is
       running - and it is the one that takes its own depth off its own
       stack */
    biglock_restore(depth);
}

void schedule(void)
{
    switch_to(-1);
}

/* --- A secondary core joins the scheduler -----------------------------
 *
 * Every core needs a task to fall back on when there is nothing to
 * do - otherwise there would be nowhere to put its context. That task
 * is its property: it stands on the stack the core was given when it
 * was woken, and nobody else has the right to run it. This function
 * does not return.
 */
void task_core_enter(int nr)
{
    if (nr <= 0 || nr >= SMP_CORES || !scheduler_up)
        return;

    struct task64 *t = task_alloc();
    if (!t)
        return;
    t->stack = NULL;             /* its stack comes from the boot trampoline */
    t->state = TASK_RUNNING;
    t->id = -nr;                 /* a negative number: this is not a process */
    t->wake_at = 0;
    t->space = 0;
    t->kstack_top = 0;
    t->parent = -1;
    t->sig_pending = 0;
    t->sig_handler = 0;
    t->sig_restorer = 0;
    for (int i = 0; i < FD_MAX; i++)
        t->fds[i].kind = FDK_FREE;
    t->brk_base = t->brk = 0;
    t->mmap = USER64_MMAP_BASE;
    for (int i = 0; i < MMAP_FILES; i++)
        /* The whole entry, not only its ends: 'blocks' is a pointer
           that gets freed when the mapping goes, and a pointer left
           unset is whatever was in that memory before - a free of
           something that was never allocated. It waited a long time
           for a heap that handed out a block with rubbish in it. */
        {
            t->maps[i].start = t->maps[i].end = 0;
            t->maps[i].size = 0;
            t->maps[i].nblocks_of = 0;
            t->maps[i].blocks = (void *)0;
            t->maps[i].write = 0;
        }
    t->fpu = fpu64_alloc();
    t->cwd = FS_ROOT;
    t->level = USER_ADMIN;       /* a kernel thread is the kernel */
    t->vc = 0;
    t->anycore = 0;
    t->nolock = 1;         /* an idle task does not take the lock */
    t->idle_for_core = nr;
    set_name(t, "idle");
    /* the core's number goes right after the name, wherever it ends -
       a fixed index is a trap for whoever renames the task */
    int len = 0;
    while (t->name[len])
        len++;
    t->name[len] = (char)('0' + nr);
    t->name[len + 1] = '\0';

    irqflags_t flags = spin_lock_irq(&lock);
    t->next = cores[0].running->next;
    cores[0].running->next = t;
    cores[nr].running = t;
    cores[nr].idle = t;
    spin_unlock_irq(&lock, flags);

    __asm__ volatile("sti");
    for (;;) {
        smp_run_work(nr);
        task64_yield();
        /* Nothing for me - stand still until the next interrupt */
        __asm__ volatile("hlt");
    }
}

void task64_yield(void)
{
    irqflags_t flags = irq_save();
    schedule();
    irq_restore(flags);
}

void task64_sleep(uint32_t ms)
{
    irqflags_t flags = irq_save();
    current->wake_at = pit_ticks() + (ms + 9) / 10;   /* one tick = 10 ms */
    switch_to(TASK_SLEEPING);
    irq_restore(flags);
}

void task64_exit_code(int code)
{
    irq_save();
    task64_fd_close_all();
    /* Write back to disk what the process wrote into a writable
       mapping and did not manage to write back itself - this has to
       happen NOW, while its address space is still alive */
    proc64_msync();
    task64_mmap_release();
    drop_space(current);
    record_exit(current, code);
    switch_to(TASK_ZOMBIE);
    for (;;)                              /* a zombie will not be picked */
        __asm__ volatile("hlt");
}

void task64_exit(void)
{
    irq_save();
    task64_fd_close_all();
    /* Write back to disk what the process wrote into a writable
       mapping and did not manage to write back itself - this has to
       happen NOW, while its address space is still alive */
    proc64_msync();
    task64_mmap_release();
    drop_space(current);
    record_exit(current, 0);
    switch_to(TASK_ZOMBIE);
    for (;;)                              /* a zombie will not be picked */
        __asm__ volatile("hlt");
}

/* Kills the current task with a given reason. Does not return.
 *
 * Called from an exception handler when a ring 3 program was at fault.
 * The only difference from task64_exit is that here there is something
 * to say: the task did not finish by itself, it was stopped. */
void task64_kill_current(const char *why)
{
    irq_save();
    printk("[kernel] task %d (%s) killed: %s\n",
           current->id, current->name, why);
    task64_fd_close_all();
    drop_space(current);
    record_exit(current, -1);          /* killed, that is not of its own death */
    switch_to(TASK_ZOMBIE);
    for (;;)
        __asm__ volatile("hlt");
}

/* --- Signals -------------------------------------------------------
 *
 * A task carries one signal awaiting delivery. One is enough: a signal
 * is a notification, not a message - a second Ctrl+C before the first
 * is delivered carries no new information.
 *
 * Delivery cannot happen here: the task is somewhere else at this
 * moment, it may be asleep, it may be in the middle of a system call.
 * It will receive it only on the return to ring 3 - and that is the
 * only moment at which its frame can be replaced. */
int task64_signal(int id, int sig)
{
    irqflags_t flags = irq_save();
    struct task64 *t = current;
    int found = 0;
    do {
        if (t->id == id && t->state != TASK_ZOMBIE) {
            t->sig_pending = sig;
            if (t->state == TASK_SLEEPING)
                t->state = TASK_READY;      /* a signal interrupts the wait */
            found = 1;
            break;
        }
        t = t->next;
    } while (t != current);
    irq_restore(flags);
    return found;
}

int task64_signal_pending(void)
{
    return current ? current->sig_pending : 0;
}

int task64_take_signal(void)
{
    irqflags_t flags = irq_save();
    int sig = current->sig_pending;
    current->sig_pending = 0;
    irq_restore(flags);
    return sig;
}

void task64_set_sighandler(uint64_t handler, uint64_t restorer)
{
    irqflags_t flags = irq_save();
    current->sig_handler = handler;
    current->sig_restorer = restorer;
    irq_restore(flags);
}

uint64_t task64_sighandler(void)  { return current->sig_handler; }
uint64_t task64_sigrestorer(void) { return current->sig_restorer; }
int task64_current_id(void)       { return current ? current->id : -1; }
int task64_id_current(void)       { return current ? current->id : -1; }

/* Is the task with this number still alive. A zombie does not count -
   the network code asks this in order to clean up the sockets of the
   dead. */
/* A wait that does not wait: -3 while the process is still running,
   0 once it has ended (and then its exit code is taken out of the
   table, so that nobody reaps it twice), -4 when there is nothing
   there any more. */
int task64_trywait(int id, int *code)
{
    if (task64_alive(id))
        return -3;
    irqflags_t flags = irq_save();
    for (int i = 0; i < EXIT_SLOTS; i++) {
        if (finished[i].used && finished[i].id == id) {
            if (code)
                *code = finished[i].code;
            finished[i].used = 0;
            irq_restore(flags);
            return 0;
        }
    }
    irq_restore(flags);
    return -4;
}

int task64_alive(int id)
{
    irqflags_t flags = irq_save();
    struct task64 *t = current;
    int alive = 0;
    do {
        if (t->id == id && t->state != TASK_ZOMBIE) {
            alive = 1;
            break;
        }
        t = t->next;
    } while (t != current);
    irq_restore(flags);
    return alive;
}

/* The working directory is a property of a task, just like its
   descriptors: two processes may stand in different places of the tree
   and neither gets in the other's way. A child starts where its parent
   stood. */
int task64_cwd(void)          { return current ? current->cwd : FS_ROOT; }
void task64_set_cwd(int slot) { if (current) current->cwd = slot; }

/* The virtual console is a property of a task for the same reason as
   the working directory: a program started on the second console keeps
   writing to it even while we are looking at the first. Before the
   scheduler starts (boot messages) there is no task yet - then it is
   the first console. */
/* --- Swapping: who is to give something back ------------------------
 *
 * The PMM asks the kernel about this when it runs out of frames. We go
 * round the tasks in a circle and ask each one to give pages to disk.
 * We start from the one after the last visited, so as not to strip the
 * same process over and over.
 *
 * Kernel threads are skipped: their space is the kernel's space, and
 * kernel memory does not go to disk - there would be nobody to bring
 * it back, because it is brought back by the page fault handler, which
 * itself sits inside the kernel.
 *
 * Everything runs with interrupts off, and that is not merely caution
 * about the task list: between clearing a page entry and writing the
 * page to disk nobody may get the CPU. */
int task64_reclaim_page(void)
{
    static struct task64 *cursor;
    if (!current)                    /* the scheduler has not started yet */
        return 0;

    irqflags_t flags = irq_save();
    struct task64 *start = (cursor && cursor->state != TASK_ZOMBIE)
                               ? cursor : current;
    struct task64 *t = start;
    int given_back = 0;

    do {
        t = t->next;
        /* In batches: sweeping the page tables alone costs more than
           writing one page, so it does not pay to walk them from
           scratch for every missing frame */
        if (t->space && t->state != TASK_ZOMBIE &&
            paging64_reclaim_pages(t->space, 32)) {
            cursor = t;
            given_back = 1;
            break;
        }
    } while (t != start);

    irq_restore(flags);
    return given_back;
}

int task64_level(void)       { return current ? current->level : USER_ADMIN; }
void task64_set_level(int l)  { if (current) current->level = l; }

/* The level of the process with that pid, so that a caller can ask
   whether what it is about to signal is its own. */
int task64_level_of(int pid)
{
    if (!current)
        return USER_ADMIN;
    irqflags_t flags = irq_save();
    struct task64 *t = current;
    int level = current->level;
    do {
        if (t->id == pid) {
            level = t->level;
            break;
        }
        t = t->next;
    } while (t != current);
    irq_restore(flags);
    return level;
}

int task64_vc(void)          { return current ? current->vc : 0; }
void task64_set_vc(int vc)   { if (current) current->vc = vc; }

/* --- The program heap ----------------------------------------------
 *
 * The kernel allocates not one page here. It only moves the boundary
 * and remembers how far the program has asked for it - the pages
 * appear on first touch, in the page fault handler.
 *
 * The difference is bigger than it looks: asking for a megabyte costs
 * zero, and a program that asked and did not touch takes nothing away
 * from anybody. */
void task64_set_brk_base(uint64_t base)
{
    current->brk_base = base;
    current->brk = base;
    task64_set_fsbase(0);            /* the new image sets up its own */
    /* A new program image is a new memory layout - the mappings from
       the previous one vanished with its address space */
    current->mmap = USER64_MMAP_BASE;
}

/* --- The mapping area ------------------------------------------------
 *
 * Successive mappings go one after another upwards, without freeing:
 * the process takes this whole area with it when it ends anyway. */
int task64_mmap_add(uint64_t start, uint64_t end, uint32_t size,
                      uint32_t *blocks, uint32_t nblocks_of, uint32_t write)
{
    if (!current)
        return -1;
    for (int i = 0; i < MMAP_FILES; i++) {
        if (current->maps[i].end)
            continue;
        current->maps[i].start = start;
        current->maps[i].size = size;
        current->maps[i].blocks = blocks;
        current->maps[i].nblocks_of = nblocks_of;
        current->maps[i].write = write;
        /* 'end' says the entry is in use, so it goes in LAST. The page
           fault handler reads this table without the big kernel lock,
           and it is to see either a free entry or a whole one - never
           a marker over fields that are not written yet. */
        __asm__ volatile("" ::: "memory");
        current->maps[i].end = end;
        return 0;
    }
    return -1;
}

/* Called from the page fault handler, so it has to be short and
   without any surprises: just a lookup in a table of four entries. */
int task64_mmap_find(uint64_t addr, uint32_t *first_block,
                       uint32_t *nbytes, uint32_t *write)
{
    if (!current)
        return 0;
    uint64_t page = addr & ~0xFFFULL;

    for (int i = 0; i < MMAP_FILES; i++) {
        struct file_map *m = &current->maps[i];
        if (!m->end || page < m->start || page >= m->end)
            continue;

        uint64_t offset = page - m->start;
        uint32_t index = (uint32_t)(offset / FS_BLOCK);
        if (index >= m->nblocks_of)
            return 0;
        *first_block = index;
        /* How much of the file is left from the start of this page - the
           tail of the last page is to be zeroed, not filled with
           rubbish from disk */
        *nbytes = (offset < m->size)
                      ? (uint32_t)(m->size - offset) : 0;
        *write = m->write;
        return 1;
    }
    return 0;
}

/* The TFS block number at a given index of the mapping covering an address */
uint32_t task64_mmap_block(uint64_t addr, uint32_t index)
{
    uint64_t page = addr & ~0xFFFULL;
    for (int i = 0; i < MMAP_FILES; i++) {
        struct file_map *m = &current->maps[i];
        if (!m->end || page < m->start || page >= m->end)
            continue;
        return index < m->nblocks_of ? m->blocks[index] : 0;
    }
    return 0;
}

void task64_mmap_release(void)
{
    if (!current)
        return;
    for (int i = 0; i < MMAP_FILES; i++) {
        if (current->maps[i].blocks)
            kfree(current->maps[i].blocks);
        current->maps[i].blocks = (uint32_t *)0;
        current->maps[i].start = current->maps[i].end = 0;
    }
}

/* The child gets its own copy of the descriptions: each one frees its
   block number table itself, so they cannot be shared. When the copy
   fails, the child simply does not see that file - and that is better
   than two tasks freeing the same pointer. */
void task64_mmap_inherit(struct task64 *child)
{
    for (int i = 0; i < MMAP_FILES; i++) {
        child->maps[i] = current->maps[i];
        child->maps[i].blocks = (uint32_t *)0;
        if (!current->maps[i].end)
            continue;
        uint32_t n = current->maps[i].nblocks_of;
        uint32_t *copy = kmalloc(n * sizeof(uint32_t));
        if (!copy) {
            child->maps[i].start = child->maps[i].end = 0;
            continue;
        }
        for (uint32_t k = 0; k < n; k++)
            copy[k] = current->maps[i].blocks[k];
        child->maps[i].blocks = copy;
    }
}

/* The description of the current process's i-th mapping - for writing
   pages back to disk, which has to go through each of them in turn.
   Returns 0 when the entry is free. */
int task64_mmap_info(int i, uint64_t *start, uint64_t *end,
                     uint32_t *size, uint32_t *write)
{
    if (!current || i < 0 || i >= MMAP_FILES || !current->maps[i].end)
        return 0;
    *start = current->maps[i].start;
    *end = current->maps[i].end;
    *size = current->maps[i].size;
    *write = current->maps[i].write;
    return 1;
}

/* --- what belongs to the SPACE, not to the task ----------------------
 *
 * The heap limit and the next free mapping address describe an address
 * space, and threads share one. Kept per task they would be copied at
 * creation and moved separately afterwards - so two threads asking for
 * memory at the same moment would be handed the SAME address, each
 * convinced it was the first there. So it is written to every task
 * standing in the same space. */
static void set_in_space(int which_brk, uint64_t value)
{
    irqflags_t flags = irq_save();
    struct task64 *t = current;
    do {
        if (t->space == current->space) {
            if (which_brk)
                t->brk = value;
            else
                t->mmap = value;
        }
        t = t->next;
    } while (t != current);
    irq_restore(flags);
}

uint64_t task64_mmap_next(void)          { return current ? current->mmap : 0; }
void task64_set_mmap_next(uint64_t adr)  { if (current) set_in_space(0, adr); }

uint64_t task64_brk(void)      { return current->brk; }

void task64_set_fsbase(uint64_t v)
{
    current->fsbase = v;
    write_fsbase(v);
}

uint64_t task64_brk_base(void) { return current->brk_base; }

void task64_set_brk(uint64_t brk)
{
    set_in_space(1, brk);
}

/* Does the address lie in the part of the mapping area already handed
   out, but not covered by a mapped file? Then it is anonymous memory
   the process asked for and has now touched for the first time. */
int task64_in_mmap(uint64_t addr)
{
    return current && addr >= USER64_MMAP_BASE && addr < current->mmap;
}

/* Does the address belong to the current task's heap? The page fault
   handler asks this before calling it a program error. */
int task64_in_heap(uint64_t addr)
{
    return current && current->brk_base &&
           addr >= current->brk_base && addr < current->brk;
}

void task64_inherit_brk(struct task64 *child)
{
    child->brk_base = current->brk_base;
    child->brk = current->brk;
    child->mmap = current->mmap;     /* the child sees the same layout */
    child->fsbase = current->fsbase; /* and its memory, thread block included */
    /* The child inherits the right to run on any core: that is a
       property of the program, not of a single task */
    child->anycore = current->anycore;
}

/* --- Descriptors ---------------------------------------------------
 *
 * The table belongs to a task, so one process's descriptor means
 * nothing in another - and that is the whole difference between a
 * descriptor number and an inode, which is shared by the whole
 * system. */
int task64_fd_open(int slot)
{
    irqflags_t flags = irq_save();
    int fd = -1;
    for (int i = FD_FIRST; i < FD_MAX; i++) {
        if (current->fds[i].kind == FDK_FREE) {
            current->fds[i].kind = FDK_FILE;
            current->fds[i].slot = slot;
            current->fds[i].pos = 0;
            fd = i;
            break;
        }
    }
    irq_restore(flags);
    return fd;
}

/* A number for a pipe end. We look from the third, because zero and
   one have a conventional meaning and are only put there through
   dup2. */
int task64_fd_pipe(int idx, int reader_task)
{
    irqflags_t flags = irq_save();
    int fd = -1;
    for (int i = FD_FIRST; i < FD_MAX; i++) {
        if (current->fds[i].kind == FDK_FREE) {
            current->fds[i].kind = reader_task ? FDK_POTOK_R : FDK_POTOK_W;
            current->fds[i].slot = idx;
            current->fds[i].pos = 0;
            fd = i;
            break;
        }
    }
    irq_restore(flags);
    return fd;
}

int task64_fd_kind(int fd)
{
    if (fd < 0 || fd >= FD_MAX)
        return FDK_FREE;
    return current->fds[fd].kind;
}

int task64_fd_slot(int fd)
{
    if (fd < 0 || fd >= FD_MAX || current->fds[fd].kind == FDK_FREE)
        return -1;
    return current->fds[fd].slot;
}

uint32_t task64_fd_pos(int fd)
{
    if (fd < 0 || fd >= FD_MAX || current->fds[fd].kind != FDK_FILE)
        return 0;
    return current->fds[fd].pos;
}

void task64_fd_advance(int fd, uint32_t n)
{
    if (fd >= 0 && fd < FD_MAX && current->fds[fd].kind == FDK_FILE)
        current->fds[fd].pos += n;
}

/* Puts the reading point where the program says. Only a file has one -
   a pipe or the keyboard has nowhere to go back to. */
int task64_fd_seek(int fd, uint32_t pos)
{
    if (fd < 0 || fd >= FD_MAX || current->fds[fd].kind != FDK_FILE)
        return -1;
    current->fds[fd].pos = pos;
    return 0;
}

/* Closing a pipe end decreases the owner count - and it is exactly
   that which lets a reader ever see the end of the data */
static void release(struct task64 *t, int fd)
{
    if (t->fds[fd].kind == FDK_POTOK_R)
        pipe64_unref(t->fds[fd].slot, 1);
    else if (t->fds[fd].kind == FDK_POTOK_W)
        pipe64_unref(t->fds[fd].slot, 0);
    t->fds[fd].kind = FDK_FREE;
}

int task64_fd_close(int fd)
{
    if (fd < 0 || fd >= FD_MAX || current->fds[fd].kind == FDK_FREE)
        return -1;
    irqflags_t flags = irq_save();
    release(current, fd);
    irq_restore(flags);
    return 0;
}

/* Copying a number: this is how input and output are redirected. The
   destination is closed first, because it is usually zero or one. */
int task64_fd_dup2(int old, int fresh_one)
{
    if (old < 0 || old >= FD_MAX || fresh_one < 0 || fresh_one >= FD_MAX)
        return -1;
    if (current->fds[old].kind == FDK_FREE)
        return -1;
    if (old == fresh_one)
        return fresh_one;

    irqflags_t flags = irq_save();
    if (current->fds[fresh_one].kind != FDK_FREE)
        release(current, fresh_one);
    current->fds[fresh_one] = current->fds[old];
    if (current->fds[fresh_one].kind == FDK_POTOK_R)
        pipe64_ref(current->fds[fresh_one].slot, 1);
    else if (current->fds[fresh_one].kind == FDK_POTOK_W)
        pipe64_ref(current->fds[fresh_one].slot, 0);
    irq_restore(flags);
    return fresh_one;
}

/* Closes everything the task held - called when it ends. Without this
   a pipe would never see the end of its writers. */
void task64_fd_close_all(void)
{
    for (int i = 0; i < FD_MAX; i++)
        if (current->fds[i].kind != FDK_FREE)
            release(current, i);
}

/* A child inherits open files together with the position they stand
   at - just as it inherits memory. Pipes get one more owner in the
   process, because from now on two hold them. */
void task64_inherit_fds(struct task64 *child)
{
    irqflags_t flags = irq_save();
    for (int i = 0; i < FD_MAX; i++) {
        child->fds[i] = current->fds[i];
        if (child->fds[i].kind == FDK_POTOK_R)
            pipe64_ref(child->fds[i].slot, 1);
        else if (child->fds[i].kind == FDK_POTOK_W)
            pipe64_ref(child->fds[i].slot, 0);
    }
    irq_restore(flags);
}

int task64_id(const struct task64 *t)
{
    return t ? t->id : -1;
}

int task64_count(void)
{
    irqflags_t flags = irq_save();
    int n = 0;
    struct task64 *t = current;
    do {
        n++;
        t = t->next;
    } while (t != current);
    irq_restore(flags);
    return n;
}

/* One row of the task list for a program in ring 3 - the same thing
   the shell's 'ps' prints, only readable rather than printed. See
   task_ps() in the 32-bit kernel: the same rows, the same rule that
   the list a program shows is one refresh behind. */
int task64_ps(int index, struct psinfo64 *out)
{
    if (index < 0 || !out)
        return -1;

    irqflags_t flags = spin_lock_irq(&lock);
    struct task64 *t = current;
    int i = 0, found = 0;
    do {
        if (i == index) {
            out->pid = t->id;
            out->parent = t->parent;
            out->state = (int)t->state;
            out->anycore = t->anycore;
            /* Its own address space is what makes a process; a
               kernel thread runs in the kernel's. */
            out->ring3 = t->space ? 1 : 0;
            for (int k = 0; k < PS_NAME_MAX64 - 1; k++) {
                out->name[k] = t->name[k];
                if (!t->name[k])
                    break;
            }
            out->name[PS_NAME_MAX64 - 1] = '\0';
            found = 1;
            break;
        }
        i++;
        t = t->next;
    } while (t != current);
    spin_unlock_irq(&lock, flags);
    return found ? 0 : -1;
}

void task64_dump(void)
{
    static const char *state_names[] = { "ready", "running", "spi", "zombie" };
    irqflags_t flags = irq_save();
    printk("  ID  STATE   NAME\n");
    struct task64 *t = current;
    do {
        printk("  %d   %s  %s%s\n", t->id, state_names[t->state], t->name,
               t == current ? " (current)" : "");
        t = t->next;
    } while (t != current);
    irq_restore(flags);
}
