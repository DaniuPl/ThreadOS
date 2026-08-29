#ifndef PROTONOS_NETDEV_H
#define PROTONOS_NETDEV_H

#include <stdint.h>

/* Warstwa kart sieciowych
 *
 * To samo, co blk.c robi dla dyskow: nad sterownikami stoi jedna
 * nazwa, a ktory z nich odpowiedzial, jest sprawa rozruchu. Kod
 * sieciowy pyta o adres MAC i wysyla ramki, nie wiedzac, czy pod
 * spodem jest RTL8139 z portami I/O, czy e1000 z rejestrami
 * w pamieci - a to sa dwa zupelnie inne sposoby rozmowy z karta.
 *
 * Odbior idzie w druga strone: sterownik sam wola net_rx, gdy ramka
 * przyjdzie. Ta warstwa go w tym nie posredniczy - byloby to
 * przekladanie wskaznika bez powodu.
 */

/* Szuka karty i uruchamia pierwsza znaleziona. 1 = jest karta. */
int netdev_init(void);
int netdev_present(void);

/* Nazwa sterownika, ktory odpowiedzial (do komunikatow) */
const char *netdev_name(void);

const uint8_t *netdev_mac(void);
int netdev_send(const void *frame, uint16_t len);
uint32_t netdev_rx_packets(void);
uint32_t netdev_tx_packets(void);

#endif
