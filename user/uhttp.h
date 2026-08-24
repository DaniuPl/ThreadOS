/* Klient HTTP/1.1 dla programow ring 3
 *
 * Wspoldzielony przez 'httpget' i przegladarke 'gweb'. Obsluguje to,
 * czego wymagaja prawdziwe serwery:
 *   - HTTP/1.1 z 'Connection: close' (koniec tresci po FIN),
 *   - kodowanie 'chunked' (serwery uzywaja go, gdy nie znaja z gory
 *     dlugosci odpowiedzi) sklejane przez granice pakietow,
 *   - naglowek Location, zeby dalo sie isc za przekierowaniem,
 *   - rozbior adresu "http://host[:port]/sciezka" oraz https://,
 *   - polaczenie po TLS 1.3, gdy adres jest https (patrz utls.h).
 *
 * Tresc idzie strumieniem do funkcji zwrotnej - dzieki temu program
 * moze ja od razu zapisywac na dysk albo skladac do okna, bez trzymania
 * calej strony w pamieci.
 */

#ifndef PROTONOS_UHTTP_H
#define PROTONOS_UHTTP_H

#include "usys.h"
#include "udns.h"
#include "utls.h"

#define HTTP_HDR_MAX  2048
#define HTTP_CHUNK    1024

/* Jedno miejsce, przez ktore ida bajty - zwykly TCP albo TLS */
struct http_transport {
    int tls;
    int32_t sock;
    struct tls_conn conn;
};

static inline int http_tp_send(struct http_transport *tp, const void *data,
                               uint32_t len)
{
    if (tp->tls)
        return tls_send(&tp->conn, data, len);

    const uint8_t *p = data;
    uint32_t done = 0;
    while (done < len) {
        uint32_t take = len - done;
        if (take > 1024)
            take = 1024;
        if (tcp_send(tp->sock, p + done, take, 10000) < 0)
            return -1;
        done += take;
    }
    return 0;
}

static inline int http_tp_recv(struct http_transport *tp, void *buf,
                               uint32_t max)
{
    if (tp->tls)
        return tls_recv(&tp->conn, buf, max);
    return tcp_recv(tp->sock, buf, max, 10000);
}

static inline void http_tp_close(struct http_transport *tp)
{
    if (tp->tls)
        tls_close(&tp->conn);
    else if (tp->sock >= 0)
        tcp_close(tp->sock);
}

struct http_result {
    int status;                  /* kod odpowiedzi albo 0 */
    uint32_t body_len;           /* odebranych bajtow tresci */
    int chunked;                 /* czy tresc byla w kawalkach */
    char location[192];          /* naglowek Location (przekierowanie) */
};

/* Wolane dla kolejnych kawalkow tresci */
typedef void (*http_sink)(const char *data, uint32_t len, void *arg);

static inline int uhttp_lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

/* Czy linia naglowka zaczyna sie od danej nazwy (bez rozroznienia
   wielkosci liter)? Zwraca wskaznik na wartosc albo 0. */
static inline const char *uhttp_header(const char *line, const char *name)
{
    int i = 0;
    while (name[i]) {
        if (uhttp_lower(line[i]) != uhttp_lower(name[i]))
            return 0;
        i++;
    }
    if (line[i] != ':')
        return 0;
    i++;
    while (line[i] == ' ')
        i++;
    return line + i;
}

/* Rozbior "http://host[:port]/sciezka", "https://..." albo samego
   "host[:port]/sciezka". W *tls wraca 1 dla https (i port 443). */
static inline int http_parse_url(const char *url, char *host, int hmax,
                                 uint32_t *port, char *path, int pmax,
                                 int *tls)
{
    int i = 0, secure = 0;

    if (url[0] == 'h' && url[1] == 't' && url[2] == 't' && url[3] == 'p') {
        int j = 4;
        if (url[j] == 's') {
            secure = 1;
            j++;
        }
        if (url[j] == ':' && url[j + 1] == '/' && url[j + 2] == '/')
            i = j + 3;
        else
            secure = 0;           /* to nie byl schemat, tylko nazwa hosta */
    }
    if (tls)
        *tls = secure;

    int h = 0;
    while (url[i] && url[i] != ':' && url[i] != '/' && h < hmax - 1)
        host[h++] = url[i++];
    host[h] = '\0';
    if (!h)
        return 0;

    *port = secure ? 443 : 80;
    if (url[i] == ':') {
        i++;
        uint32_t p = 0;
        while (url[i] >= '0' && url[i] <= '9')
            p = p * 10 + (uint32_t)(url[i++] - '0');
        *port = p;
    }

    int t = 0;
    while (url[i] && t < pmax - 1)
        path[t++] = url[i++];
    if (!t)
        path[t++] = '/';
    path[t] = '\0';
    return *port ? 1 : 0;
}

/* Stosuje naglowek Location: adres bezwzgledny zmienia host i port,
   adres zaczynajacy sie od "/" tylko sciezke (tak wysyla przekierowania
   wiekszosc serwerow). Zwraca 1, gdy udalo sie ustalic nowy cel. */
static inline int http_apply_location(const char *loc, char *host, int hmax,
                                      uint32_t *port, char *path, int pmax,
                                      int *tls)
{
    for (int i = 0; loc[i] && i < 8; i++) {
        if (loc[i] == ':' && loc[i + 1] == '/' && loc[i + 2] == '/')
            return http_parse_url(loc, host, hmax, port, path, pmax, tls);
    }

    int t = 0;
    if (loc[0] != '/')
        path[t++] = '/';               /* adres wzgledny bez ukosnika */
    for (int i = 0; loc[i] && t < pmax - 1; i++)
        path[t++] = loc[i];
    path[t] = '\0';
    return t > 0;
}

/* Pobiera zasob. Zwraca 0 przy powodzeniu, -1 gdy nie udalo sie
   polaczyc albo wyslac zadania. Kod HTTP i Location w 'res'. */
static inline int http_get(uint32_t ip, uint32_t port, const char *host,
                           const char *path, int use_tls, http_sink sink,
                           void *arg, struct http_result *res)
{
    static char req[512];
    static char buf[HTTP_CHUNK];
    static char hdr[HTTP_HDR_MAX];
    static struct http_transport tp;

    res->status = 0;
    res->body_len = 0;
    res->chunked = 0;
    res->location[0] = '\0';

    tp.tls = use_tls;
    tp.sock = -1;
    if (use_tls) {
        if (tls_connect(&tp.conn, ip, port, host) < 0)
            return -1;
    } else {
        tp.sock = tcp_connect(ip, port, 5000);
        if (tp.sock < 0)
            return -1;
    }

    int r = 0;
    const char *pre = "GET ";
    while (*pre)
        req[r++] = *pre++;
    for (int i = 0; path[i] && r < (int)sizeof(req) - 128; i++)
        req[r++] = path[i];
    const char *mid = " HTTP/1.1\r\nHost: ";
    while (*mid)
        req[r++] = *mid++;
    for (int i = 0; host[i] && r < (int)sizeof(req) - 64; i++)
        req[r++] = host[i];
    const char *end = "\r\nUser-Agent: ProtonOS\r\nAccept: text/html\r\n"
                      "Connection: close\r\n\r\n";
    while (*end)
        req[r++] = *end++;

    if (http_tp_send(&tp, req, (uint32_t)r) < 0) {
        http_tp_close(&tp);
        return -1;
    }

    int hlen = 0, headers_done = 0;
    /* stany dekodera 'chunked' */
    enum { CH_SIZE, CH_DATA, CH_CRLF, CH_DONE } cstate = CH_SIZE;
    uint32_t cremain = 0;
    char sizeline[16];
    int sizelen = 0;

    for (;;) {
        int n = http_tp_recv(&tp, buf, sizeof(buf));
        if (n <= 0)
            break;                     /* 0 = koniec strumienia, -1 = timeout */

        int i = 0;

        if (!headers_done) {
            while (i < n && !headers_done) {
                if (hlen < HTTP_HDR_MAX - 1)
                    hdr[hlen++] = buf[i];
                i++;
                if (hlen >= 4 && hdr[hlen - 4] == '\r' && hdr[hlen - 3] == '\n' &&
                    hdr[hlen - 2] == '\r' && hdr[hlen - 1] == '\n')
                    headers_done = 1;
            }
            if (!headers_done)
                continue;

            hdr[hlen] = '\0';

            /* status: "HTTP/1.x NNN ..." */
            if (hlen > 12)
                res->status = (hdr[9] - '0') * 100 + (hdr[10] - '0') * 10 +
                              (hdr[11] - '0');

            /* naglowki linia po linii */
            for (int p = 0; p < hlen;) {
                int q = p;
                while (q < hlen && hdr[q] != '\r')
                    q++;
                char save = hdr[q];
                hdr[q] = '\0';

                const char *v = uhttp_header(hdr + p, "transfer-encoding");
                if (v) {
                    for (int k = 0; v[k]; k++) {
                        if (uhttp_lower(v[k]) == 'c' && uhttp_lower(v[k + 1]) == 'h') {
                            res->chunked = 1;
                            break;
                        }
                    }
                }
                v = uhttp_header(hdr + p, "location");
                if (v) {
                    int k = 0;
                    while (v[k] && k < (int)sizeof(res->location) - 1) {
                        res->location[k] = v[k];
                        k++;
                    }
                    res->location[k] = '\0';
                }

                hdr[q] = save;
                p = q + 2;             /* przeskocz CRLF */
                if (q + 1 >= hlen)
                    break;
            }
            if (res->chunked)
                cstate = CH_SIZE;
        }

        /* tresc */
        while (i < n) {
            if (!res->chunked) {
                sink(buf + i, (uint32_t)(n - i), arg);
                res->body_len += (uint32_t)(n - i);
                i = n;
                break;
            }

            if (cstate == CH_DONE)
                break;

            if (cstate == CH_SIZE) {
                char c = buf[i++];
                if (c == '\n') {
                    sizeline[sizelen] = '\0';
                    uint32_t v = 0;
                    for (int k = 0; sizeline[k]; k++) {
                        char d = sizeline[k];
                        uint32_t digit;
                        if (d >= '0' && d <= '9')
                            digit = (uint32_t)(d - '0');
                        else if (d >= 'a' && d <= 'f')
                            digit = (uint32_t)(d - 'a' + 10);
                        else if (d >= 'A' && d <= 'F')
                            digit = (uint32_t)(d - 'A' + 10);
                        else
                            break;     /* rozszerzenia po sredniku itp. */
                        v = v * 16 + digit;
                    }
                    sizelen = 0;
                    cremain = v;
                    cstate = v ? CH_DATA : CH_DONE;
                } else if (c != '\r' && sizelen < (int)sizeof(sizeline) - 1) {
                    sizeline[sizelen++] = c;
                }
                continue;
            }

            if (cstate == CH_DATA) {
                uint32_t avail = (uint32_t)(n - i);
                uint32_t take = avail < cremain ? avail : cremain;
                sink(buf + i, take, arg);
                res->body_len += take;
                i += (int)take;
                cremain -= take;
                if (!cremain)
                    cstate = CH_CRLF;
                continue;
            }

            /* CH_CRLF: pomin znaczniki konca kawalka */
            if (buf[i] == '\n')
                cstate = CH_SIZE;
            i++;
        }

        if (res->chunked && cstate == CH_DONE)
            break;
    }

    http_tp_close(&tp);
    return 0;
}

/* Rozwiazuje nazwe (albo bierze adres wprost). 0 = nie udalo sie. */
static inline uint32_t http_resolve(const char *host)
{
    uint32_t ip = uparse_ip(host);
    if (ip)
        return ip;

    struct netinfo ni;
    if (netinfo(&ni) < 0 || !ni.dns)
        return 0;
    return dns_lookup(host, ni.dns, 0);
}

#endif
