/* httpget - klient HTTP/HTTPS w ring 3
 *
 * Uzycie: run httpget <adres> [port] [sciezka] [plik]
 *   run httpget 10.0.2.2 8000 /                - na konsole
 *   run httpget https://przyklad.pl / str.htm  - przez TLS 1.3, do pliku
 *
 * Nazwe hosta rozwiazuje przez DNS, radzi sobie z kodowaniem 'chunked'
 * i idzie za przekierowaniami (do trzech). Dla adresow https laczy sie
 * przez TLS 1.3 - certyfikat nie jest jeszcze sprawdzany (patrz utls.h).
 */

#include "usys.h"
#include "uhttp.h"

#define MAX_REDIRECTS 3

struct sink_state {
    int fd;                  /* -1 = pisz na konsole */
    uint32_t written;
};

static void body_sink(const char *data, uint32_t len, void *arg)
{
    struct sink_state *st = arg;
    if (st->fd >= 0)
        sys_write(st->fd, data, len);
    else
        sys_write(1, data, len);
    st->written += len;
}

void _start(int argc, char **argv)
{
    if (argc < 2) {
        print("uzycie: httpget <host|ip> [port] [sciezka] [plik]\n");
        sys_exit(1);
    }

    char host[64];
    char path[128];
    uint32_t port = argc > 2 ? (uint32_t)uatoi(argv[2]) : 0;
    const char *outfile = argc > 4 ? argv[4] : 0;

    /* Host moze byc sam ("przyklad.pl") albo pelnym adresem */
    uint32_t parsed_port = port;
    int use_tls = 0;
    if (!http_parse_url(argv[1], host, sizeof(host), &parsed_port,
                        path, sizeof(path), &use_tls)) {
        print("[httpget] zly adres\n");
        sys_exit(1);
    }
    if (port)
        parsed_port = port;              /* port z argumentu ma pierwszenstwo */
    if (argc > 3) {
        int i = 0;
        while (argv[3][i] && i < (int)sizeof(path) - 1) {
            path[i] = argv[3][i];
            i++;
        }
        path[i] = '\0';
    }

    struct sink_state st;
    st.fd = -1;
    st.written = 0;

    if (outfile) {
        st.fd = sys_open(outfile, O_CREATE | O_WRITE);
        if (st.fd < 0) {
            print("[httpget] nie moge otworzyc pliku ");
            print(outfile);
            print("\n");
            sys_exit(1);
        }
    }

    struct http_result res;
    for (int hop = 0; hop <= MAX_REDIRECTS; hop++) {
        uint32_t ip = http_resolve(host);
        if (!ip) {
            print("[httpget] nie znam adresu ");
            print(host);
            print("\n");
            if (st.fd >= 0)
                sys_close(st.fd);
            sys_exit(1);
        }

        print(use_tls ? "[httpget] TLS 1.3 " : "[httpget] ");
        print(host);
        print(" (");
        print_ip(ip);
        print(":");
        print_dec(parsed_port);
        print(")");
        print(path);
        print("\n");

        if (http_get(ip, parsed_port, host, path, use_tls, body_sink, &st,
                     &res) < 0) {
            print("[httpget] nie udalo sie polaczyc\n");
            if (st.fd >= 0)
                sys_close(st.fd);
            sys_exit(1);
        }

        int redirect = (res.status == 301 || res.status == 302 ||
                        res.status == 307 || res.status == 308);
        if (!redirect || !res.location[0])
            break;

        print("[httpget] przekierowanie -> ");
        print(res.location);
        print("\n");
        if (!http_apply_location(res.location, host, sizeof(host),
                                 &parsed_port, path, sizeof(path), &use_tls))
            break;
        st.written = 0;                  /* tresc przekierowania nie liczy sie */
    }

    if (st.fd >= 0)
        sys_close(st.fd);

    print("[httpget] HTTP ");
    print_dec((uint32_t)res.status);
    print(res.chunked ? " (chunked), tresc " : ", tresc ");
    print_dec(res.body_len);
    print(" B\n");
    sys_exit(res.status == 200 ? 0 : 1);
}
