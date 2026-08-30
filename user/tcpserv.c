/* tcpserv - a TCP server in ring 3 (port 5555)
 *
 * It greets a client, echoes back what it received and ends the
 * connection. Proof that ProtonOS handles a passive open too: a SYN
 * from a foreign host, SYN|ACK, a queue of established connections and
 * a stream in both directions.
 */

#include "usys.h"

#define PORT 5555
#define GREETING "ProtonOS TCP gotowy\n"

void _start(void)
{
    int32_t lis = tcp_listen(PORT);
    if (lis < 0) {
        print("[tcpserv] nie moge nasluchiwac na porcie 5555\n");
        sys_exit(1);
    }
    print("[tcpserv] nasluchuje na porcie 5555\n");

    int served = 0;
    while (served < 3) {
        int32_t conn = tcp_accept(lis, 30000);
        if (conn < 0)
            break;                       /* nobody came - we finish */

        print("[tcpserv] polaczenie przyjete\n");
        tcp_send(conn, GREETING, sizeof(GREETING) - 1, 5000);

        char buf[256];
        int32_t n = tcp_recv(conn, buf, sizeof(buf) - 1, 5000);
        if (n > 0) {
            buf[n] = '\0';
            print("[tcpserv] od klienta: ");
            print(buf);
            if (buf[n - 1] != '\n')
                print("\n");
            tcp_send(conn, buf, (uint32_t)n, 5000);
        }

        tcp_close(conn);
        served++;
    }

    tcp_close(lis);
    print("[tcpserv] koniec, obsluzonych polaczen: ");
    print_dec((uint32_t)served);
    print("\n");
    sys_exit(0);
}
