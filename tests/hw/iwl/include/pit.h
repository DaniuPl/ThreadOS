/* The timer on the host: 100 Hz ticks of card time; asking costs a
   microsecond, so a loop that only watches the clock still gets there */
#ifndef HW_IWL_PIT_H
#define HW_IWL_PIT_H
#include <stdint.h>
uint32_t pit_ticks(void);
#endif
