#ifndef THREADOS_RTL8168_H
#define THREADOS_RTL8168_H

#include <stdint.h>

/* Driver for the Realtek RTL8111/8168 family (gigabit Ethernet, PCIe)
 *
 * The wired port of most laptops and boards of the last fifteen years.
 * Despite the name it has little in common with the RTL8139: two rings
 * of sixteen-byte descriptors, like the e1000, but with ownership
 * marked in each descriptor rather than by head and tail registers.
 *
 * Same interface as the other cards. One more call: rtl8168_poll()
 * takes in what has arrived - a thread of the driver's calls it, since
 * whether the card's interrupt reaches the 8259 at all is up to the
 * firmware of the machine.
 */

int rtl8168_init(void);
int rtl8168_present(void);
const uint8_t *rtl8168_mac(void);
int rtl8168_send(const void *frame, uint16_t len);
uint32_t rtl8168_rx_packets(void);
uint32_t rtl8168_tx_packets(void);
int rtl8168_poll(void);
int rtl8168_link(void);            /* 1 = a cable, and the other end answers */

#endif
