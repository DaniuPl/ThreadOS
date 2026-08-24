#ifndef PROTONOS_RTL8139_H
#define PROTONOS_RTL8139_H

#include <stdint.h>

/* Sterownik karty sieciowej Realtek RTL8139 (QEMU: -device rtl8139).
   Zwraca 1, gdy karta zostala znaleziona i zainicjalizowana. */
int rtl8139_init(void);

int rtl8139_present(void);

/* Adres MAC karty (6 bajtow) */
const uint8_t *rtl8139_mac(void);

/* Wysyla gotowa ramke ethernetowa (bez CRC - karta dolicza je sama).
   Zwraca 0 przy sukcesie, -1 gdy nie ma wolnego deskryptora TX. */
int rtl8139_send(const void *frame, uint16_t len);

/* Statystyki (komenda 'ifconfig') */
uint32_t rtl8139_rx_packets(void);
uint32_t rtl8139_tx_packets(void);

#endif
