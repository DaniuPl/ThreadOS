/* udpecho - a UDP server in ring 3: it echoes back what it received
 *
 * It shows that the network has moved out of the kernel into
 * processes: a socket belongs to a process, the kernel cleans it up
 * after its death, and the data travel through checked pointers.
 */

#include "usys.h"

#define PORT 7777

void _start(void)
{
    int32_t sock = udp_open(PORT);
    if (sock < 0) {
        print("[udpecho] the socket could not be opened\n");
        sys_exit(1);
    }

    print("[udpecho] listening on port 7777\n");

    char buf[256];
    uint32_t from_ip, from_port;
    int served = 0;

    /* A few datagrams or 15 s - then we finish, so as not to hang */
    while (served < 4) {
        int32_t n = udp_recv(sock, buf, sizeof(buf) - 1, 15000,
                             &from_ip, &from_port);
        if (n < 0)
            break;
        buf[n] = '\0';

        print("[udpecho] od ");
        print_ip(from_ip);
        print(":");
        print_dec(from_port);
        print(" -> '");
        print(buf);
        print("'\n");

        udp_send(sock, from_ip, from_port, buf, (uint32_t)n);
        served++;
    }

    udp_close(sock);
    print("[udpecho] done, datagrams served: ");
    print_dec((uint32_t)served);
    print("\n");
    sys_exit(0);
}
