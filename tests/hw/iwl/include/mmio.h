/* The card's registers, on the host: every access goes to the model
   (tests/hw/iwl/card.c), which moves its clock by the time a PCIe
   access takes and checks the access against the specification */
#ifndef HW_IWL_MMIO_H
#define HW_IWL_MMIO_H
#include <stdint.h>
uint32_t model_mmio_read(const volatile void *base, uint32_t off);
void model_mmio_write(const volatile void *base, uint32_t off, uint32_t v);
static inline uint32_t mmio_read32(volatile uint8_t *base, uint32_t off)
{
    return model_mmio_read(base, off);
}
static inline void mmio_write32(volatile uint8_t *base, uint32_t off,
                                uint32_t value)
{
    model_mmio_write(base, off, value);
}
#endif
