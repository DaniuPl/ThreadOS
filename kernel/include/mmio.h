#ifndef THREADOS_MMIO_H
#define THREADOS_MMIO_H

#include <stdint.h>

/* Device registers mapped into memory, read and written 32 bits at a
 * time. A driver that goes through these two and never dereferences
 * its window itself can be run on the host against a model of the
 * card: the model's copy of this header answers instead of the
 * hardware (tests/hw). The volatile access is what keeps the compiler
 * from merging, reordering or dropping a register access. */

static inline uint32_t mmio_read32(volatile uint8_t *base, uint32_t off)
{
    return *(volatile uint32_t *)(base + off);
}

static inline void mmio_write32(volatile uint8_t *base, uint32_t off,
                                uint32_t value)
{
    *(volatile uint32_t *)(base + off) = value;
}

#endif
