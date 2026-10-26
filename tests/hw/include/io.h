/* The ports, on the host: every access goes to the model of the card */
#ifndef HW_IO_H
#define HW_IO_H
#include <stdint.h>
typedef unsigned long irqflags_t;
uint32_t model_in(uint16_t port, int size);
void model_out(uint16_t port, uint32_t value, int size);
static inline irqflags_t irq_save(void) { return 0; }
static inline void irq_restore(irqflags_t f) { (void)f; }
static inline void io_wait(void) {}
static inline void outb(uint16_t p, uint8_t v) { model_out(p, v, 1); }
static inline uint8_t inb(uint16_t p) { return (uint8_t)model_in(p, 1); }
static inline void outw(uint16_t p, uint16_t v) { model_out(p, v, 2); }
static inline uint16_t inw(uint16_t p) { return (uint16_t)model_in(p, 2); }
static inline void outl(uint16_t p, uint32_t v) { model_out(p, v, 4); }
static inline uint32_t inl(uint16_t p) { return model_in(p, 4); }
#endif
