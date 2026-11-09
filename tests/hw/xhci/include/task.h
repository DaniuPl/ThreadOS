#ifndef HW_XHCI_TASK_H
#define HW_XHCI_TASK_H
#include <stdint.h>
struct task;
struct task *task_create(const char *name, void (*entry)(void *), void *arg);
void task_sleep(uint32_t ms);
int task_scheduler_ready(void);
#endif
