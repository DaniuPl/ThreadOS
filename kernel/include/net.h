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
void net_set_config(uint32_t ip, uint32_t mask, uint32_t gw);

/* "10.0.2.2" -> 0x0A000202; zwraca 1 przy sukcesie */
int net_parse_ip(const char *s, uint32_t *out);
void net_print_ip(uint32_t ip);

void net_ifconfig(void);        /* komenda 'ifconfig' */
void net_arp_dump(void);        /* komenda 'arp' */

/* Wysyla 'count' zadan ICMP echo i wypisuje odpowiedzi.
   Blokuje (task_sleep), wiec wolac z kontekstu watku, nie z IRQ. */
void net_ping(uint32_t ip, int count);

#endif
