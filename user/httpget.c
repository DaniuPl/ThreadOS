/* httpget - klient HTTP w ring 3 (TCP + HTTP/1.0)
 *
 * Uzycie: run httpget <ip> [port] [sciezka] [plik]
 *   run httpget 10.0.2.2 8000 /            - wypisz odpowiedz na konsole
 *   run httpget 10.0.2.2 8000 / strona.htm - zapisz tresc do pliku na PFS
 *
 * Nazwe hosta rozwiazuje osobno 'nslookup' - tutaj podajemy adres,
 * zeby program robil jedna rzecz.
 */

#include "usys.h"

#define BUF 1024

static void append(char *dst, int *len, const char *s)
{
    while (*s)
        dst[(*len)++] = *s++;
}

void _start(int argc, char **argv)
{
    if (argc < 2) {
        print("uzycie: httpget <ip> [port] [sciezka] [plik]\n");
        sys_exit(1);
    }

    uint32_t ip = uparse_ip(argv[1]);
    if (!ip) {
        print("[httpget] zly adres IP\n");
        sys_exit(1);
    }
    uint32_t port = argc > 2 ? (uint32_t)uatoi(argv[2]) : 80;
    const char *path = argc > 3 ? argv[3] : "/";
    const char *outfile = argc > 4 ? argv[4] : 0;

    print("[httpget] lacze sie z ");
    print_ip(ip);
    print(":");
    print_dec(port);
    print("\n");

    int32_t conn = tcp_connect(ip, port, 5000);
    if (conn < 0) {
        print("[httpget] nie udalo sie polaczyc\n");
        sys_exit(1);
    }

    /* Zadanie HTTP/1.0: serwer zamyka polaczenie po odpowiedzi, wiec
       koniec danych poznajemy po FIN - bez parsowania Content-Length */
    static char req[256];
    int rlen = 0;
    append(req, &rlen, "GET ");
    append(req, &rlen, path);
    append(req, &rlen, " HTTP/1.0\r\nHost: ");
    append(req, &rlen, argv[1]);
    append(req, &rlen, "\r\nUser-Agent: ProtonOS\r\nConnection: close\r\n\r\n");

    if (tcp_send(conn, req, (uint32_t)rlen, 5000) < 0) {
        print("[httpget] nie udalo sie wyslac zadania\n");
        tcp_close(conn);
        sys_exit(1);
    }

    int32_t fd = -1;
    if (outfile) {
        fd = sys_open(outfile, O_CREATE | O_WRITE);
        if (fd < 0) {
            print("[httpget] nie moge otworzyc pliku ");
            print(outfile);
            print("\n");
            tcp_close(conn);
            sys_exit(1);
        }
    }

    static char buf[BUF];
    uint32_t total = 0;
    int header_done = 0;
    int status_shown = 0;

    for (;;) {
        int32_t n = tcp_recv(conn, buf, BUF, 10000);
        if (n < 0) {
            print("[httpget] przerwane (timeout)\n");
            break;
        }
        if (n == 0)
            break;                       /* serwer zamknal - koniec tresci */
        total += (uint32_t)n;

        if (!status_shown) {             /* pierwsza linia odpowiedzi */
            int end = 0;
            while (end < n && buf[end] != '\r' && buf[end] != '\n')
                end++;
            char save = buf[end];
            buf[end] = '\0';
            print("[httpget] ");
            print(buf);
            print("\n");
            buf[end] = save;
            status_shown = 1;
        }

        if (fd >= 0) {
            /* Do pliku zapisujemy sama tresc - naglowki konczy pusta
               linia (CRLF CRLF) */
            int start = 0;
            if (!header_done) {
                for (int i = 0; i + 3 < n; i++) {
                    if (buf[i] == '\r' && buf[i + 1] == '\n' &&
                        buf[i + 2] == '\r' && buf[i + 3] == '\n') {
                        start = i + 4;
                        header_done = 1;
                        break;
                    }
                }
                if (!header_done)
                    continue;            /* naglowki jeszcze trwaja */
            }
            if (n > start)
                sys_write(fd, buf + start, (uint32_t)(n - start));
        } else {
            sys_write(1, buf, (uint32_t)n);
        }
    }

    if (fd >= 0)
        sys_close(fd);
    tcp_close(conn);

    print("[httpget] odebrano ");
    print_dec(total);
    print(" B\n");
    sys_exit(total ? 0 : 1);
}
