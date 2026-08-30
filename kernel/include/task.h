#ifndef PROTONOS_TASK_H
#define PROTONOS_TASK_H

#include <stdint.h>
#include "signal.h"

struct task;

/* The process file descriptor table. fd 0/1/2 = keyboard/console,
   higher fds = files on disk or pipe ends. */
#define TASK_MAX_FD 8

enum fd_kind {
    FD_CONSOLE,           /* keyboard (read) / screen (write) */
    FD_FILE,              /* file on the PFS disk */
    FD_PIPE,              /* end of an inter-process pipe */
};

struct file_desc {
    int used;
    enum fd_kind kind;
    int inode;            /* FD_FILE: PFS inode index */
    uint32_t off;         /* FD_FILE: current offset */
    int pipe;             /* FD_PIPE: pipe index */
    int readable;
    int writable;
};

/* The current task's fd table (for the file syscall layer) */
struct file_desc *task_fds(void);

/* fork: a new task with the given address space, inheriting the
   current one's heap, working directory, priority and descriptors */
struct task *task_fork(void (*entry)(void *), void *arg, uint32_t cr3);

/* --- signals: the task holds the state, signal.c does the delivery --- */
int task_signal_send(int pid, int sig);
int task_signal_set(int sig, uint32_t handler, uint32_t tramp);
int task_signal_take(void);            /* lowest pending one, 0 = none */
int task_signal_pending(void);         /* is anything waiting to be delivered */
uint32_t task_signal_handler(int sig);
uint32_t task_signal_tramp(void);
int task_signal_in_handler(void);
void task_signal_enter(uint32_t frame);
uint32_t task_signal_leave(void);      /* frame address or 0 */

/* Waits for any child of its own: returns its pid and writes the exit
   code into *code. -1 = no children left to wait for. */
int task_wait_any(int *code);

/* Evicts one unused page of some process to disk.
   1 = a frame was freed. The PMM calls this when memory runs out. */
int task_reclaim_page(void);

/* Renames the current task (exec replaces the program) */
void task_rename(const char *name);

/* Is this address space still used by some other thread? */
int task_cr3_shared(uint32_t cr3);

/* Working directory of the current task (PFS slot, -1 = root) */
/* --- Memory-mapped files --------------------------------------------
 *
 * A file seen as memory: the program gets an address and the pages
 * appear under its fingers - just like the heap, only filled with
 * content from disk instead of zeros. MMAP_FILES says how many
 * mappings one process may hold at a time. */
#define MMAP_FILES 4

int task_mmap_add(uint32_t start, uint32_t end, uint32_t size,
                    uint32_t *blocks, uint32_t nblocks_of, uint32_t write);
int task_mmap_find(uint32_t addr, uint32_t *first_block,
                     uint32_t *nbytes, uint32_t *write);
int task_mmap_info(int i, uint32_t *start, uint32_t *end,
                   uint32_t *size, uint32_t *write);
uint32_t task_mmap_block(uint32_t addr, uint32_t index);
void task_mmap_release(void);
void task_mmap_inherit(struct task *child);

/* A task that EVERY core may run. An ordinary one stays on the boot
   core: the rest of the kernel is written for a single core. */
struct task *task_create_anycore(const char *name, void (*entry)(void *),
                                  void *arg);

/* Is the scheduler up yet (a secondary core waits before joining) */
/* From now on any core may run the current task */
void task_set_anycore(void);

int task_scheduler_ready(void);

/* A secondary core joins the scheduler. Never returns. */
void task_core_enter(int nr);

int task_cwd(void);
void task_set_cwd(int slot);

/* Turns the current thread of execution (kmain) into the first task */
void scheduler_init(void);

/* Creates a kernel thread with its own stack; it starts running on the
   next timer tick */
struct task *task_create(const char *name, void (*entry)(void *), void *arg);

/* Creates a thread sharing the current process's address space (cr3) */
struct task *task_create_thread(const char *name, void (*entry)(void *),
                                void *arg, uint32_t cr3);

void task_yield(void);              /* give up the CPU voluntarily */
void task_sleep(uint32_t ms);       /* put the current thread to sleep */
void task_exit(int code);           /* end the current thread with a code */
int  task_wait(int pid);            /* wait for a process, return its code */
int  task_kill(int pid);            /* mark a process to be killed (0/-1) */
int  task_kill_name(const char *name); /* the same by name -> pid or -1 */
int  task_find_name(const char *name); /* pid of a live task with this name */
int  task_killed(void);             /* is the current task due to die */
void task_kill_pending(void);       /* deliver the kill on return to ring 3 */
void task_block(void);              /* sleep until somebody wakes us */
void task_unblock(struct task *t);  /* wake up (call with IF=0) */

struct task *task_current(void);
int task_id_of(struct task *t);
int task_current_id(void);
int task_alive(int id);             /* is the task with this id still alive */

/* Picks the next thread and switches context. Call with IF=0. */
void schedule(void);

/* Prints the list of threads (the 'ps' shell command) */
void task_dump(void);

const char *task_current_name(void);

/* The current task moves to an address space of its own; the page
   directory will be released when the zombie is reaped */
void task_use_address_space(uint32_t dir_phys);

uint32_t task_current_cr3(void);    /* CR3 of the current task */
int task_vc(void);                  /* the console this task writes to */
void task_set_vc(int vc);
uint32_t task_brk(void);            /* end of the process heap (0 = kernel thread) */
void task_set_brk(uint32_t brk);
uint32_t task_brk_base(void);       /* start of the process heap */
void task_set_brk_base(uint32_t base);
uint32_t task_mmap_next(void);      /* next free mmap address */
void task_set_mmap_next(uint32_t addr);
void task_set_priority(int prio);   /* 0=high, 1=normal, 2=low */

#endif
