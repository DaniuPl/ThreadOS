#ifndef HW_XHCI_PAGING64_H
#define HW_XHCI_PAGING64_H
#include <stdint.h>
int paging64_map_phys(uint64_t phys, uint64_t len);
#endif
