#ifndef PROTONOS_E1000_H
#define PROTONOS_E1000_H

#include <stdint.h>

/* Sterownik karty Intel 82540EM ("e1000", QEMU: -device e1000)
 *
 * Karta o pokolenie nowsza niz RTL8139 i zbudowana zupelnie inaczej:
 * zamiast portow I/O ma rejestry w pamieci, a zamiast jednego
 * pierscienia bajtow - dwa pierscienie DESKRYPTOROW, po jednym na
 * kazda strone ruchu. Deskryptor mowi, gdzie lezy bufor i co sie
 * z nim stalo; kto go ma na wlasnosc, wynika z tego, kto ostatni
 * przesunal wskaznik ogona.
 *
 * Interfejs jest ten sam, co RTL8139 - i o to chodzi: warstwa wyzej
 * ma nie wiedziec, ktora karta akurat siedzi w gniezdzie.
 */

int e1000_init(void);
int e1000_present(void);
const uint8_t *e1000_mac(void);
int e1000_send(const void *frame, uint16_t len);
uint32_t e1000_rx_packets(void);
uint32_t e1000_tx_packets(void);

#endif
