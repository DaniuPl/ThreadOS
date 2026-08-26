#ifndef STUB_PRINTK_H
#define STUB_PRINTK_H
/* W testach komunikaty jadra tylko przeszkadzaja - polykamy je */
static inline void printk(const char *fmt, ...) { (void)fmt; }
#endif
