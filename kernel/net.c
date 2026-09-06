/* The ProtonOS network stack: Ethernet + ARP + IPv4 + ICMP + UDP + TCP
 *
 * Minimal but real: the system answers ARP queries and pings from
 * outside on its own, and the 'ping' command resolves a MAC address
 * through ARP and measures the round-trip time of an ICMP echo.
 *
 * The default configuration matches QEMU's user network
 * (-netdev user): 10.0.2.15/24, gateway 10.0.2.2.
 *
 * Incoming frames arrive here from the card's interrupt, so the
 * handling is short: an answer, or a write into the ARP table / the
 * ping state. The waiting happens in the thread that called
 * net_ping().
 */

#include "include/net.h"
#include "include/netdev.h"
#include "include/printk.h"
#include "include/string.h"
#include "include/io.h"
#include "include/pit.h"
#include "include/task.h"
#include "include/tcp.h"

#define ETH_HDR_LEN   14
#define ETH_P_IP      0x0800
#define ETH_P_ARP     0x0806

#define ARP_REQUEST   1
#define ARP_REPLY     2

#define IP_PROTO_ICMP 1
#define IP_PROTO_UDP  17
#define IP_PROTO_TCP  6

#define ICMP_ECHO_REQUEST 8
#define ICMP_ECHO_REPLY   0

#define ARP_CACHE_SIZE 8

/* A fallback configuration in case there is no DHCP server on the
   network - the values of QEMU's user network */
#define FALLBACK_IP   IP4(10, 0, 2, 15)
#define FALLBACK_MASK IP4(255, 255, 255, 0)
#define FALLBACK_GW   IP4(10, 0, 2, 2)

/* We start without an address: only DHCP (or the fallback) gives the
   system an IP */
static uint32_t my_ip, my_mask, my_gw, my_dns;

static const uint8_t bcast_mac[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

struct arp_entry {
    uint32_t ip;
    uint8_t mac[6];
    int used;
};

static struct arp_entry arp_cache[ARP_CACHE_SIZE];
static int arp_next;                 /* we overwrite the oldest entry */

/* UDP sockets: each has a port and a short datagram queue, filled by
   the card's interrupt and drained by receivers (the kernel or a
   ring 3 process through a syscall). */
#define UDP_SOCKS     8
#define UDP_QUEUE     4
#define UDP_DATA_MAX  1024
#define UDP_PORT_MIN  49152        /* ephemeral ports */

struct udp_dgram {
    uint32_t from_ip;
    uint16_t from_port;
    uint16_t len;
    uint8_t data[UDP_DATA_MAX];
};

struct udp_sock {
    int used;
    int owner;                     /* the process pid; -1 = a kernel socket */
    uint16_t port;
    int head, count;               /* a ring queue */
    struct udp_dgram q[UDP_QUEUE];
};

static struct udp_sock udp_socks[UDP_SOCKS];
static uint16_t udp_next_port = UDP_PORT_MIN;
static uint32_t udp_dropped;

/* The state of the current ping (filled in from the card's interrupt) */
static volatile int ping_waiting;
static volatile uint16_t ping_id, ping_seq;
static volatile int ping_got;
static volatile uint32_t ping_reply_tick;
static volatile uint8_t ping_ttl;

static uint32_t rx_dropped;

/* --- byte order conversions -------------------------------------------- */

static inline uint16_t hton16(uint16_t v)
{
    return (uint16_t)((v >> 8) | (v << 8));
}

static inline uint16_t ntoh16(uint16_t v)
{
    return hton16(v);
}

static void put32(uint8_t *p, uint32_t v)      /* IP -> network (big endian) */
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t get32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

/* The Internet checksum: the one's complement of a sum of 16-bit words */
static uint16_t checksum(const uint8_t *data, uint32_t len)
{
    uint32_t sum = 0;
    while (len > 1) {
        sum += get16(data);
        data += 2;
        len -= 2;
    }
    if (len)
        sum += (uint32_t)data[0] << 8;
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

/* --- addresses ---------------------------------------------------------- */

uint32_t net_ip(void)
{
    return my_ip;
}

uint32_t net_mask(void)
{
    return my_mask;
}

uint32_t net_gw(void)
{
    return my_gw;
}

uint32_t net_dns(void)
{
    return my_dns;
}

int net_configured(void)
{
    return my_ip != 0;
}

void net_set_config(uint32_t ip, uint32_t mask, uint32_t gw, uint32_t dns)
{
    my_ip = ip;
    my_mask = mask;
    my_gw = gw;
    my_dns = dns;
    for (int i = 0; i < ARP_CACHE_SIZE; i++)
        arp_cache[i].used = 0;
}

/* Pointing at a DNS server by hand (the 'dns' command) - useful when
   DHCP did not supply one or we want to query a different one */
void net_set_dns(uint32_t ip)
{
    my_dns = ip;
}

void net_use_fallback(void)
{
    net_set_config(FALLBACK_IP, FALLBACK_MASK, FALLBACK_GW, 0);
}

void net_print_ip(uint32_t ip)
{
    printk("%u.%u.%u.%u", (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
           (ip >> 8) & 0xFF, ip & 0xFF);
}

int net_parse_ip(const char *s, uint32_t *out)
{
    uint32_t parts[4] = { 0, 0, 0, 0 };
    int part = 0, digits = 0;

    for (; *s; s++) {
        if (*s >= '0' && *s <= '9') {
            parts[part] = parts[part] * 10 + (uint32_t)(*s - '0');
            if (parts[part] > 255 || ++digits > 3)
                return 0;
        } else if (*s == '.') {
            if (!digits || part == 3)
                return 0;
            part++;
            digits = 0;
        } else {
            return 0;
        }
    }
    if (part != 3 || !digits)
        return 0;

    *out = IP4(parts[0], parts[1], parts[2], parts[3]);
    return 1;
}

/* --- the ARP table ------------------------------------------------------ */

static const uint8_t *arp_lookup(uint32_t ip)
{
    for (int i = 0; i < ARP_CACHE_SIZE; i++)
        if (arp_cache[i].used && arp_cache[i].ip == ip)
            return arp_cache[i].mac;
    return 0;
}

static void arp_store(uint32_t ip, const uint8_t *mac)
{
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].used && arp_cache[i].ip == ip) {
            memcpy(arp_cache[i].mac, mac, 6);
            return;
        }
    }
    struct arp_entry *e = &arp_cache[arp_next];
    arp_next = (arp_next + 1) % ARP_CACHE_SIZE;
    e->ip = ip;
    memcpy(e->mac, mac, 6);
    e->used = 1;
}

void net_arp_dump(void)
{
    int shown = 0;
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!arp_cache[i].used)
            continue;
        net_print_ip(arp_cache[i].ip);
        const uint8_t *m = arp_cache[i].mac;
        printk("  %x:%x:%x:%x:%x:%x\n", m[0], m[1], m[2], m[3], m[4], m[5]);
        shown++;
    }
    if (!shown)
        printk("the ARP table is empty (try 'ping <ip>')\n");
}

/* --- sending ------------------------------------------------------------ */

/* Resolving a MAC (defined below, next to ping) - it blocks, so it may
   only be used from a thread context */
static const uint8_t *arp_resolve(uint32_t ip);

/* Assembles an Ethernet header in a buffer and returns its length */
static int eth_header(uint8_t *frame, const uint8_t *dst_mac, uint16_t type)
{
    memcpy(frame, dst_mac, 6);
    memcpy(frame + 6, netdev_mac(), 6);
    put16(frame + 12, type);
    return ETH_HDR_LEN;
}

static void arp_send(uint16_t op, uint32_t target_ip, const uint8_t *target_mac)
{
    uint8_t frame[ETH_HDR_LEN + 28];
    uint8_t *a = frame + ETH_HDR_LEN;

    eth_header(frame, op == ARP_REQUEST ? bcast_mac : target_mac, ETH_P_ARP);

    put16(a + 0, 1);                 /* hardware: Ethernet */
    put16(a + 2, ETH_P_IP);          /* protocol: IPv4 */
    a[4] = 6;
    a[5] = 4;
    put16(a + 6, op);
    memcpy(a + 8, netdev_mac(), 6);
    put32(a + 14, my_ip);
    memcpy(a + 18, op == ARP_REQUEST ? bcast_mac : target_mac, 6);
    put32(a + 24, target_ip);

    netdev_send(frame, sizeof(frame));
}

/* Assembles and sends an IPv4 packet with a ready payload.
   The frame goes through a shared static buffer - building it and
   handing it to the card happens with interrupts disabled, so a ping
   from a thread and an answer assembled in the card's interrupt do not
   get in each other's way (a kernel thread stack is only 8 KiB, there
   is no room for frames on it). */
#define IP_PAYLOAD_MAX 1480

static uint8_t tx_frame[ETH_HDR_LEN + 20 + IP_PAYLOAD_MAX];

static int ip_send_from(uint32_t src_ip, uint32_t dst_ip,
                        const uint8_t *dst_mac, uint8_t proto,
                        const uint8_t *payload, uint16_t payload_len)
{
    static uint16_t ip_id;

    if (payload_len > IP_PAYLOAD_MAX)
        return -1;

    uint32_t flags = irq_save();

    eth_header(tx_frame, dst_mac, ETH_P_IP);
    uint8_t *ip = tx_frame + ETH_HDR_LEN;

    memset(ip, 0, 20);
    ip[0] = 0x45;                    /* IPv4, a 20 B header */
    put16(ip + 2, (uint16_t)(20 + payload_len));
    put16(ip + 4, ++ip_id);
    ip[8] = 64;                      /* TTL */
    ip[9] = proto;
    put32(ip + 12, src_ip);
    put32(ip + 16, dst_ip);
    put16(ip + 10, checksum(ip, 20));

    memcpy(ip + 20, payload, payload_len);
    int ret = netdev_send(tx_frame, (uint16_t)(ETH_HDR_LEN + 20 + payload_len));

    irq_restore(flags);
    return ret;
}

static int ip_send(uint32_t dst_ip, const uint8_t *dst_mac, uint8_t proto,
                   const uint8_t *payload, uint16_t payload_len)
{
    return ip_send_from(my_ip, dst_ip, dst_mac, proto, payload, payload_len);
}

/* --- UDP ---------------------------------------------------------------- */

/* The UDP checksum is computed over a pseudo-header (IP addresses,
   protocol, length) glued onto the datagram itself - hence a separate
   function. */
static uint16_t udp_checksum(uint32_t src_ip, uint32_t dst_ip,
                             const uint8_t *udp, uint16_t len)
{
    uint32_t sum = 0;

    sum += (src_ip >> 16) & 0xFFFF;
    sum += src_ip & 0xFFFF;
    sum += (dst_ip >> 16) & 0xFFFF;
    sum += dst_ip & 0xFFFF;
    sum += IP_PROTO_UDP;
    sum += len;

    for (uint16_t i = 0; i + 1 < len; i += 2)
        sum += get16(udp + i);
    if (len & 1)
        sum += (uint32_t)udp[len - 1] << 8;

    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);

    uint16_t csum = (uint16_t)~sum;
    return csum ? csum : 0xFFFF;     /* 0 means "no checksum" */
}

/* Sends a UDP datagram with a given source address and destination
   MAC. Used directly by the DHCP client (which has neither an IP nor
   an ARP entry yet). */
int net_udp_send_raw(uint32_t src_ip, uint32_t dst_ip, const uint8_t *dst_mac,
                     uint16_t src_port, uint16_t dst_port,
                     const void *data, uint16_t len)
{
    static uint8_t udp[8 + UDP_DATA_MAX];

    if (len > UDP_DATA_MAX)
        return -1;

    uint32_t flags = irq_save();

    put16(udp + 0, src_port);
    put16(udp + 2, dst_port);
    put16(udp + 4, (uint16_t)(8 + len));
    put16(udp + 6, 0);
    memcpy(udp + 8, data, len);
    put16(udp + 6, udp_checksum(src_ip, dst_ip, udp, (uint16_t)(8 + len)));

    int ret = ip_send_from(src_ip, dst_ip, dst_mac, IP_PROTO_UDP, udp,
                           (uint16_t)(8 + len));
    irq_restore(flags);
    return ret;
}

static void udp_deliver(uint16_t port, uint32_t from_ip, uint16_t from_port,
                        const uint8_t *data, uint16_t len);

/* Releases sockets of processes that are no longer alive (like desktop
   windows) */
static void udp_reap(void)
{
    for (int i = 0; i < UDP_SOCKS; i++)
        if (udp_socks[i].used && udp_socks[i].owner >= 0 &&
            !task_alive(udp_socks[i].owner))
            udp_socks[i].used = 0;
}

static int udp_port_taken(uint16_t port)
{
    for (int i = 0; i < UDP_SOCKS; i++)
        if (udp_socks[i].used && udp_socks[i].port == port)
            return 1;
    return 0;
}

int net_udp_open(uint16_t port, int owner)
{
    uint32_t flags = irq_save();
    udp_reap();

    if (port && udp_port_taken(port)) {
        irq_restore(flags);
        return -1;                   /* the port is already taken */
    }
    if (!port) {                     /* an ephemeral port */
        for (int tries = 0; tries < 1000; tries++) {
            if (udp_next_port < UDP_PORT_MIN)
                udp_next_port = UDP_PORT_MIN;
            if (!udp_port_taken(udp_next_port)) {
                port = udp_next_port++;
                break;
            }
            udp_next_port++;
        }
        if (!port) {
            irq_restore(flags);
            return -1;
        }
    }

    for (int i = 0; i < UDP_SOCKS; i++) {
        if (udp_socks[i].used)
            continue;
        udp_socks[i].used = 1;
        udp_socks[i].owner = owner;
        udp_socks[i].port = port;
        udp_socks[i].head = 0;
        udp_socks[i].count = 0;
        irq_restore(flags);
        return i;
    }
    irq_restore(flags);
    return -1;                       /* no free sockets */
}

void net_udp_close(int sock, int owner)
{
    if (sock < 0 || sock >= UDP_SOCKS)
        return;
    uint32_t flags = irq_save();
    if (udp_socks[sock].used && udp_socks[sock].owner == owner)
        udp_socks[sock].used = 0;
    irq_restore(flags);
}

uint16_t net_udp_port(int sock)
{
    if (sock < 0 || sock >= UDP_SOCKS || !udp_socks[sock].used)
        return 0;
    return udp_socks[sock].port;
}

int net_udp_send(int sock, int owner, uint32_t ip, uint16_t port,
                 const void *data, uint16_t len)
{
    if (sock < 0 || sock >= UDP_SOCKS || !udp_socks[sock].used ||
        udp_socks[sock].owner != owner)
        return -1;
    if (!my_ip)
        return -1;                   /* the system has no address yet */

    /* Loopback: a datagram to our own address (or to 127.x) comes back
       straight into the socket queues, without going out on the wire.
       Thanks to that two processes can talk over UDP even without a
       network. */
    if (ip == my_ip || (ip >> 24) == 127) {
        if (len > UDP_DATA_MAX)
            return -1;
        uint32_t flags = irq_save();
        udp_deliver(port, my_ip, udp_socks[sock].port, data, len);
        irq_restore(flags);
        return len;
    }

    const uint8_t *mac = arp_resolve(ip);
    if (!mac)
        return -1;

    return net_udp_send_raw(my_ip, ip, mac, udp_socks[sock].port, port,
                            data, len);
}

/* Receiving from a socket: waits up to timeout_ms for a datagram.
   Called from a thread (task_sleep), never from an interrupt. Returns
   the length or -1. */
int net_udp_recv(int sock, int owner, uint32_t *from_ip, uint16_t *from_port,
                 void *buf, uint16_t max, uint32_t timeout_ms)
{
    if (sock < 0 || sock >= UDP_SOCKS || !udp_socks[sock].used ||
        udp_socks[sock].owner != owner)
        return -1;

    struct udp_sock *s = &udp_socks[sock];
    uint32_t waited = 0;

    for (;;) {
        uint32_t flags = irq_save();
        if (s->count) {
            struct udp_dgram *d = &s->q[s->head];
            uint16_t n = d->len < max ? d->len : max;
            memcpy(buf, d->data, n);
            if (from_ip)
                *from_ip = d->from_ip;
            if (from_port)
                *from_port = d->from_port;
            s->head = (s->head + 1) % UDP_QUEUE;
            s->count--;
            irq_restore(flags);
            return n;
        }
        irq_restore(flags);

        if (waited >= timeout_ms)
            return -1;
        task_sleep(10);
        waited += 10;
        if (!s->used || s->owner != owner)
            return -1;               /* the socket was closed in the meantime */
        if (task_killed())
            return -1;               /* the process is due to die - do not wait to the end */
    }
}

/* Puts a received datagram into a socket's queue (interrupt context) */
static void udp_deliver(uint16_t port, uint32_t from_ip, uint16_t from_port,
                        const uint8_t *data, uint16_t len)
{
    for (int i = 0; i < UDP_SOCKS; i++) {
        struct udp_sock *s = &udp_socks[i];
        if (!s->used || s->port != port)
            continue;
        if (s->count == UDP_QUEUE) {
            udp_dropped++;           /* the receiver cannot keep up */
            return;
        }
        struct udp_dgram *d = &s->q[(s->head + s->count) % UDP_QUEUE];
        d->from_ip = from_ip;
        d->from_port = from_port;
        d->len = len > UDP_DATA_MAX ? UDP_DATA_MAX : len;
        memcpy(d->data, data, d->len);
        s->count++;
        return;
    }
    udp_dropped++;                   /* nobody is listening on that port */
}

static void handle_udp(uint32_t src_ip, const uint8_t *udp, uint16_t len)
{
    if (len < 8)
        return;
    uint16_t src_port = get16(udp + 0);
    uint16_t dst_port = get16(udp + 2);
    uint16_t ulen = get16(udp + 4);
    if (ulen < 8 || ulen > len)
        return;
    udp_deliver(dst_port, src_ip, src_port, udp + 8, (uint16_t)(ulen - 8));
}

/* --- services for the TCP layer ----------------------------------------- */

/* A checksum with a pseudo-header - the same for UDP and TCP */
uint16_t net_transport_checksum(uint32_t src_ip, uint32_t dst_ip,
                                uint8_t proto, const uint8_t *seg,
                                uint16_t len)
{
    uint32_t sum = 0;

    sum += (src_ip >> 16) & 0xFFFF;
    sum += src_ip & 0xFFFF;
    sum += (dst_ip >> 16) & 0xFFFF;
    sum += dst_ip & 0xFFFF;
    sum += proto;
    sum += len;

    for (uint16_t i = 0; i + 1 < len; i += 2)
        sum += get16(seg + i);
    if (len & 1)
        sum += (uint32_t)seg[len - 1] << 8;

    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

/* Sends a ready segment to an already known destination MAC. TCP
   acknowledges segments from the card's interrupt, and ARP resolution
   blocks - that is why the MAC is remembered in the connection when it
   is set up. */
int net_ip_send_mac(uint32_t dst_ip, const uint8_t *dst_mac, uint8_t proto,
                    const void *payload, uint16_t len)
{
    return ip_send_from(my_ip, dst_ip, dst_mac, proto, payload, len);
}

/* Resolves the MAC for an address (through the gateway when the
   address is off-network). Blocks - from a thread context only. Copies
   6 bytes into out. */
int net_resolve_mac(uint32_t ip, uint8_t *out)
{
    const uint8_t *mac = arp_resolve(ip);
    if (!mac)
        return -1;
    memcpy(out, mac, 6);
    return 0;
}

/* --- receiving ---------------------------------------------------------- */

static void handle_arp(const uint8_t *a, uint16_t len)
{
    if (len < 28)
        return;
    if (get16(a + 0) != 1 || get16(a + 2) != ETH_P_IP)
        return;

    uint16_t op = get16(a + 6);
    uint32_t sender_ip = get32(a + 14);
    uint32_t target_ip = get32(a + 24);

    arp_store(sender_ip, a + 8);     /* the sender introduced itself */

    if (op == ARP_REQUEST && target_ip == my_ip)
        arp_send(ARP_REPLY, sender_ip, a + 8);
}

static void handle_icmp(uint32_t src_ip, const uint8_t *src_mac, uint8_t ttl,
                        const uint8_t *icmp, uint16_t len)
{
    if (len < 8)
        return;

    if (icmp[0] == ICMP_ECHO_REQUEST) {
        /* Somebody is pinging ProtonOS - we bounce the packet back as an echo reply */
        uint8_t reply[64];
        uint16_t n = len > sizeof(reply) ? (uint16_t)sizeof(reply) : len;
        memcpy(reply, icmp, n);
        reply[0] = ICMP_ECHO_REPLY;
        reply[1] = 0;
        put16(reply + 2, 0);
        put16(reply + 2, checksum(reply, n));
        arp_store(src_ip, src_mac);
        ip_send(src_ip, src_mac, IP_PROTO_ICMP, reply, n);
        return;
    }

    if (icmp[0] == ICMP_ECHO_REPLY && ping_waiting) {
        if (get16(icmp + 4) == ping_id && get16(icmp + 6) == ping_seq) {
            ping_reply_tick = pit_ticks();
            ping_ttl = ttl;
            ping_got = 1;
        }
    }
}

static void handle_ip(const uint8_t *ip, uint16_t len, const uint8_t *src_mac)
{
    if (len < 20 || (ip[0] >> 4) != 4)
        return;

    uint16_t ihl = (uint16_t)((ip[0] & 0x0F) * 4);
    uint16_t total = get16(ip + 2);
    if (ihl < 20 || total < ihl || total > len)
        return;

    uint32_t dst_ip = get32(ip + 16);
    if (dst_ip != my_ip && dst_ip != 0xFFFFFFFFu)
        return;                      /* not for us */

    if (ip[9] == IP_PROTO_ICMP)
        handle_icmp(get32(ip + 12), src_mac, ip[8], ip + ihl,
                    (uint16_t)(total - ihl));
    else if (ip[9] == IP_PROTO_UDP)
        handle_udp(get32(ip + 12), ip + ihl, (uint16_t)(total - ihl));
    else if (ip[9] == IP_PROTO_TCP)
        tcp_input(get32(ip + 12), dst_ip, src_mac, ip + ihl,
                  (uint16_t)(total - ihl));
}

void net_rx(const uint8_t *frame, uint16_t len)
{
    if (len < ETH_HDR_LEN) {
        rx_dropped++;
        return;
    }

    uint16_t type = get16(frame + 12);
    const uint8_t *payload = frame + ETH_HDR_LEN;
    uint16_t payload_len = (uint16_t)(len - ETH_HDR_LEN);

    if (type == ETH_P_ARP)
        handle_arp(payload, payload_len);
    else if (type == ETH_P_IP)
        handle_ip(payload, payload_len, frame + 6);
    else
        rx_dropped++;
}

/* --- ping --------------------------------------------------------------- */

/* Resolves the MAC address for an IP (through the gateway when the
   address is off-network). Returns a pointer to 6 MAC bytes or 0 when
   there was no answer. */
static const uint8_t *arp_resolve(uint32_t ip)
{
    uint32_t target = ((ip & my_mask) == (my_ip & my_mask)) ? ip : my_gw;

    const uint8_t *mac = arp_lookup(target);
    if (mac)
        return mac;

    for (int try = 0; try < 4; try++) {
        arp_send(ARP_REQUEST, target, bcast_mac);
        for (int i = 0; i < 25; i++) {
            task_sleep(10);
            mac = arp_lookup(target);
            if (mac)
                return mac;
            if (task_killed())
                return 0;
        }
    }
    return 0;
}

void net_ping(uint32_t ip, int count)
{
    if (!netdev_present()) {
        printk("ping: there is no network card\n");
        return;
    }
    if (!my_ip) {
        printk("ping: there is no IP address (try 'dhcp')\n");
        return;
    }

    const uint8_t *mac = arp_resolve(ip);
    if (!mac) {
        printk("ping: no ARP answer for ");
        net_print_ip(ip);
        printk("\n");
        return;
    }

    printk("PING ");
    net_print_ip(ip);
    printk(" (%u B of data)\n", 32);

    int received = 0;
    for (int seq = 1; seq <= count; seq++) {
        uint8_t icmp[40];
        memset(icmp, 0, sizeof(icmp));
        icmp[0] = ICMP_ECHO_REQUEST;
        put16(icmp + 4, 0x1234);
        put16(icmp + 6, (uint16_t)seq);
        for (int i = 0; i < 32; i++)
            icmp[8 + i] = (uint8_t)('a' + (i % 26));
        put16(icmp + 2, checksum(icmp, 40));

        ping_id = 0x1234;
        ping_seq = (uint16_t)seq;
        ping_got = 0;
        ping_waiting = 1;

        uint32_t sent_tick = pit_ticks();
        if (ip_send(ip, mac, IP_PROTO_ICMP, icmp, 40) < 0) {
            printk("ping: could not send the packet\n");
            ping_waiting = 0;
            return;
        }

        for (int i = 0; i < 100 && !ping_got; i++) { /* up to 1 s */
            task_sleep(10);
            if (task_killed()) {      /* 'kill ping' is to work at once */
                ping_waiting = 0;
                printk("ping: interrupted\n");
                return;
            }
        }
        ping_waiting = 0;

        if (ping_got) {
            uint32_t ms = (ping_reply_tick - sent_tick) * 10;
            received++;
            printk("40 B od ");
            net_print_ip(ip);
            printk(": seq=%u ttl=%u time=%ums\n", (uint32_t)seq,
                   (uint32_t)ping_ttl, ms);
        } else {
            printk("no answer: seq=%u (1 s timeout)\n", (uint32_t)seq);
        }

        if (seq < count)
            task_sleep(500);
    }

    printk("--- statistics: sent %u, received %u ---\n",
           (uint32_t)count, (uint32_t)received);
}

/* --- init and ifconfig --------------------------------------------------- */

void net_ifconfig(void)
{
    if (!netdev_present()) {
        printk("there is no network card (QEMU: -device e1000 or rtl8139)\n");
        return;
    }
    const uint8_t *m = netdev_mac();
    printk("eth0  %s, MAC %x:%x:%x:%x:%x:%x\n", netdev_name(),
           m[0], m[1], m[2], m[3], m[4], m[5]);
    if (!my_ip) {
        printk("      no IP address - DHCP is still configuring\n");
        return;
    }
    printk("      IP ");
    net_print_ip(my_ip);
    printk("  netmask ");
    net_print_ip(my_mask);
    printk("  gateway ");
    net_print_ip(my_gw);
    printk("  DNS ");
    net_print_ip(my_dns);
    printk("\n      RX %u packets, TX %u packets, dropped %u+%u\n",
           netdev_rx_packets(), netdev_tx_packets(), rx_dropped,
           udp_dropped);
}

int net_init(void)
{
    if (!netdev_init())
        return 0;
    return 1;
}
