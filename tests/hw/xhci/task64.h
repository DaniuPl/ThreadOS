/* A task's rights, for the model: one task, always the system's */
#ifndef HW_XHCI_TASK64_H
#define HW_XHCI_TASK64_H
int task64_level(void);
int task64_uid(void);
void task64_set_level(int level);
void task64_set_uid(int uid);
#endif
