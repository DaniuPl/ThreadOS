/* Kernel threads and the scheduler (preemptive round-robin)
 *
 * Every thread has its own 8 KiB stack. A context switch (switch.S)
 * saves the callee-saved registers and ESP - the rest of the state is
 * held by the stack: a thread preempted by an interrupt has a full
 * interrupt frame there and resumes through iret, while a thread that
 * called task_sleep/yield returns from an ordinary call to schedule().
 *
 * The task list is circular; "main" (the former kmain) never sleeps
 * and plays the part of the idle thread (hlt), so the scheduler always
 * has somebody to pick. Finished threads (zombies) are reaped on later
 * calls to schedule() - we never reap the current one.
 */

#include <stddef.h>
#include "include/task.h"
#include "include/user.h"
#include "include/heap.h"
#include "include/fs.h"
#include "include/pit.h"
#include "include/printk.h"
#include "include/string.h"
#include "include/io.h"
#include "include/gdt.h"
#include "include/paging.h"
#include "include/tlb.h"
#include "include/kfile.h"
#include "include/pipe.h"
#include "include/signal.h"
#include "include/exec.h"
#include "include/spinlock.h"
#include "include/smp.h"
#include "include/biglock.h"

#define STACK_SIZE 8192
#define NAME_MAX   16

enum task_state { TASK_READY, TASK_RUNNING, TASK_SLEEPING, TASK_BLOCKED,
                  TASK_ZOMBIE };

struct task {
    uint32_t esp;
    uint32_t kstack_top;        /* for TSS.esp0 (interrupts from ring 3) */
    uint32_t cr3;               /* the task's address space */
    int owns_dir;               /* free the page directory on reap? */
    uint32_t brk;               /* end of the process heap (sbrk); 0 = kernel thread */
    uint32_t brk_base;          /* start of the heap - from here begins the area
                                   handed out only on first touch */
    uint32_t mmap_next;         /* next free mmap address (0 = kernel thread) */
    /* Memory-mapped files. Block numbers are fixed once, when the
       mapping is created, and read afterwards through the block layer,
       bypassing the PFS code - for the same reason as with swapping:
       PFS has one shared block buffer, and a page fault can land in
       the middle of any file operation. */
    struct file_map {
        uint32_t start, end;  /* address range (0 = free entry) */
        uint32_t size;        /* how many bytes the file itself has */
        uint32_t nblocks_of;
        uint32_t *blocks;
        uint32_t write;          /* 1 = changes go back to the file */
    } maps[MMAP_FILES];
    int cwd;                     /* working directory (PFS slot, -1 = root) */
    /* What this task is allowed to do: USER_ADMIN or USER_PLAIN. A
       child gets what its parent had - the level travels with the
       process, not with the program. */
    int level;
    int parent;                  /* parent pid (-1 = none, an orphan) */
    int vc;                      /* the console this task writes to */
    uint32_t sig_pending;        /* mask of pending signals */
    uint32_t sig_handler[NSIG];  /* SIG_DFL / SIG_IGN / handler address */
    uint32_t sig_tramp;          /* return trampoline (an address in ring 3) */
    uint32_t sig_frame;          /* frame of the interrupted state on the stack */
    int in_handler;              /* is a handler running right now */
    void (*entry)(void *);
    void *arg;
    enum task_state state;
    uint32_t wake_at;           /* the tick at which to wake up */
    int id;
    int priority;               /* 0=high, 1=normal, 2=low (idle) */
    /* Where this task may be run.
       'anycore' = 1 means any core may take it. The default is zero
       and that is how it should be: a task that reaches for the disk,
       the network or anything else written for a single core is to
       stay where it was. A secondary core takes only what somebody
       explicitly allowed.
       'idle_for_core' >= 0 means this is THIS core's idle task -
       nobody else runs it, because it stands on that core's stack. */
    int anycore;
    /* 'nolock' = 1 means the thread does not take the big kernel lock:
       whoever asked for it took on the promise that the thread touches
       only things that survive two cores at once. A ring 3 process
       does NOT have this - its kernel side is ordinary kernel code and
       is to be protected like any other. */
    int nolock;
    int idle_for_core;
    int exit_code;              /* the code returned by task_exit */
    int killed;                 /* a pending kill */
    char name[NAME_MAX];
    void *raw;                  /* what kmalloc gave, before aligning */
    void *stack;                /* NULL for the main thread (stack from boot.S) */
    struct file_desc fds[TASK_MAX_FD];
    struct task *next;
};

/* A task structure gets a cache line to itself.
 *
 * The first words of it - the stack pointer and the state - are
 * written on every context switch, by whichever core is running that
 * task. Two structures sharing a line means two cores writing into one
 * line, and then the line spends its time travelling between them: it
 * cost ten times the running time of four kernel threads. Worse, it
 * did or did not happen depending on where the heap's blocks fell,
 * which depends on the size of code that has nothing to do with any of
 * it - four lines added to the keyboard map were enough to flip it.
 *
 * The heap aligns to eight, so we ask for a line more and step forward
 * to the boundary ourselves, keeping the address to give back. */
#define CACHELINE 64

static struct task *task_alloc(void)
{
    char *raw = kmalloc(sizeof(struct task) + CACHELINE);
    if (!raw)
        return (struct task *)0;
    uintptr_t at = ((uintptr_t)raw + CACHELINE - 1) & ~(uintptr_t)(CACHELINE - 1);
    struct task *t = (struct task *)at;
    t->raw = raw;
    return t;
}

static void task_free(struct task *t)
{
    if (t)
        kfree(t->raw);
}

/* The boot stack from boot.S - the main thread carries on using it */
extern char stack_top[];

extern void context_switch(uint32_t *old_esp_store, uint32_t new_esp);

/* --- What belongs to a core ------------------------------------------
 *
 * Through this kernel's whole history the current task was ONE global
 * variable - and that is exactly why more than one core had no way to
 * run tasks. Now there are as many of them as there are cores, and
 * which one is mine the processor says itself (the number in its local
 * APIC).
 *
 * The 'current' macro stays, because all kernel code speaks that word
 * and there is no reason for it to stop. All that changed is what it
 * points at.
 */
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
    struct task *running;
    struct task *idle;      /* the one a core returns to when it has no work */
    char pad[CACHELINE - 2 * sizeof(struct task *)];
} __attribute__((aligned(CACHELINE)));
static struct core cores[SMP_CORES];

#define current (cores[smp_core()].running)

/* The task list is one for the whole machine, so the lock is one too.
 *
 * It is held ACROSS a context switch, and released by whoever arrives
 * on the other side. Otherwise there would be a window in which a task
 * is already marked ready while its stack is still in use - and a
 * second core would step onto a stack the first is still walking. */
/* And the lock on a line of its own, for the same reason: it is taken
   by every core, and a line it shares with anything else carries that
   traffic too. */
static struct spinlock lock __attribute__((aligned(CACHELINE)))
    = SPINLOCK_FREE;

static int next_id;
static int scheduler_up;

/* The table of exit codes of finished processes - wait reads them from
   here. Each entry also remembers the parent, because a task leaves
   the list sooner than the parent gets round to it (task_wait_any asks
   exactly here). A small ring buffer; on overflow it overwrites the
   oldest entry. */
#define EXIT_SLOTS 16
static struct { int pid; int parent; int code; int valid; } exit_codes[EXIT_SLOTS];
static int exit_next;

static void exit_record(int pid, int parent, int code)
{
    exit_codes[exit_next].pid = pid;
    exit_codes[exit_next].parent = parent;
    exit_codes[exit_next].code = code;
    exit_codes[exit_next].valid = 1;
    exit_next = (exit_next + 1) % EXIT_SLOTS;
}

/* Takes (and clears) the remembered exit code of a given pid; 0 if none */
static int exit_take(int pid)
{
    for (int i = 0; i < EXIT_SLOTS; i++) {
        if (exit_codes[i].valid && exit_codes[i].pid == pid) {
            exit_codes[i].valid = 0;
            return exit_codes[i].code;
        }
    }
    return 0;
}

static void task_set_name(struct task *t, const char *name)
{
    int i = 0;
    for (; name[i] && i < NAME_MAX - 1; i++)
        t->name[i] = name[i];
    t->name[i] = '\0';
}

/* A clean signal table: everything default, nothing pending */
static void task_init_signals(struct task *t)
{
    t->sig_pending = 0;
    t->sig_tramp = 0;
    t->sig_frame = 0;
    t->in_handler = 0;
    for (int i = 0; i < NSIG; i++)
        t->sig_handler[i] = SIG_DFL;
}

/* Standard streams: 0=keyboard (read), 1/2=console (write) */
static void task_init_fds(struct task *t)
{
    memset(t->fds, 0, sizeof(t->fds));         /* kind=FD_CONSOLE (=0) */
    t->fds[0].used = 1; t->fds[0].readable = 1;   /* stdin  */
    t->fds[1].used = 1; t->fds[1].writable = 1;   /* stdout */
    t->fds[2].used = 1; t->fds[2].writable = 1;   /* stderr */
}

struct file_desc *task_fds(void)
{
    return current->fds;
}

int task_cwd(void)
{
    return current->cwd;
}

/* What the running task may do, and the two ways it changes: up by a
   password (checked by the caller - only the caller knows what was
   typed), down for free. */
int task_level(void)
{
    return current ? current->level : USER_ADMIN;
}

void task_set_level(int level)
{
    if (current)
        current->level = level;
}

/* The level of the process with that pid - so that a caller can ask
   whether the process it is about to signal is its own. A pid nobody
   holds answers with the caller's own level: there is nothing there to
   protect, and 'no such process' is the answer the caller will get
   from the send itself. */
int task_level_of(int pid)
{
    irqflags_t flags = spin_lock_irq(&lock);
    struct task *t = current;
    int level = task_level();
    do {
        if (t->id == pid) {
            level = t->level;
            break;
        }
        t = t->next;
    } while (t != current);
    spin_unlock_irq(&lock, flags);
    return level;
}

void task_set_cwd(int slot)
{
    current->cwd = slot;
}

/* The first code of every new thread - context_switch "returns" here */
static void task_trampoline(void)
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
     * touches only things that survive two cores at once. If such a
     * task took the big lock too, four threads computing in parallel
     * would compute one after another - and that is not what they are
     * for. */
    if (!current->nolock)
        biglock_enter();
    __asm__ volatile("sti");
    current->entry(current->arg);
    task_exit(0);
}

void scheduler_init(void)
{
    struct task *main_task = task_alloc();
    main_task->id = next_id++;
    task_set_name(main_task, "main");
    main_task->state = TASK_RUNNING;
    main_task->stack = NULL;
    main_task->kstack_top = (uint32_t)stack_top;
    main_task->cr3 = paging_kernel_cr3();
    main_task->owns_dir = 0;
    main_task->brk = 0;
    main_task->brk_base = 0;
    main_task->mmap_next = 0;
    main_task->cwd = -1;        /* the root directory */
    main_task->level = USER_ADMIN;
    main_task->parent = -1;     /* the root of the process tree */
    main_task->vc = 0;
    main_task->killed = 0;
    main_task->priority = 2;    /* the main thread = idle, lowest priority */
    task_init_fds(main_task);
    task_init_signals(main_task);
    main_task->anycore = 0;             /* the main thread stays on its own */
    main_task->nolock = 0;
    main_task->idle_for_core = 0;    /* it is core 0's idle task */
    main_task->next = main_task;
    cores[0].running = main_task;
    cores[0].idle = main_task;
    scheduler_up = 1;

    /* The main thread is finishing the boot, so from now on it is an
       ordinary task inside kernel code. It gives the lock back only
       once it becomes the idle task (the end of kmain). */
    biglock_enter();
}

int task_scheduler_ready(void)
{
    return scheduler_up;
}

struct task *task_create(const char *name, void (*entry)(void *), void *arg)
{
    struct task *t = task_alloc();
    t->stack = kmalloc(STACK_SIZE);
    t->kstack_top = (uint32_t)t->stack + STACK_SIZE;
    t->cr3 = paging_kernel_cr3();
    t->owns_dir = 0;
    t->brk = 0;
    t->brk_base = 0;
    t->mmap_next = 0;
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
    t->cwd = current ? current->cwd : -1;  /* the child inherits cwd */
    /* Started from a shell, a program gets the shell's level; started
       by another program, the level that one had. The kernel shell is
       not a task of its own - it runs inside a kernel thread - so a
       thread standing in the kernel's own address space is a shell as
       far as this is concerned. */
    t->level = (current && current->cr3 != paging_kernel_cr3())
                   ? current->level : user_shell_level();
    t->parent = current ? current->id : -1;
    t->vc = current ? current->vc : 0;   /* the child writes where the parent does */
    t->killed = 0;
    t->priority = 1;            /* normal priority by default */
    t->anycore = 0;            /* until somebody says otherwise */
    t->nolock = 0;
    t->idle_for_core = -1;
    task_init_fds(t);
    task_init_signals(t);
    t->entry = entry;
    t->arg = arg;
    t->state = TASK_READY;
    t->id = next_id++;
    task_set_name(t, name);

    /* Prepare the stack so that context_switch pops zeroed registers
       and "returns" into the trampoline */
    uint32_t *sp = (uint32_t *)((uint32_t)t->stack + STACK_SIZE);
    *--sp = (uint32_t)task_trampoline;  /* return address from context_switch */
    *--sp = 0;                          /* ebp */
    *--sp = 0;                          /* ebx */
    *--sp = 0;                          /* esi */
    *--sp = 0;                          /* edi */
    t->esp = (uint32_t)sp;

    irqflags_t flags = spin_lock_irq(&lock);
    t->next = current->next;
    current->next = t;
    spin_unlock_irq(&lock, flags);
    return t;
}

/* A task that EVERY core may run.
 *
 * An ordinary kernel task stays on the boot core, and rightly so: it
 * reaches for drivers, the filesystem and all the rest written for a
 * single core. Here the caller takes on that its thread touches only
 * what survives two cores at once. */
struct task *task_create_anycore(const char *name, void (*entry)(void *),
                                  void *arg)
{
    struct task *t = task_create(name, entry, arg);
    t->anycore = 1;
    t->nolock = 1;
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

/* Creates a thread-task sharing the current process's address space
   (cr3). owns_dir=1, but the space is destroyed only by the last
   thread (reap checks cr3_in_use). It inherits the process's
   brk/mmap_next. */
struct task *task_create_thread(const char *name, void (*entry)(void *),
                                void *arg, uint32_t cr3)
{
    struct task *t = task_create(name, entry, arg);
    uint32_t flags = irq_save();
    t->cr3 = cr3;
    t->owns_dir = 1;
    t->brk = current->brk;
    t->brk_base = current->brk_base;
    t->mmap_next = current->mmap_next;
    task_mmap_inherit(t);
    /* The right to run on any core is a property of the program, and a
       thread is the same program. It could not be given away before:
       two cores standing in ONE address space is exactly the case in
       which a changed mapping had to reach a cache the kernel could not
       reach - which is what the shootdown is for. */
    t->anycore = current->anycore;
    irq_restore(flags);
    return t;
}

/* --- signals -------------------------------------------------------- */

/* Sends a signal to the process with the given pid. SIGKILL goes the
   old way (the 'killed' flag) so that it cannot be caught or ignored.
   A sleeping addressee is woken - otherwise the signal would wait
   until morning. */
int task_signal_send(int pid, int sig)
{
    if (sig <= 0 || sig >= NSIG)
        return -1;
    if (sig == SIGKILL)
        return task_kill(pid);

    irqflags_t flags = spin_lock_irq(&lock);
    struct task *t = current;
    int found = 0;
    do {
        if (t->id == pid && t->state != TASK_ZOMBIE) {
            t->sig_pending |= 1u << sig;
            if (t->state == TASK_SLEEPING || t->state == TASK_BLOCKED)
                t->state = TASK_READY;
            found = 1;
            break;
        }
        t = t->next;
    } while (t != current);
    spin_unlock_irq(&lock, flags);
    return found ? 0 : -1;
}

/* Sets a signal handler for the current process.
   'tramp' is the address of the return trampoline in ring 3 (from
   usys.h). */
int task_signal_set(int sig, uint32_t handler, uint32_t tramp)
{
    if (sig <= 0 || sig >= NSIG || sig == SIGKILL)
        return -1;                  /* SIGKILL cannot be caught */
    uint32_t flags = irq_save();
    current->sig_handler[sig] = handler;
    if (tramp)
        current->sig_tramp = tramp;
    irq_restore(flags);
    return 0;
}

/* Takes the lowest pending signal (0 = none waiting) */
int task_signal_take(void)
{
    uint32_t flags = irq_save();
    int sig = 0;
    for (int i = 1; i < NSIG; i++) {
        if (current->sig_pending & (1u << i)) {
            current->sig_pending &= ~(1u << i);
            sig = i;
            break;
        }
    }
    irq_restore(flags);
    return sig;
}

/* Is any signal waiting whose delivery would change something? The
   blocking read asks this - otherwise a process would wait for a key
   forever and the signal would only be delivered once one arrived. */
int task_signal_pending(void)
{
    uint32_t flags = irq_save();
    uint32_t p = current->sig_pending;
    int any = 0;
    for (int i = 1; i < NSIG; i++)
        if ((p & (1u << i)) && current->sig_handler[i] != SIG_IGN) {
            any = 1;
            break;
        }
    irq_restore(flags);
    return any;
}

uint32_t task_signal_handler(int sig)
{
    return (sig > 0 && sig < NSIG) ? current->sig_handler[sig] : SIG_DFL;
}

uint32_t task_signal_tramp(void)
{
    return current->sig_tramp;
}

int task_signal_in_handler(void)
{
    return current->in_handler;
}

void task_signal_enter(uint32_t frame)
{
    current->sig_frame = frame;
    current->in_handler = 1;
}

uint32_t task_signal_leave(void)
{
    uint32_t frame = current->in_handler ? current->sig_frame : 0;
    current->in_handler = 0;
    current->sig_frame = 0;
    return frame;
}

/* Memory reclaim: walks the address spaces of processes and asks each
   of them for one page to give to disk. It starts from the one after
   the last visited so as not to strip the same process over and over.
   Returns 1 when a frame was freed.

   Kernel threads (the kernel cr3) are skipped - kernel memory does not
   go to disk.

   The whole job runs with interrupts off, and that is not merely
   caution about the task list: between clearing a page entry and
   writing the page to disk nobody may get the CPU. Otherwise a thread
   sharing that space would find a page marked "already on disk" that
   is not there yet. */
int task_reclaim_page(void)
{
    static struct task *cursor;
    if (!current)                   /* the scheduler has not started yet */
        return 0;
    /* Evicting pages to disk stays on the boot core. It reaches for the
       swap file and the block layer, and those were written for a
       single core; a secondary core running out of memory is to say so
       plainly rather than quietly stir the disk. */
    if (smp_core() != 0)
        return 0;
    uint32_t flags = irq_save();

    struct task *start = cursor && cursor->state != TASK_ZOMBIE
                             ? cursor : current;
    struct task *t = start;
    uint32_t kernel_cr3 = paging_kernel_cr3();
    int freed = 0;

    do {
        t = t->next;
        /* In batches: sweeping the page tables alone costs more than
           writing one page, so it does not pay to walk them from
           scratch for every missing frame */
        if (t->cr3 != kernel_cr3 && t->state != TASK_ZOMBIE &&
            paging_reclaim_pages(t->cr3, 32)) {
            cursor = t;
            freed = 1;
            break;
        }
    } while (t != start);

    irq_restore(flags);
    return freed;
}

/* Renaming the current task - exec replaces the program, so 'ps' is to
   show the new one, not the one it grew out of */
void task_rename(const char *name)
{
    uint32_t flags = irq_save();
    task_set_name(current, name);
    /* The new process image no longer contains the code of the old
       handlers - signals go back to their default reactions (as in
       Unix after exec) */
    for (int i = 0; i < NSIG; i++)
        if (current->sig_handler[i] != SIG_IGN)
            current->sig_handler[i] = SIG_DFL;
    current->sig_tramp = 0;
    current->sig_frame = 0;
    current->in_handler = 0;
    irq_restore(flags);
}

/* Forking a process (fork): the child gets a ready copy of the address
   space (cr3, see paging_clone_address_space) and everything else from
   the parent - the heap, the mmap area, the working directory, the
   priority and the descriptor table. Pipe ends inherited by the child
   get one more owner, otherwise closing them in the child would give
   the parent a premature EOF. */
struct task *task_fork(void (*entry)(void *), void *arg, uint32_t cr3)
{
    struct task *t = task_create(current->name, entry, arg);
    uint32_t flags = irq_save();
    t->cr3 = cr3;
    t->owns_dir = 1;
    t->brk = current->brk;
    t->brk_base = current->brk_base;
    t->mmap_next = current->mmap_next;
    task_mmap_inherit(t);
    t->cwd = current->cwd;
    t->level = current->level;
    t->priority = current->priority;
    /* The child inherits the right to run on any core: that is a
       property of the program, not of a single task */
    t->anycore = current->anycore;
    /* Signal handlers are inherited from the parent; pending signals are
       not - those are the parent's business. The trampoline is at the
       same place in the code, because the child has a copy of the same
       image. */
    t->sig_tramp = current->sig_tramp;
    for (int i = 0; i < NSIG; i++)
        t->sig_handler[i] = current->sig_handler[i];
    for (int i = 0; i < TASK_MAX_FD; i++) {
        t->fds[i] = current->fds[i];
        if (t->fds[i].used && t->fds[i].kind == FD_PIPE) {
            if (t->fds[i].readable)
                pipe_add_reader(t->fds[i].pipe);
            if (t->fds[i].writable)
                pipe_add_writer(t->fds[i].pipe);
        }
    }
    irq_restore(flags);
    return t;
}

/* Is the address space still used by some other thread? exec asks this
   before destroying the old process image. */
int task_cr3_shared(uint32_t cr3)
{
    irqflags_t flags = spin_lock_irq(&lock);
    int shared = 0;
    struct task *t = current->next;
    while (t != current) {
        if (t->cr3 == cr3 && t->state != TASK_ZOMBIE) {
            shared = 1;
            break;
        }
        t = t->next;
    }
    spin_unlock_irq(&lock, flags);
    return shared;
}

/* Is some other task still using this address space (CR3)? The threads
   of a process share a CR3 - we destroy the space only when the last
   of them finishes. */
static int cr3_in_use(uint32_t cr3, struct task *except)
{
    struct task *t = current;
    do {
        if (t != except && t->cr3 == cr3)
            return 1;
        t = t->next;
    } while (t != current);
    return 0;
}

static void reap_zombies(void)
{
    struct task *t = current;
    while (t->next != current) {
        if (t->next->state == TASK_ZOMBIE) {
            struct task *z = t->next;
            t->next = z->next;
            /* Destroy the address space only when no other (process) thread
               is using it any more. A zombie's CR3 is not active. */
            if (z->owns_dir && !cr3_in_use(z->cr3, z))
                paging_destroy_address_space(z->cr3);
            for (int i = 0; i < MMAP_FILES; i++)
                if (z->maps[i].blocks)
                    kfree(z->maps[i].blocks);
            kfree(z->stack);
            task_free(z);
        } else {
            t = t->next;
        }
    }
}

/* Wakes sleepers whose time has come and picks the next task by
   priority: a higher priority (a smaller number) preempts a lower one;
   equals run round-robin; the current task stays when it is strictly
   higher than everything ready. */
/* May core 'nr' take this task?
 *
 * The boot core runs everything, as it always has. A secondary core -
 * only what somebody explicitly allowed, because the rest of the
 * kernel is written for a single core and quietly counts on it.
 *
 * An idle task belongs to its own core and to nobody else: it stands
 * on the stack that core was given at startup. */
static int may_run(const struct task *t, int nr)
{
    if (t->idle_for_core >= 0)
        return t->idle_for_core == nr;
    return nr == 0 || t->anycore;
}

static struct task *pick_next(int nr)
{
    uint32_t now = pit_ticks();

    /* Wake the sleepers whose time has passed */
    struct task *t = current->next;
    do {
        if (t->state == TASK_SLEEPING && (int32_t)(now - t->wake_at) >= 0)
            t->state = TASK_READY;
        t = t->next;
    } while (t != current->next);

    /* The highest priority (smallest number) among the ready ones */
    int best = 1 << 30;
    t = current;
    do {
        if (t->state == TASK_READY && may_run(t, nr) && t->priority < best)
            best = t->priority;
        t = t->next;
    } while (t != current);
    if (best == (1 << 30))
        return NULL;                  /* nobody is ready - the current one carries on */

    /* The current task strictly higher than anything ready -> it stays */
    if (current->state == TASK_RUNNING && current->priority < best)
        return NULL;

    /* Round-robin among the ready ones of priority 'best', starting after current */
    t = current->next;
    do {
        if (t->state == TASK_READY && may_run(t, nr) &&
            t->priority == best)
            return t;
        t = t->next;
    } while (t != current->next);

    return NULL;
}

/* Choosing the next task and switching to it.
 *
 * ALWAYS called with interrupts off - either from an interrupt handler
 * or from under irq_save. We take the task list lock without touching
 * IF: if the timer stepped into the middle of a section holding that
 * lock, it would wait for it forever, because only the code it just
 * interrupted could release it.
 *
 * We hold the lock ACROSS the switch and it is released by whoever
 * arrives on the other side - because only then is the previous task's
 * stack really free. A task that is ending never returns here and
 * releases nothing; its successor does it for it.
 */
static void switch_to(int new_state)
{
    int nr = smp_core();
    if (!cores[nr].running)   /* the timer ticks before the scheduler starts */
        return;

    /* We give the big kernel lock up FOR THE DURATION OF THE SWITCH.
       Holding it across a context change would mean that a task going
       to sleep in the middle of a system call closes the kernel for
       everybody until it wakes up again. The 'depth' variable lives on
       THIS task's stack, so each one gets exactly its own back. */
    int depth = biglock_release_all();

    spin_lock(&lock);
    /* We change the current task's state ALREADY UNDER THE LOCK,
       together with picking the next one. Setting it earlier would
       leave a window in which the task is marked as sleeping while its
       stack is still in use - and a second core could wake it in the
       meantime and step onto that stack. */
    if (new_state >= 0)
        cores[nr].running->state = (enum task_state)new_state;
    reap_zombies();

    struct task *next = pick_next(nr);
    if (!next) {                /* nobody else is ready - carry on */
        spin_unlock(&lock);
        biglock_restore(depth);
        return;
    }

    struct task *prev = cores[nr].running;
    if (prev->state == TASK_RUNNING)
        prev->state = TASK_READY;
    next->state = TASK_RUNNING;
    cores[nr].running = next;
    /* An interrupt from this thread's ring 3 is to land on ITS kernel
       stack, and in THIS core's task state segment - each has its own */
    tss_set_esp0(next->kstack_top);
    /* Change the address space only when needed (reloading CR3 empties
       the TLB - a waste of cycles between kernel threads) */
    if (next->cr3 != prev->cr3)
        tlb_switch(next->cr3);
    context_switch(&prev->esp, next->esp);
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
 * was woken, and nobody else has the right to run it.
 *
 * This function does not return. From now on the core has two things
 * to do: compute what it was ordered to, and run a task if there is
 * one for it.
 */
void task_core_enter(int nr)
{
    if (nr <= 0 || nr >= SMP_CORES || !scheduler_up)
        return;

    struct task *t = task_alloc();
    if (!t)
        return;
    t->id = -nr;                 /* a negative number: this is not a process */
    task_set_name(t, "idle");
    /* the core's number goes right after the name, wherever it ends -
       a fixed index is a trap for whoever renames the task */
    int len = 0;
    while (t->name[len])
        len++;
    t->name[len] = (char)('0' + nr);
    t->name[len + 1] = '\0';
    t->state = TASK_RUNNING;
    t->stack = NULL;             /* its stack comes from the boot trampoline */
    t->kstack_top = 0;
    t->cr3 = paging_kernel_cr3();
    t->owns_dir = 0;
    t->brk = t->brk_base = t->mmap_next = 0;
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
    t->cwd = -1;
    t->level = USER_ADMIN;       /* a kernel thread is the kernel */
    t->parent = -1;
    t->vc = 0;
    t->killed = 0;
    t->priority = 2;
    t->anycore = 0;
    t->nolock = 1;         /* an idle task does not take the lock */
    t->idle_for_core = nr;
    task_init_fds(t);
    task_init_signals(t);

    irqflags_t flags = spin_lock_irq(&lock);
    t->next = cores[0].running->next;
    cores[0].running->next = t;
    cores[nr].running = t;
    cores[nr].idle = t;
    spin_unlock_irq(&lock, flags);

    __asm__ volatile("sti");
    for (;;) {
        smp_run_work(nr);
        task_yield();
        /* Nothing for me - stand still until the next interrupt.
           This core's timer ticks a hundred times a second, and a
           wake-up from the boot core arrives at once. */
        __asm__ volatile("hlt");
    }
}

void task_yield(void)
{
    uint32_t flags = irq_save();
    schedule();
    irq_restore(flags);
}

void task_sleep(uint32_t ms)
{
    uint32_t flags = irq_save();
    current->wake_at = pit_ticks() + (ms + 9) / 10;  /* one tick = 10 ms */
    switch_to(TASK_SLEEPING);
    irq_restore(flags);
}

/* Blocking with no deadline - somebody has to wake us via task_unblock */
void task_block(void)
{
    uint32_t flags = irq_save();
    switch_to(TASK_BLOCKED);
    irq_restore(flags);
}

/* Called with IF=0 (IRQ context or a critical section) */
void task_unblock(struct task *t)
{
    if (t->state == TASK_BLOCKED)
        t->state = TASK_READY;
}

struct task *task_current(void)
{
    return current;
}

int task_id_of(struct task *t)
{
    return t->id;
}

int task_current_id(void)
{
    return current->id;
}

int task_alive(int id)
{
    irqflags_t flags = spin_lock_irq(&lock);
    int alive = 0;
    struct task *t = current;
    do {
        if (t->id == id && t->state != TASK_ZOMBIE) {
            alive = 1;
            break;
        }
        t = t->next;
    } while (t != current);
    spin_unlock_irq(&lock, flags);
    return alive;
}

void task_exit(int code)
{
    /* Write back to disk what the process wrote into a writable file
       mapping and has not managed to write back itself. This has to
       happen HERE, while its address space is still alive: the zombie
       reaper looks at those pages from somebody else's CR3 already. */
    process_msync();

    /* Close the descriptors - pipe ends have to release the other side
       (the reader gets EOF, the writer a broken pipe) */
    file_close_all();

    irqflags_t flags = spin_lock_irq(&lock);
    current->exit_code = code;
    /* Remember the code straight away - wait sees 'not alive' already at
       ZOMBIE, and the reaper may clean the task up before wait reads it */
    exit_record(current->id, current->parent, code);

    /* Children that outlived us are put in the care of their own
       grandparent (and, failing that, of the main thread) - otherwise
       nobody would ever collect their exit codes. */
    /* The parent gets a SIGCHLD - by default it will ignore it, but it
       may want to know at once that there is something to collect
       through wait */
    if (current->parent >= 0) {
        struct task *p = current->next;
        while (p != current) {
            if (p->id == current->parent && p->state != TASK_ZOMBIE) {
                p->sig_pending |= 1u << SIGCHLD;
                break;
            }
            p = p->next;
        }
    }

    int grandparent = current->parent >= 0 ? current->parent : 0;
    struct task *t = current->next;
    while (t != current) {
        if (t->parent == current->id)
            t->parent = grandparent;
        t = t->next;
    }
    for (int i = 0; i < EXIT_SLOTS; i++)
        if (exit_codes[i].valid && exit_codes[i].parent == current->id)
            exit_codes[i].parent = grandparent;
    /* We release the lock BEFORE the switch: switch_to takes it itself,
       and taking it twice on the same core would never end. Interrupts
       stay off - this task has nowhere left to return to anyway. */
    spin_unlock(&lock);
    switch_to(TASK_ZOMBIE);
    (void)flags;
    /* a zombie will never be picked again */
    for (;;)
        __asm__ volatile("hlt");
}

/* Waits for the process with the given pid to end and returns its exit code */
int task_wait(int pid)
{
    while (task_alive(pid))
        task_sleep(20);
    return exit_take(pid);
}

/* Does the current task still have live children? */
static int has_children(void)
{
    irqflags_t flags = spin_lock_irq(&lock);
    int any = 0;
    struct task *t = current->next;
    while (t != current) {
        if (t->parent == current->id && t->state != TASK_ZOMBIE) {
            any = 1;
            break;
        }
        t = t->next;
    }
    spin_unlock_irq(&lock, flags);
    return any;
}

/* Takes the exit code of any finished child; -1 when none has finished
   yet */
static int take_child_exit(int *code)
{
    irqflags_t flags = spin_lock_irq(&lock);
    int pid = -1;
    for (int i = 0; i < EXIT_SLOTS; i++) {
        if (exit_codes[i].valid && exit_codes[i].parent == current->id) {
            exit_codes[i].valid = 0;
            pid = exit_codes[i].pid;
            *code = exit_codes[i].code;
            break;
        }
    }
    spin_unlock_irq(&lock, flags);
    return pid;
}

/* Waits for any child of its own and returns its pid (the exit code
   goes into *code). -1 means there is nothing left to wait for - no
   children at all. */
int task_wait_any(int *code)
{
    for (;;) {
        int pid = take_child_exit(code);
        if (pid >= 0)
            return pid;
        if (!has_children())
            return -1;
        task_sleep(20);
    }
}

/* Marks the process with the given pid to be killed. The actual end
   happens when that process returns to user mode (task_kill_pending).
   It is woken if sleeping/waiting, so that it reaches the return point
   sooner. */
int task_kill(int pid)
{
    irqflags_t flags = spin_lock_irq(&lock);
    struct task *t = current;
    int found = 0;
    do {
        if (t->id == pid && t->state != TASK_ZOMBIE) {
            t->killed = 1;
            if (t->state == TASK_SLEEPING || t->state == TASK_BLOCKED)
                t->state = TASK_READY;
            found = 1;
            break;
        }
        t = t->next;
    } while (t != current);
    spin_unlock_irq(&lock, flags);
    return found ? 0 : -1;
}

/* Kills the first live task with the given name and returns its pid
   (or -1). Useful when the pid is not known - e.g. an application was
   launched from the desktop menu rather than from a shell. */
/* Has the current task been marked to be killed? This lets long waits
   inside the kernel (e.g. receiving from a socket) break off instead
   of keeping the process alive until their own timeout. */
int task_killed(void)
{
    return current->killed;
}

int task_kill_name(const char *name)
{
    int pid = task_find_name(name);
    return pid >= 0 && task_kill(pid) == 0 ? pid : -1;
}

/* Pid of a live task with the given name (-1 = there is none).
   Separate from killing, because the shell can send any signal by
   name. */
int task_find_name(const char *name)
{
    irqflags_t flags = spin_lock_irq(&lock);
    struct task *t = current;
    int pid = -1;
    do {
        if (t->state != TASK_ZOMBIE && !strcmp(t->name, name)) {
            pid = t->id;
            break;
        }
        t = t->next;
    } while (t != current);
    spin_unlock_irq(&lock, flags);
    return pid;
}

/* Called on return to ring 3: if the process has a pending kill, it
   ends here (in its own context). */
void task_kill_pending(void)
{
    if (current->killed) {
        printk("[kernel] process '%s' (pid %d) killed (code -9)\n",
               current->name, current->id);
        task_exit(-9);
    }
}

void task_use_address_space(uint32_t dir_phys)
{
    uint32_t flags = irq_save();
    current->cr3 = dir_phys;
    current->owns_dir = 1;
    tlb_switch(dir_phys);
    irq_restore(flags);
}

uint32_t task_current_cr3(void)
{
    return current->cr3;
}

/* The current task's console. Before the scheduler starts there is
   none yet - boot messages go to the first one. */
int task_vc(void)
{
    return current ? current->vc : 0;
}

void task_set_vc(int vc)
{
    current->vc = vc;
}

uint32_t task_brk(void)
{
    return current->brk;
}

/* --- what belongs to the SPACE, not to the task ----------------------
 *
 * The heap limit and the next free mmap address describe an address
 * space, and threads share one. Kept per task they were copied at
 * creation and advanced separately afterwards - so two threads asking
 * for memory at the same moment were handed the SAME address, each
 * convinced it was the first there.
 *
 * It was wrong the day threads appeared; it was merely hard to hit
 * while a process could only be on one core. So it is written to every
 * task standing in the same space, and read from whoever asks. */
static void set_in_space(uint32_t off_brk, uint32_t value)
{
    uint32_t flags = irq_save();
    struct task *t = current;
    do {
        if (t->cr3 == current->cr3) {
            if (off_brk)
                t->brk = value;
            else
                t->mmap_next = value;
        }
        t = t->next;
    } while (t != current);
    irq_restore(flags);
}

void task_set_brk(uint32_t brk)
{
    set_in_space(1, brk);
}

uint32_t task_brk_base(void)
{
    return current->brk_base;
}

/* The start of the heap is set by exec together with its end - the
   area between them belongs to the process, but gets frames only on
   first touch */
void task_set_brk_base(uint32_t base)
{
    current->brk_base = base;
}

/* --- Memory-mapped files ------------------------------------------ */

int task_mmap_add(uint32_t start, uint32_t end, uint32_t size,
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

/* Called from the page fault handler, so short and without surprises */
int task_mmap_find(uint32_t addr, uint32_t *first_block,
                     uint32_t *nbytes, uint32_t *write)
{
    if (!current)
        return 0;
    uint32_t page = addr & ~0xFFFu;

    for (int i = 0; i < MMAP_FILES; i++) {
        struct file_map *m = &current->maps[i];
        if (!m->end || page < m->start || page >= m->end)
            continue;

        uint32_t offset = page - m->start;
        uint32_t index = offset / FS_BLOCK;
        if (index >= m->nblocks_of)
            return 0;
        *first_block = index;
        /* How much of the file is left from the start of this page - the
           tail of the last one is to be zeroed, not filled with
           rubbish from disk */
        *nbytes = offset < m->size ? m->size - offset : 0;
        *write = m->write;
        return 1;
    }
    return 0;
}

uint32_t task_mmap_block(uint32_t addr, uint32_t index)
{
    uint32_t page = addr & ~0xFFFu;
    for (int i = 0; i < MMAP_FILES; i++) {
        struct file_map *m = &current->maps[i];
        if (!m->end || page < m->start || page >= m->end)
            continue;
        return index < m->nblocks_of ? m->blocks[index] : 0;
    }
    return 0;
}

void task_mmap_release(void)
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
   block number table itself, so they cannot be shared. */
void task_mmap_inherit(struct task *child)
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
int task_mmap_info(int i, uint32_t *start, uint32_t *end,
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

uint32_t task_mmap_next(void)
{
    return current->mmap_next;
}

void task_set_mmap_next(uint32_t addr)
{
    set_in_space(0, addr);
}

void task_set_priority(int prio)
{
    if (prio < 0)
        prio = 0;
    if (prio > 2)
        prio = 2;
    current->priority = prio;
}

const char *task_current_name(void)
{
    return current ? current->name : "(none)";
}

/* How many tasks there are. The desktop asks once a second, for a
   number on the bar - so it walks the list rather than keeping a
   counter that every birth and death would have to remember. */
int task_count(void)
{
    irqflags_t flags = spin_lock_irq(&lock);
    struct task *t = current;
    int n = 0;
    do {
        n++;
        t = t->next;
    } while (t != current);
    spin_unlock_irq(&lock, flags);
    return n;
}

/* One entry of the task list, as a program in ring 3 sees it.
 *
 * The shell has had 'ps' from the beginning, but it prints - and a
 * program cannot read printing. This hands over the same rows one at a
 * time, the way the directory is read: index 0, 1, 2 ... until there
 * is nothing there. The order is the order of the list, which does not
 * change between two calls unless a task is born or dies - and then
 * the list a program shows is one refresh behind, which is what every
 * task manager in the world shows anyway.
 */
int task_ps(int index, struct psinfo *out)
{
    if (index < 0 || !out)
        return -1;

    irqflags_t flags = spin_lock_irq(&lock);
    struct task *t = current;
    int i = 0;
    int found = 0;
    do {
        if (i == index) {
            out->pid = t->id;
            out->parent = t->parent;
            out->state = (int)t->state;
            out->anycore = t->anycore;
            /* A process has an address space of ITS OWN - that is
               what separates it from a kernel thread, which lives in
               the kernel's. The heap is not the test: kernel threads
               have one too. */
            out->ring3 = t->owns_dir ? 1 : 0;
            for (int k = 0; k < PS_NAME_MAX - 1; k++) {
                out->name[k] = t->name[k];
                if (!t->name[k])
                    break;
            }
            out->name[PS_NAME_MAX - 1] = '\0';
            found = 1;
            break;
        }
        i++;
        t = t->next;
    } while (t != current);
    spin_unlock_irq(&lock, flags);
    return found ? 0 : -1;
}

void task_dump(void)
{
    static const char *state_names[] = { "ready", "running", "spi",
                                         "waiting", "zombie" };

    irqflags_t flags = spin_lock_irq(&lock);
    printk("  ID  PARENT  STATE   NAME\n");
    struct task *t = current;
    do {
        printk("  %d   ", t->id);
        if (t->parent >= 0)
            printk("%d", t->parent);
        else
            printk("-");
        printk("       %s", state_names[t->state]);
        for (int pad = 8 - (int)strlen(state_names[t->state]); pad > 0; pad--)
            printk(" ");
        /* An asterisk: a task that any core may run */
        printk("%s%s%s\n", t->name, t->anycore ? " *" : "",
               t == current ? " (current)" : "");
        t = t->next;
    } while (t != current);
    spin_unlock_irq(&lock, flags);
}
