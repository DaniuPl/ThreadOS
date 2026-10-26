#ifndef THREADOS_NETDEV_H
#define THREADOS_NETDEV_H

#include <stdint.h>

/* The network card layer
 *
 * The same thing blk.c does for disks: above the drivers there is one
 * name, and which of them answered is a matter for startup. The
 * network code asks for a MAC address and sends frames without knowing
 * whether underneath sits an RTL8139 with I/O ports, an e1000 with
 * registers in memory, or a VirtIO device with split queues.
 *
 * Reception goes the other way: the driver calls net_rx itself when a
 * frame arrives. This layer does not mediate that - it would be
 * passing a pointer along for no reason.
 */

/* Looks for a card and starts the first one found. 1 = there is a card. */
int netdev_init(void);
int netdev_present(void);

/* Name of the driver that answered (for messages) */
const char *netdev_name(void);

const uint8_t *netdev_mac(void);
int netdev_send(const void *frame, uint16_t len);
uint32_t netdev_rx_packets(void);
uint32_t netdev_tx_packets(void);

/* Whether the cable has a link: 1, 0, or -1 for a card that does not
   say (a virtual one is always up). DHCP waits for it - a real PHY
   needs seconds to agree a speed with the other end. */
int netdev_link(void);

#endif
