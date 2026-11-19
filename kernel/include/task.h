#ifndef THREADOS_TASK_H
#define THREADOS_TASK_H

#include <stdint.h>

/* The task calls the shared code makes. The scheduler is kernel64's
   (task64.c); kernel64/compat64.c answers these under the names the
   drivers, the network stack and the desktop use. */

struct task;

/* Creates a kernel thread with its own stack; it starts running on the
   next timer tick */
struct task *task_create(const char *name, void (*entry)(void *), void *arg);

/* A task that EVERY core may run. An ordinary one stays on the boot
   core: the rest of the kernel is written for a single core. */
struct task *task_create_anycore(const char *name, void (*entry)(void *),
                                  void *arg);

/* Is the scheduler up yet (a secondary core waits before joining) */
int task_scheduler_ready(void);

/* A secondary core joins the scheduler. Never returns. */
void task_core_enter(int nr);

/* Picks the next thread and switches context. Call with IF=0. */
void schedule(void);

void task_sleep(uint32_t ms);       /* put the current thread to sleep */
int  task_kill(int pid);            /* kill a process (0/-1) */
int  task_killed(void);             /* is the current task due to die */
int  task_current_id(void);
/* the program the current task is a thread of: its first task's id */
int  task_current_group(void);
int  task_alive(int id);            /* is the task with this id still alive */
/* how an ended program ended: its code, or -1 and why it was stopped */
int  task_end_of(int id, int *code, const char **why, uint64_t *addr);
int  task_count(void);

/* The name a process runs under, into out (max bytes): 1, or 0 when
   there is no such process */
int task_name_of(int pid, char *out, int max);
/* The process that started this one, or -1 */
int task_parent_of(int pid);

int task_vc(void);                  /* the console this task writes to */
int task_cwd(void);                 /* working directory (TFS slot, -1 = root) */

/* Evicts one unused page of some process to disk.
   1 = a frame was freed. The PMM calls this when memory runs out. */
int task_reclaim_page(void);

#endif
