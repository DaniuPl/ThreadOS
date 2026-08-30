/* unet64 - siec od strony programu
 *
 * Do tej pory z sieci korzystalo tylko jadro: 'ping' i 'ifconfig' to
 * jego wlasne polecenia. Ten program robi to sam, przez wywolania
 * systemowe, i nie ma przy tym zadnych szczegolnych uprawnien.
 *
 * Sprawdza trzy rzeczy po kolei:
 *   - czy zna swoj adres (ten z DHCP),
 *   - czy datagram wyslany do siebie samego wraca taki sam,
 *   - czy da sie polaczyc z serwerem i dostac od niego odpowiedz.
 *
 * Adres i port serwera podaje sie w argumentach; bez nich robi tylko
 * dwie pierwsze proby.
 */

#include "usys64.h"

#define MY_PORT 7777

static char out[192];
static char buf[512];

static int put(int n, const char *s)
{
    while (*s)
        out[n++] = *s++;
    return n;
}

static int putn(int n, uint64_t v)
{
    char tmp[24];
    utoa(v, tmp);
    return put(n, tmp);
}

static int put_ip(int n, uint32_t ip)
{
    for (int i = 3; i >= 0; i--) {
        n = putn(n, (ip >> (i * 8)) & 0xFF);
        if (i)
            out[n++] = '.';
    }
    return n;
}

static void err(const char *co, int64_t v)
{
    int n = put(0, "[unet64] BLAD: ");
    n = put(n, co);
    n = put(n, " -> ");
    n = putn(n, (uint64_t)v);
    out[n++] = '\n';
    out[n] = 0;
    print(out);
    sys_exit(1);
}

void _start(int argc, char **argv)
{
    uint32_t addrs[4];

    /* 1. Wlasny adres - ten, ktory przyszedl z sieci przez DHCP */
    if (sys_netinfo(addrs) != 1)
        err("system nie ma adresu IP", 0);

    int n = put(0, "[unet64] moj adres ");
    n = put_ip(n, addrs[0]);
    n = put(n, ", brama ");
    n = put_ip(n, addrs[2]);
    n = put(n, "\n");
    out[n] = 0;
    print(out);

    /* 2. Datagram do samego siebie. Petla zwrotna nie wychodzi na
          kabel, wiec proba nie zalezy od zadnej sieci na zewnatrz. */
    int64_t s = sys_udp_open(MY_PORT);
    if (s < 0)
        err("nie moge otworzyc gniazda UDP", s);

    static const char body[] = "datagram z ring 3";
    struct udp_info o;
    o.sock = (int)s;
    o.ip = addrs[0];
    o.port = MY_PORT;
    o.timeout = 3000;

    if (sys_udp_send(&o, body, ustrlen(body)) < 0)
        err("wysylka datagramu nie przeszla", -1);

    o.sock = (int)s;
    o.timeout = 3000;
    int64_t got = sys_udp_recv(&o, buf, sizeof(buf));
    if (got <= 0)
        err("datagram nie wrocil", got);

    if ((uint64_t)got != ustrlen(body))
        err("wrocil datagram innej dlugosci", got);
    for (int64_t i = 0; i < got; i++)
        if (buf[i] != body[i])
            err("tresc datagramu sie nie zgadza", i);

    n = put(0, "[unet64] datagram wrocil do mnie (");
    n = putn(n, (uint64_t)got);
    n = put(n, " B) od ");
    n = put_ip(n, o.ip);
    n = put(n, "\n");
    out[n] = 0;
    print(out);
    sys_udp_close((int)s);

    if (argc < 3) {
        print("[unet64] bez adresu serwera koncze na UDP\n");
        sys_exit(0);
    }

    /* 3. Polaczenie TCP i prosba HTTP - najkrotsza droga do
          sprawdzenia, czy strumien dziala w obie strony */
    int64_t ip = uparse_ip(argv[1]);
    int64_t port = uatoi(argv[2]);
    if (ip < 0 || port <= 0)
        err("zly adres albo port serwera", 0);

    o.ip = (uint32_t)ip;
    o.port = (uint16_t)port;
    o.timeout = 5000;
    int64_t c = sys_tcp_connect(&o);
    if (c < 0)
        err("nie moge sie polaczyc", c);

    static const char task[] = "GET / HTTP/1.0\r\n\r\n";
    if (sys_tcp_send((int)c, task, ustrlen(task)) < 0)
        err("wysylka zadania nie przeszla", -1);

    int64_t total = 0;
    for (;;) {
        int64_t k = sys_tcp_recv((int)c, buf, sizeof(buf) - 1);
        if (k <= 0)
            break;
        total += k;
        if (total > 16)
            break;                   /* naglowek wystarczy za dowod */
    }
    sys_tcp_close((int)c);

    if (total <= 0)
        err("serwer nic nie odpowiedzial", total);

    buf[16] = 0;
    n = put(0, "[unet64] serwer odpowiedzial (");
    n = putn(n, (uint64_t)total);
    n = put(n, " B): ");
    n = put(n, buf);
    n = put(n, "\n");
    out[n] = 0;
    print(out);

    sys_exit(0);
}
