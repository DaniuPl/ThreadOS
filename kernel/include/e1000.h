#ifndef THREADOS_E1000_H
#define THREADOS_E1000_H

#include <stdint.h>

/* Driver for the Intel 82540EM card ("e1000", QEMU: -device e1000)
 *
 * A generation newer than the RTL8139 and built completely
 * differently: instead of I/O ports it has registers in memory, and
 * instead of a single ring of bytes - two rings of DESCRIPTORS, one
 * for each direction of traffic. A descriptor says where the buffer
 * lies and what happened to it; who owns it follows from who last
 * moved the tail pointer.
 *
 * The interface is the same as the RTL8139's - and that is the point:
 * the layer above is not to know which card happens to be in the slot.
 */

int e1000_init(void);
int e1000_present(void);
const uint8_t *e1000_mac(void);
int e1000_send(const void *frame, uint16_t len);
uint32_t e1000_rx_packets(void);
uint32_t e1000_tx_packets(void);

#endif
