#ifndef STUB_PRINTK_H
#define STUB_PRINTK_H
/* In the tests the kernel's messages are only in the way - we swallow
   them */
static inline void printk(const char *fmt, ...) { (void)fmt; }
#endif
