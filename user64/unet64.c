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

#define MOJ_PORT 7777

static char out[192];
static char bufor[512];

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

static void blad(const char *co, int64_t v)
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
    uint32_t adresy[4];

    /* 1. Wlasny adres - ten, ktory przyszedl z sieci przez DHCP */
    if (sys_netinfo(adresy) != 1)
        blad("system nie ma adresu IP", 0);

    int n = put(0, "[unet64] moj adres ");
    n = put_ip(n, adresy[0]);
    n = put(n, ", brama ");
    n = put_ip(n, adresy[2]);
    n = put(n, "\n");
    out[n] = 0;
    print(out);

    /* 2. Datagram do samego siebie. Petla zwrotna nie wychodzi na
          kabel, wiec proba nie zalezy od zadnej sieci na zewnatrz. */
    int64_t s = sys_udp_open(MOJ_PORT);
    if (s < 0)
        blad("nie moge otworzyc gniazda UDP", s);

    static const char tresc[] = "datagram z ring 3";
    struct udp_opis o;
    o.sock = (int)s;
    o.ip = adresy[0];
    o.port = MOJ_PORT;
    o.timeout = 3000;

    if (sys_udp_send(&o, tresc, ustrlen(tresc)) < 0)
        blad("wysylka datagramu nie przeszla", -1);

    o.sock = (int)s;
    o.timeout = 3000;
    int64_t got = sys_udp_recv(&o, bufor, sizeof(bufor));
    if (got <= 0)
        blad("datagram nie wrocil", got);

    if ((uint64_t)got != ustrlen(tresc))
        blad("wrocil datagram innej dlugosci", got);
    for (int64_t i = 0; i < got; i++)
        if (bufor[i] != tresc[i])
            blad("tresc datagramu sie nie zgadza", i);

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
        blad("zly adres albo port serwera", 0);

    o.ip = (uint32_t)ip;
    o.port = (uint16_t)port;
    o.timeout = 5000;
    int64_t c = sys_tcp_connect(&o);
    if (c < 0)
        blad("nie moge sie polaczyc", c);

    static const char zadanie[] = "GET / HTTP/1.0\r\n\r\n";
    if (sys_tcp_send((int)c, zadanie, ustrlen(zadanie)) < 0)
        blad("wysylka zadania nie przeszla", -1);

    int64_t razem = 0;
    for (;;) {
        int64_t k = sys_tcp_recv((int)c, bufor, sizeof(bufor) - 1);
        if (k <= 0)
            break;
        razem += k;
        if (razem > 16)
            break;                   /* naglowek wystarczy za dowod */
    }
    sys_tcp_close((int)c);

    if (razem <= 0)
        blad("serwer nic nie odpowiedzial", razem);

    bufor[16] = 0;
    n = put(0, "[unet64] serwer odpowiedzial (");
    n = putn(n, (uint64_t)razem);
    n = put(n, " B): ");
    n = put(n, bufor);
    n = put(n, "\n");
    out[n] = 0;
    print(out);

    sys_exit(0);
}
