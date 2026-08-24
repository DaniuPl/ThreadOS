/* udpecho - serwer UDP w ring 3: odsyla to, co dostal
 *
 * Pokazuje, ze siec wyszla z jadra do procesow: gniazdo nalezy do
 * procesu, jadro sprzata je po jego smierci, a dane chodza przez
 * sprawdzone wskazniki.
 */

#include "usys.h"

#define PORT 7777

void _start(void)
{
    int32_t sock = udp_open(PORT);
    if (sock < 0) {
        print("[udpecho] nie udalo sie otworzyc gniazda\n");
        sys_exit(1);
    }

    print("[udpecho] nasluchuje na porcie 7777\n");

    char buf[256];
    uint32_t from_ip, from_port;
    int served = 0;

    /* Kilka datagramow albo 15 s - potem konczymy, zeby nie wisiec */
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
    print("[udpecho] koniec, obsluzonych datagramow: ");
    print_dec((uint32_t)served);
    print("\n");
    sys_exit(0);
}
