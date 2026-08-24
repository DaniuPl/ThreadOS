/* Klient DNS dla programow ring 3 (zapytanie A po UDP)
 *
 * Wspoldzielony przez 'nslookup' i przegladarke 'gweb': jedno miejsce,
 * w ktorym sklada sie zapytanie i rozbiera odpowiedz.
 */

#ifndef PROTONOS_UDNS_H
#define PROTONOS_UDNS_H

#include "usys.h"

#define DNS_PORT      53
#define DNS_QUERY_MAX 300
#define DNS_REPLY_MAX 512

/* Zamienia "example.com" na format DNS: 7example3com0.
   Zwraca dlugosc albo -1. */
static inline int dns_encode_name(uint8_t *out, const char *name)
{
    int o = 0, start = 0, i = 0;

    for (;; i++) {
        if (name[i] == '.' || name[i] == '\0') {
            int len = i - start;
            if (len <= 0 || len > 63)
                return -1;
            out[o++] = (uint8_t)len;
            for (int k = 0; k < len; k++)
                out[o++] = (uint8_t)name[start + k];
            start = i + 1;
            if (name[i] == '\0')
                break;
        }
        if (o > DNS_QUERY_MAX - 8)
            return -1;
    }
    out[o++] = 0;                       /* korzen */
    return o;
}

/* Przeskakuje nazwe: etykiety albo wskaznik kompresji. -1 przy smieciu. */
static inline int dns_skip_name(const uint8_t *pkt, int len, int off)
{
    while (off < len) {
        uint8_t n = pkt[off];
        if ((n & 0xC0) == 0xC0)
            return off + 2;             /* wskaznik konczy nazwe */
        off++;
        if (!n)
            return off;                 /* korzen konczy nazwe */
        off += n;
    }
    return -1;
}

/* Kody wyniku dla dns_lookup (obok adresu >0) */
#define DNS_ERR_SOCKET   0
#define DNS_ERR_TIMEOUT  0

/* Odpytuje serwer dns_ip o rekord A nazwy. Zwraca adres w kolejnosci
   hosta albo 0. Gdy rcode != 0, wpisuje go do *rcode_out (jesli podano). */
static inline uint32_t dns_lookup(const char *name, uint32_t dns_ip,
                                  int *rcode_out)
{
    static uint8_t q[DNS_QUERY_MAX];
    static uint8_t r[DNS_REPLY_MAX];

    if (rcode_out)
        *rcode_out = 0;
    if (!dns_ip)
        return 0;

    q[0] = 0x50; q[1] = 0x53;           /* identyfikator "PS" */
    q[2] = 0x01; q[3] = 0x00;           /* zapytanie z rekursja */
    q[4] = 0x00; q[5] = 0x01;           /* jedno pytanie */
    for (int i = 6; i < 12; i++)
        q[i] = 0;

    int n = dns_encode_name(q + 12, name);
    if (n < 0)
        return 0;
    int qlen = 12 + n;
    q[qlen++] = 0; q[qlen++] = 1;       /* QTYPE = A */
    q[qlen++] = 0; q[qlen++] = 1;       /* QCLASS = IN */

    int32_t sock = udp_open(0);
    if (sock < 0)
        return 0;

    if (udp_send(sock, dns_ip, DNS_PORT, q, (uint32_t)qlen) < 0) {
        udp_close(sock);
        return 0;
    }

    int32_t rn = udp_recv(sock, r, DNS_REPLY_MAX, 5000, 0, 0);
    udp_close(sock);

    if (rn < 12 || r[0] != q[0] || r[1] != q[1])
        return 0;

    int rcode = r[3] & 0x0F;
    if (rcode) {
        if (rcode_out)
            *rcode_out = rcode;
        return 0;
    }

    int qd = (r[4] << 8) | r[5];
    int an = (r[6] << 8) | r[7];
    int off = 12;

    for (int i = 0; i < qd; i++) {      /* pomin powtorzone pytania */
        off = dns_skip_name(r, rn, off);
        if (off < 0)
            return 0;
        off += 4;                       /* QTYPE + QCLASS */
    }

    for (int i = 0; i < an && off + 10 <= rn; i++) {
        off = dns_skip_name(r, rn, off);
        if (off < 0 || off + 10 > rn)
            break;
        int type = (r[off] << 8) | r[off + 1];
        int rdlen = (r[off + 8] << 8) | r[off + 9];
        off += 10;
        if (off + rdlen > rn)
            break;
        if (type == 1 && rdlen == 4)    /* rekord A */
            return ((uint32_t)r[off] << 24) | ((uint32_t)r[off + 1] << 16) |
                   ((uint32_t)r[off + 2] << 8) | r[off + 3];
        off += rdlen;
    }
    return 0;
}

#endif
