/* Physical memory on the host: frames from a pool below 2 GiB, so that
   the address handed to the card is the address the driver writes
   through, as with the kernel's identity mapping. Not zeroed, as the
   kernel's are not. */
#ifndef HW_IWL_PMM_H
#define HW_IWL_PMM_H
#include <stdint.h>
#define PAGE_SIZE 4096u
uint32_t pmm_alloc_contiguous(uint32_t n);
#endif
