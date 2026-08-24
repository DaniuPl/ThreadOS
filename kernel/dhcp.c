/* Klient DHCP
 *
 * Zamiast wpisanego na sztywno adresu system prosi o niego siec:
 *   DISCOVER (broadcast) -> OFFER -> REQUEST -> ACK
 * Z odpowiedzi bierzemy adres (yiaddr) oraz opcje: maske (1), brame (3)
 * i serwer DNS (6). Bez tego ProtonOS dzialalby tylko w sieci
 * uzytkownika QEMU, gdzie adresy sa z gory znane.
 *
 * DHCP chodzi po UDP (68 -> 67), wiec korzysta ze zwyklych gniazd stosu.
 * Dopoki nie mamy adresu, pakiety musza isc rozglosnie i z adresem
 * zrodlowym 0.0.0.0 - stad net_udp_send_raw() zamiast net_udp_send().
 */

#include "include/dhcp.h"
#include "include/net.h"
#include "include/rtl8139.h"
#include "include/task.h"
#include "include/pit.h"
#include "include/printk.h"
#include "include/string.h"
#include "include/shell.h"

#define DHCP_CLIENT_PORT 68
#define DHCP_SERVER_PORT 67

#define BOOTREQUEST 1
#define BOOTREPLY   2

#define DHCP_DISCOVER 1
#define DHCP_OFFER    2
#define DHCP_REQUEST  3
#define DHCP_ACK      5
#define DHCP_NAK      6

#define OPT_SUBNET    1
#define OPT_ROUTER    3
#define OPT_DNS       6
#define OPT_REQUESTED 50
#define OPT_MSGTYPE   53
#define OPT_SERVERID  54
#define OPT_PARAMLIST 55
#define OPT_END       255

#define BOOTP_FIXED   236            /* czesc stala pakietu (przed opcjami) */
#define PKT_MAX       548
#define MAGIC_COOKIE  0x63825363u

static const uint8_t bcast_mac[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static void put32be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t get32be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* Sklada czesc stala pakietu; zwraca offset pierwszej opcji */
static int bootp_header(uint8_t *pkt, uint32_t xid)
{
    memset(pkt, 0, PKT_MAX);
    pkt[0] = BOOTREQUEST;
    pkt[1] = 1;                      /* Ethernet */
    pkt[2] = 6;                      /* dlugosc adresu sprzetowego */
    put32be(pkt + 4, xid);
    pkt[10] = 0x80;                  /* flaga broadcast: odpowiedz rozglosnie
                                        (nie mamy jeszcze adresu, wiec nie
                                        odebralibysmy pakietu unicast) */
    memcpy(pkt + 28, rtl8139_mac(), 6);
    put32be(pkt + BOOTP_FIXED, MAGIC_COOKIE);
    return BOOTP_FIXED + 4;
}

/* Wyszukuje opcje w pakiecie; zwraca wskaznik na dane albo 0 */
static const uint8_t *find_option(const uint8_t *pkt, int len, uint8_t want,
                                  uint8_t *out_len)
{
    int i = BOOTP_FIXED + 4;
    while (i + 1 < len) {
        uint8_t code = pkt[i];
        if (code == OPT_END)
            break;
        if (code == 0) {             /* wypelniacz */
            i++;
            continue;
        }
        uint8_t olen = pkt[i + 1];
        if (i + 2 + olen > len)
            break;
        if (code == want) {
            if (out_len)
                *out_len = olen;
            return pkt + i + 2;
        }
        i += 2 + olen;
    }
    return 0;
}

static uint8_t option_msgtype(const uint8_t *pkt, int len)
{
    uint8_t olen;
    const uint8_t *p = find_option(pkt, len, OPT_MSGTYPE, &olen);
    return (p && olen == 1) ? *p : 0;
}

static uint32_t option_ip(const uint8_t *pkt, int len, uint8_t code)
{
    uint8_t olen;
    const uint8_t *p = find_option(pkt, len, code, &olen);
    return (p && olen >= 4) ? get32be(p) : 0;
}

/* Czeka na odpowiedz serwera o naszym xid i zadanym typie */
static int wait_reply(int sock, uint8_t *pkt, uint32_t xid, uint8_t want,
                      uint32_t timeout_ms)
{
    uint32_t waited = 0;
    while (waited < timeout_ms) {
        int n = net_udp_recv(sock, -1, 0, 0, pkt, PKT_MAX, 200);
        waited += 200;
        if (n < BOOTP_FIXED + 4)
            continue;
        if (pkt[0] != BOOTREPLY || get32be(pkt + 4) != xid)
            continue;
        uint8_t type = option_msgtype(pkt, n);
        if (type == want)
            return n;
        if (type == DHCP_NAK)
            return -1;
    }
    return -1;
}

int dhcp_configure(void)
{
    static uint8_t pkt[PKT_MAX];     /* .bss - na stos watku (8 KiB) za duzy */

    int sock = net_udp_open(DHCP_CLIENT_PORT, -1);
    if (sock < 0) {
        printk("[siec] DHCP: brak wolnego gniazda\n");
        return 0;
    }

    /* xid ma odrozniac nasze wymiany od cudzych - miesza MAC z zegarem */
    const uint8_t *mac = rtl8139_mac();
    uint32_t xid = (uint32_t)(mac[3] << 24 | mac[4] << 16 | mac[5] << 8) ^
                   (pit_ticks() * 2654435761u);

    for (int attempt = 0; attempt < 3; attempt++) {
        /* --- DISCOVER --- */
        int o = bootp_header(pkt, xid);
        pkt[o++] = OPT_MSGTYPE;
        pkt[o++] = 1;
        pkt[o++] = DHCP_DISCOVER;
        pkt[o++] = OPT_PARAMLIST;
        pkt[o++] = 3;
        pkt[o++] = OPT_SUBNET;
        pkt[o++] = OPT_ROUTER;
        pkt[o++] = OPT_DNS;
        pkt[o++] = OPT_END;

        net_udp_send_raw(0, 0xFFFFFFFFu, bcast_mac, DHCP_CLIENT_PORT,
                         DHCP_SERVER_PORT, pkt, (uint16_t)o);

        int n = wait_reply(sock, pkt, xid, DHCP_OFFER, 2000);
        if (n < 0)
            continue;                /* brak oferty - sprobuj jeszcze raz */

        uint32_t offered = get32be(pkt + 16);          /* yiaddr */
        uint32_t server = option_ip(pkt, n, OPT_SERVERID);

        /* --- REQUEST --- */
        o = bootp_header(pkt, xid);
        pkt[o++] = OPT_MSGTYPE;
        pkt[o++] = 1;
        pkt[o++] = DHCP_REQUEST;
        pkt[o++] = OPT_REQUESTED;
        pkt[o++] = 4;
        put32be(pkt + o, offered);
        o += 4;
        if (server) {
            pkt[o++] = OPT_SERVERID;
            pkt[o++] = 4;
            put32be(pkt + o, server);
            o += 4;
        }
        pkt[o++] = OPT_PARAMLIST;
        pkt[o++] = 3;
        pkt[o++] = OPT_SUBNET;
        pkt[o++] = OPT_ROUTER;
        pkt[o++] = OPT_DNS;
        pkt[o++] = OPT_END;

        net_udp_send_raw(0, 0xFFFFFFFFu, bcast_mac, DHCP_CLIENT_PORT,
                         DHCP_SERVER_PORT, pkt, (uint16_t)o);

        n = wait_reply(sock, pkt, xid, DHCP_ACK, 2000);
        if (n < 0)
            continue;

        uint32_t ip = get32be(pkt + 16);
        uint32_t mask = option_ip(pkt, n, OPT_SUBNET);
        uint32_t gw = option_ip(pkt, n, OPT_ROUTER);
        uint32_t dns = option_ip(pkt, n, OPT_DNS);
        if (!ip)
            continue;
        if (!mask)
            mask = IP4(255, 255, 255, 0);

        net_udp_close(sock, -1);
        net_set_config(ip, mask, gw, dns);

        printk("[siec] DHCP: przyznano ");
        net_print_ip(ip);
        printk(", brama ");
        net_print_ip(gw);
        printk(", DNS ");
        net_print_ip(dns);
        printk("\n");
        return 1;
    }

    net_udp_close(sock, -1);
    return 0;
}

static void dhcp_thread(void *arg)
{
    (void)arg;
    if (!dhcp_configure()) {
        net_use_fallback();
        printk("[siec] DHCP: brak odpowiedzi, uzywam adresu awaryjnego ");
        net_print_ip(net_ip());
        printk("\n");
    }
    /* Komunikat wpadl w linie promptu - odrysuj zachete shella */
    shell_refresh();
}

void dhcp_start(void)
{
    task_create("dhcp", dhcp_thread, 0);
}
