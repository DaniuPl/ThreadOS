#ifndef PROTONOS_NET_H
#define PROTONOS_NET_H

#include <stdint.h>

/* Adresy IPv4 trzymamy w kolejnosci hosta: 10.0.2.15 == 0x0A00020F */
#define IP4(a, b, c, d) \
    (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | \
     ((uint32_t)(c) << 8) | (uint32_t)(d))

/* Podnosi karte sieciowa i stos IP. 1 = jest siec. */
int net_init(void);

/* Wolane przez sterownik karty dla kazdej odebranej ramki */
void net_rx(const uint8_t *frame, uint16_t len);

uint32_t net_ip(void);
uint32_t net_mask(void);
uint32_t net_gw(void);
uint32_t net_dns(void);
int net_configured(void);            /* czy system ma juz adres IP */
void net_set_config(uint32_t ip, uint32_t mask, uint32_t gw, uint32_t dns);
void net_use_fallback(void);         /* statyczna konfiguracja awaryjna */

/* "10.0.2.2" -> 0x0A000202; zwraca 1 przy sukcesie */
int net_parse_ip(const char *s, uint32_t *out);
void net_print_ip(uint32_t ip);

void net_ifconfig(void);        /* komenda 'ifconfig' */
void net_arp_dump(void);        /* komenda 'arp' */

/* Wysyla 'count' zadan ICMP echo i wypisuje odpowiedzi.
   Blokuje (task_sleep), wiec wolac z kontekstu watku, nie z IRQ. */
void net_ping(uint32_t ip, int count);

/* --- gniazda UDP ------------------------------------------------------
   'owner' to pid procesu-wlasciciela (-1 dla gniazd jadra); gniazda po
   martwych procesach sa odzyskiwane przy kolejnym otwarciu. */

/* port 0 = przydziel efemeryczny. Zwraca deskryptor gniazda albo -1. */
int net_udp_open(uint16_t port, int owner);
void net_udp_close(int sock, int owner);
uint16_t net_udp_port(int sock);

int net_udp_send(int sock, int owner, uint32_t ip, uint16_t port,
                 const void *data, uint16_t len);

/* Czeka do timeout_ms na datagram; zwraca liczbe bajtow albo -1.
   Blokuje (task_sleep) - tylko z kontekstu watku. */
int net_udp_recv(int sock, int owner, uint32_t *from_ip, uint16_t *from_port,
                 void *buf, uint16_t max, uint32_t timeout_ms);

/* Datagram z jawnym adresem zrodlowym i MAC-iem odbiorcy - dla klienta
   DHCP, ktory nie ma jeszcze ani adresu IP, ani wpisu ARP. */
int net_udp_send_raw(uint32_t src_ip, uint32_t dst_ip, const uint8_t *dst_mac,
                     uint16_t src_port, uint16_t dst_port,
                     const void *data, uint16_t len);

/* --- uslugi dla warstwy TCP ------------------------------------------- */

uint16_t net_transport_checksum(uint32_t src_ip, uint32_t dst_ip,
                                uint8_t proto, const uint8_t *seg,
                                uint16_t len);

/* Wysyla pakiet IP do znanego MAC-a (mozna wolac z przerwania) */
int net_ip_send_mac(uint32_t dst_ip, const uint8_t *dst_mac, uint8_t proto,
                    const void *payload, uint16_t len);

/* Rozwiazuje MAC (przez brame dla adresow spoza sieci); blokuje */
int net_resolve_mac(uint32_t ip, uint8_t *out);

#endif
