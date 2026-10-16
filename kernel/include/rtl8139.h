#ifndef THREADOS_RTL8139_H
#define THREADOS_RTL8139_H

#include <stdint.h>

/* Driver for the Realtek RTL8139 network card (QEMU: -device rtl8139).
   Returns 1 when the card was found and initialised. */
int rtl8139_init(void);

int rtl8139_present(void);

/* The card's MAC address (6 bytes) */
const uint8_t *rtl8139_mac(void);

/* Sends a finished Ethernet frame (without a CRC - the card adds it
   itself). Returns 0 on success, -1 when there is no free TX
   descriptor. */
int rtl8139_send(const void *frame, uint16_t len);

/* Statistics (the 'ifconfig' command) */
uint32_t rtl8139_rx_packets(void);
uint32_t rtl8139_tx_packets(void);

#endif
