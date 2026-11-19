/* The network stack, for the model: what the driver hands up is kept */
#ifndef HW_IWL_NET_H
#define HW_IWL_NET_H
#include <stdint.h>
void net_rx(const uint8_t *frame, uint16_t len);
#endif
