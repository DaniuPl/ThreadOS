/* nslookup - zapytanie DNS z ring 3 (UDP, port 53)
 *
 * Cala rozmowa dzieje sie w procesie uzytkownika: jadro daje tylko
 * gniazdo. Serwer DNS bierzemy z konfiguracji przyznanej przez DHCP.
 *
 * Uzycie: run nslookup example.com
 */

#include "usys.h"

#define DNS_PORT   53
#define QUERY_MAX  300
#define REPLY_MAX  512

/* Zamienia "example.com" na format DNS: 7example3com0 */
static int encode_name(uint8_t *out, const char *name)
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
        if (o > QUERY_MAX - 8)
            return -1;
    }
    out[o++] = 0;                       /* korzen */
    return o;
}

/* Przeskakuje nazwe w odpowiedzi: etykiety albo wskaznik kompresji */
static int skip_name(const uint8_t *pkt, int len, int off)
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

void _start(int argc, char **argv)
{
    if (argc < 2) {
        print("uzycie: nslookup <nazwa>\n");
        sys_exit(1);
    }

    struct netinfo ni;
    if (netinfo(&ni) < 0) {
        print("[nslookup] system nie ma jeszcze adresu IP\n");
        sys_exit(1);
    }
    if (!ni.dns) {
        print("[nslookup] DHCP nie podal serwera DNS\n");
        sys_exit(1);
    }

    static uint8_t q[QUERY_MAX];
    /* Naglowek: id, flagi 0x0100 (zapytanie z rekursja), 1 pytanie */
    q[0] = 0x50; q[1] = 0x53;           /* id "PS" - odpowiedz musi je oddac */
    q[2] = 0x01; q[3] = 0x00;
    q[4] = 0x00; q[5] = 0x01;
    for (int i = 6; i < 12; i++)
        q[i] = 0;

    int n = encode_name(q + 12, argv[1]);
    if (n < 0) {
        print("[nslookup] zla nazwa\n");
        sys_exit(1);
    }
    int qlen = 12 + n;
    q[qlen++] = 0; q[qlen++] = 1;       /* QTYPE = A */
    q[qlen++] = 0; q[qlen++] = 1;       /* QCLASS = IN */

    int32_t sock = udp_open(0);
    if (sock < 0) {
        print("[nslookup] brak wolnego gniazda\n");
        sys_exit(1);
    }

    print("[nslookup] pytam ");
    print_ip(ni.dns);
    print(" o ");
    print(argv[1]);
    print("\n");

    if (udp_send(sock, ni.dns, DNS_PORT, q, (uint32_t)qlen) < 0) {
        print("[nslookup] nie udalo sie wyslac zapytania\n");
        udp_close(sock);
        sys_exit(1);
    }

    static uint8_t r[REPLY_MAX];
    int32_t rn = udp_recv(sock, r, sizeof(r), 5000, 0, 0);
    udp_close(sock);

    if (rn < 12) {
        print("[nslookup] brak odpowiedzi (timeout 5 s)\n");
        sys_exit(1);
    }
    if (r[0] != q[0] || r[1] != q[1]) {
        print("[nslookup] odpowiedz z obcym identyfikatorem\n");
        sys_exit(1);
    }

    int rcode = r[3] & 0x0F;
    if (rcode == 3) {
        print("[nslookup] nazwa nie istnieje\n");
        sys_exit(2);
    }
    if (rcode) {
        print("[nslookup] serwer zwrocil blad ");
        print_dec((uint32_t)rcode);
        print("\n");
        sys_exit(2);
    }

    int qd = (r[4] << 8) | r[5];
    int an = (r[6] << 8) | r[7];
    int off = 12;

    for (int i = 0; i < qd; i++) {      /* pomin powtorzone pytania */
        off = skip_name(r, rn, off);
        if (off < 0) {
            print("[nslookup] uszkodzona odpowiedz\n");
            sys_exit(1);
        }
        off += 4;                       /* QTYPE + QCLASS */
    }

    int found = 0;
    for (int i = 0; i < an && off + 10 <= rn; i++) {
        off = skip_name(r, rn, off);
        if (off < 0 || off + 10 > rn)
            break;
        int type = (r[off] << 8) | r[off + 1];
        int rdlen = (r[off + 8] << 8) | r[off + 9];
        off += 10;
        if (off + rdlen > rn)
            break;

        if (type == 1 && rdlen == 4) {  /* rekord A */
            uint32_t ip = ((uint32_t)r[off] << 24) |
                          ((uint32_t)r[off + 1] << 16) |
                          ((uint32_t)r[off + 2] << 8) | r[off + 3];
            print(argv[1]);
            print(" ma adres ");
            print_ip(ip);
            print("\n");
            found++;
        }
        off += rdlen;
    }

    if (!found) {
        print("[nslookup] brak rekordu A dla tej nazwy\n");
        sys_exit(2);
    }
    sys_exit(0);
}
