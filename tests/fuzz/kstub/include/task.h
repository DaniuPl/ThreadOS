#ifndef STUB_TASK_H
#define STUB_TASK_H
#include <stdint.h>
struct task;
static inline void task_sleep(uint32_t ms) { (void)ms; }
static inline int task_alive(int id) { (void)id; return 1; }
static inline int task_killed(void) { return 0; }
static inline int task_current_id(void) { return 1; }
static inline struct task *task_create(const char *n, void (*e)(void *),
                                       void *a)
{ (void)n; (void)e; (void)a; return 0; }
#endif
