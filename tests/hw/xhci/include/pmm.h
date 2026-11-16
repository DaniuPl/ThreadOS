#ifndef HW_XHCI_PMM_H
#define HW_XHCI_PMM_H
#include <stdint.h>
uint32_t pmm_alloc_frame(void);
void pmm_free_frame(uint32_t addr);
#endif
