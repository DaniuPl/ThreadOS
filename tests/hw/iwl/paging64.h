/* Mapping on the host: the model notes that BAR0 was mapped; the
   registers themselves are reached only through mmio.h */
#ifndef HW_IWL_PAGING64_H
#define HW_IWL_PAGING64_H
#include <stdint.h>
int paging64_map_phys(uint64_t phys, uint64_t len);
#endif
