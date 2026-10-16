#ifndef THREADOS_NET_H
#define THREADOS_NET_H

#include <stdint.h>

/* IPv4 addresses are kept in host order: 10.0.2.15 == 0x0A00020F */
#define IP4(a, b, c, d) \
    (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | \
     ((uint32_t)(c) << 8) | (uint32_t)(d))

/* Brings up the network card and the IP stack. 1 = there is a network. */
int net_init(void);

/* Called by the card driver for every received frame */
void net_rx(const uint8_t *frame, uint16_t len);

uint32_t net_ip(void);
uint32_t net_mask(void);
uint32_t net_gw(void);
uint32_t net_dns(void);
int net_configured(void);            /* does the system have an IP address yet */
void net_set_config(uint32_t ip, uint32_t mask, uint32_t gw, uint32_t dns);
void net_set_dns(uint32_t ip);       /* manual change of the DNS server */
void net_use_fallback(void);         /* static fallback configuration */

/* "10.0.2.2" -> 0x0A000202; returns 1 on success */
int net_parse_ip(const char *s, uint32_t *out);
void net_print_ip(uint32_t ip);

void net_ifconfig(void);        /* the 'ifconfig' command */
void net_arp_dump(void);        /* the 'arp' command */

/* Sends 'count' ICMP echo requests and prints the replies.
   Blocks (task_sleep), so call from a thread context, not from an IRQ. */
void net_ping(uint32_t ip, int count);

/* --- UDP sockets ------------------------------------------------------
   'owner' is the pid of the owning process (-1 for kernel sockets);
   sockets left by dead processes are reclaimed on the next open. */

/* port 0 = allocate an ephemeral one. Returns a socket descriptor or -1. */
int net_udp_open(uint16_t port, int owner);
void net_udp_close(int sock, int owner);
uint16_t net_udp_port(int sock);

int net_udp_send(int sock, int owner, uint32_t ip, uint16_t port,
                 const void *data, uint16_t len);

/* Waits up to timeout_ms for a datagram; returns the number of bytes
   or -1. Blocks (task_sleep) - from a thread context only. */
int net_udp_recv(int sock, int owner, uint32_t *from_ip, uint16_t *from_port,
                 void *buf, uint16_t max, uint32_t timeout_ms);

/* A datagram with an explicit source address and destination MAC - for
   the DHCP client, which has neither an IP address nor an ARP entry yet. */
int net_udp_send_raw(uint32_t src_ip, uint32_t dst_ip, const uint8_t *dst_mac,
                     uint16_t src_port, uint16_t dst_port,
                     const void *data, uint16_t len);

/* --- services for the TCP layer --------------------------------------- */

uint16_t net_transport_checksum(uint32_t src_ip, uint32_t dst_ip,
                                uint8_t proto, const uint8_t *seg,
                                uint16_t len);

/* Sends an IP packet to a known MAC (may be called from an interrupt) */
int net_ip_send_mac(uint32_t dst_ip, const uint8_t *dst_mac, uint8_t proto,
                    const void *payload, uint16_t len);

/* Resolves a MAC (through the gateway for off-network addresses); blocks */
int net_resolve_mac(uint32_t ip, uint8_t *out);

#endif
