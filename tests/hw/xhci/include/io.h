/* The waits, on the host: each one is a step of the model, which is
   how the machine moves only while the driver waits for it */
#ifndef HW_XHCI_IO_H
#define HW_XHCI_IO_H
#include <stdint.h>
typedef unsigned long irqflags_t;
void model_step(void);
static inline void io_wait(void) { model_step(); }
#endif
