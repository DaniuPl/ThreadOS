#ifndef PROTONOS_TCP_H
#define PROTONOS_TCP_H

#include <stdint.h>

/* Minimalne TCP: aktywne otwarcie (klient), pasywne (serwer), przesyl
   danych z potwierdzeniami i retransmisja. 'owner' to pid procesu
   (-1 = polaczenie jadra); polaczenia po martwych procesach sa
   zamykane, tak jak gniazda UDP i okna pulpitu. */

void tcp_init(void);

/* Nawiazuje polaczenie; blokuje do timeout_ms. Zwraca id albo -1. */
int tcp_connect(uint32_t ip, uint16_t port, int owner, uint32_t timeout_ms);

/* Nasluchiwanie na porcie: zwraca id gniazda nasluchujacego albo -1 */
int tcp_listen(uint16_t port, int owner);

/* Odbiera nawiazane polaczenie z kolejki gniazda nasluchujacego */
int tcp_accept(int listener, int owner, uint32_t timeout_ms);

/* Wysyla dane i czeka na potwierdzenie. Zwraca wyslane bajty albo -1. */
int tcp_send(int conn, int owner, const void *data, uint16_t len,
             uint32_t timeout_ms);

/* Odbiera dane: >0 = bajty, 0 = druga strona zamknela (FIN),
   -1 = blad albo timeout. */
int tcp_recv(int conn, int owner, void *buf, uint16_t max,
             uint32_t timeout_ms);

void tcp_close(int conn, int owner);

/* Wejscie z warstwy IP (kontekst przerwania karty) */
void tcp_input(uint32_t src_ip, uint32_t dst_ip, const uint8_t *src_mac,
               const uint8_t *seg, uint16_t len);

void tcp_dump(void);            /* komenda 'netstat' */

#endif
