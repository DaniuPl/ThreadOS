/* The kernel log on the host. It formats only what kernel/printk.c
   does (%s %c %d %u %x %p %%, no widths), so a line that would come out
   wrong on the machine comes out wrong here too. */
#ifndef HW_IWL_PRINTK_H
#define HW_IWL_PRINTK_H
void printk(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#endif
