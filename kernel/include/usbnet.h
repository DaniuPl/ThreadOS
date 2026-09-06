#ifndef PROTONOS_USBNET_H
#define PROTONOS_USBNET_H

#include <stdint.h>

/* A network card on USB - what sits in a docking station
 *
 * A laptop with one socket for everything has its Ethernet on the same
 * bus as its disk and its keyboard. The card speaks RNDIS: a protocol
 * Microsoft invented to carry NDIS - the driver interface of Windows -
 * down a wire, which is why its messages talk about "OIDs" rather than
 * about registers. It is what nearly every USB Ethernet adapter offers
 * (usually alongside something better, and always this one).
 *
 * Frames travel through the bulk endpoints with a forty-four byte
 * header in front of each, and everything else - starting the card,
 * asking for its address, telling it what to receive - goes as
 * "encapsulated commands" through endpoint zero.
 */

/* Looks for such a card and starts it. 1 = there is one. */
int usbnet_init(void);
int usbnet_present(void);

const uint8_t *usbnet_mac(void);
int usbnet_send(const void *frame, uint16_t len);
uint32_t usbnet_rx_packets(void);
uint32_t usbnet_tx_packets(void);

#endif
