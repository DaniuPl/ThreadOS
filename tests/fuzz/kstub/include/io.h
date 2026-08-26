/* Zaslepki dostepu do sprzetu - do testow parserow jadra na hoscie */
#ifndef STUB_IO_H
#define STUB_IO_H
#include <stdint.h>
static inline uint32_t irq_save(void) { return 0; }
static inline void irq_restore(uint32_t f) { (void)f; }
static inline void outb(uint16_t p, uint8_t v) { (void)p; (void)v; }
static inline uint8_t inb(uint16_t p) { (void)p; return 0; }
static inline void outw(uint16_t p, uint16_t v) { (void)p; (void)v; }
static inline uint16_t inw(uint16_t p) { (void)p; return 0; }
static inline void outl(uint16_t p, uint32_t v) { (void)p; (void)v; }
static inline uint32_t inl(uint16_t p) { (void)p; return 0; }
#endif
