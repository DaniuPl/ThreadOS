/* Random bytes, for the model: the SNonce the station picks */
#ifndef HW_IWL_RANDOM_H
#define HW_IWL_RANDOM_H
#include <stdint.h>
void random_bytes(void *out, uint32_t n);
#endif
