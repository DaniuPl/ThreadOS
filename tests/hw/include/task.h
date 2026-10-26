#ifndef HW_TASK_H
#define HW_TASK_H
#include <stdint.h>
struct task;
struct task *task_create(const char *name, void (*entry)(void *), void *arg);
void task_sleep(uint32_t ms);
#endif
