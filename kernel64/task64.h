#ifndef PROTONOS_TASK64_H
#define PROTONOS_TASK64_H

#include <stdint.h>

/* Kernel threads in long mode
 *
 * The same idea as in the 32-bit kernel: a circular list, preemption
 * on every timer tick, a main thread that never sleeps and serves as
 * the idle thread. The only difference is the context switch - six
 * registers instead of four and eight bytes each.
 *
 * There are no processes here yet: no address spaces, no descriptors,
 * no signals. That comes together with ring 3.
 */

struct task64;

void scheduler64_init(void);
/* A task that any core is allowed to run */
struct task64 *task64_create_anycore(const char *name,
                                      void (*entry)(void *), void *arg);

/* Has the scheduler started yet (a secondary core waits before it
   joins) */
/* From now on the current task may be run by any core */
void task_set_anycore(void);

int task_scheduler_ready(void);

/* A secondary core joins the scheduler. Does not return. */
void task_core_enter(int nr);

struct task64 *task64_create(const char *name, void (*entry)(void *),
                             void *arg);
void task64_yield(void);
void task64_sleep(uint32_t ms);
void task64_exit(void);
void task64_exit_code(int code);    /* the same, but with an exit code for the parent */

/* Waits for any of our own children to end. Returns its number and
   writes out the exit code; -1 = there is nothing to wait for. */
int task64_wait(int *code);

/* The deepest living descendant of a task - it is the one holding the
   console and the one Ctrl+C belongs to, not the shell waiting for it */
int task64_foreground(int root);

/* Takes an address space into the current task's ownership and
   switches to it right away. It frees itself when the task ends. */
void task64_enter_space(uint64_t space, uint64_t kstack_top);

/* Replaces the space and hands back the old one - to be freed by the
   caller only after the processor has left it (exec) */
uint64_t task64_swap_space(uint64_t space, uint64_t kstack_top);

/* Changes the current task's name (after an exec it shows what is
   really running) */
void task64_rename(const char *name);

/* Kills the current task (a program error in ring 3). Does not
   return. */
void task64_kill_current(const char *why);
/* Signals. Sending merely queues a signal and wakes a sleeping task -
   it is delivered only by the return to ring 3. */
int task64_signal(int id, int sig);
int task64_signal_pending(void);      /* is anything waiting for delivery */
int task64_take_signal(void);         /* 0 = nothing is waiting */
void task64_set_sighandler(uint64_t handler, uint64_t restorer);
uint64_t task64_sighandler(void);
uint64_t task64_sigrestorer(void);
int task64_current_id(void);
int task64_id_current(void);

/* The task's virtual console (inherited from the parent) */
/* Handing pages out to disk when the PMM has no free frames left */
int task64_reclaim_page(void);

int task64_vc(void);
void task64_set_vc(int vc);
int task64_alive(int id);            /* is the task with this number alive */

/* The task's working directory - a child starts where the parent
   stood */
int task64_cwd(void);
void task64_set_cwd(int slot);

/* The program's heap: the kernel moves the limit, and the pages are
   allocated only by the page fault handler, on the first touch */
void task64_set_brk_base(uint64_t base);
void task64_set_brk(uint64_t brk);
uint64_t task64_brk(void);
uint64_t task64_brk_base(void);
int task64_in_heap(uint64_t addr);
void task64_inherit_brk(struct task64 *child);

/* --- Files mapped into memory --------------------------------------
 *
 * A file seen as memory: the program gets an address and the pages
 * appear under its fingers - just like the heap, only filled with
 * content from disk instead of zeros.
 *
 * We work the block numbers out once, when the mapping is set up, and
 * from then on read them through the block layer, bypassing the PFS
 * code. The reason is the same as with page swapping: PFS has one
 * shared buffer for a block, and a page fault can land in the middle
 * of any file operation.
 *
 * MMAP_FILES says how many mappings one process may have at a time. */
#define MMAP_FILES 4

/* Sets up a mapping: remembers the address range and the block
   numbers. Returns 0 or -1. The blocks pass into the task's
   ownership. */
int task64_mmap_add(uint64_t start, uint64_t end, uint32_t size,
                      uint32_t *blocks, uint32_t nblocks_of, uint32_t write);

/* Does the address belong to one of the mappings. If so, it writes out
   the PFS block number, how many bytes of the file are left from that
   point and whether the mapping is writable. */
int task64_mmap_find(uint64_t addr, uint32_t *first_block,
                       uint32_t *nbytes, uint32_t *write);

/* A description of the current process's i-th mapping (0 = a free
   entry) - for flushing dirty pages, which walks them in turn. */
int task64_mmap_info(int i, uint64_t *start, uint64_t *end,
                     uint32_t *size, uint32_t *write);

/* The PFS block number at the given index in the mapping covering an
   address */
uint32_t task64_mmap_block(uint64_t addr, uint32_t index);

/* Cleanup: on exec and at the end of a task */
void task64_mmap_release(void);
void task64_mmap_inherit(struct task64 *child);

/* The next free address in the process's mapping area */
uint64_t task64_mmap_next(void);
void task64_set_mmap_next(uint64_t adr);

/* Descriptors of open files - the table belongs to the task */
/* The kind of thing sitting under a number (has to match task64.c) */
#define FDK_FREE   0
#define FDK_FILE    1
#define FDK_POTOK_R 2
#define FDK_POTOK_W 3

int task64_fd_open(int slot);        /* returns a descriptor number or -1 */
int task64_fd_pipe(int idx, int reader_task);
int task64_fd_kind(int fd);
int task64_fd_dup2(int old, int fresh_one);
void task64_fd_close_all(void);
int task64_fd_slot(int fd);          /* PFS inode / pipe number */
uint32_t task64_fd_pos(int fd);
void task64_fd_advance(int fd, uint32_t n);
int task64_fd_close(int fd);
void task64_inherit_fds(struct task64 *child);

int  task64_id(const struct task64 *t);
int  task64_count(void);            /* how many tasks are on the list */
void task64_dump(void);

#endif
