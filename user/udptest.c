/* udptest - klient UDP: wysyla datagram do udpecho i czeka na echo
 *
 * Domyslnie gada z wlasnym systemem (petla zwrotna), wiec dziala nawet
 * bez sieci na zewnatrz. Z argumentem wysyla pod wskazany adres.
 */

#include "usys.h"

#define PORT 7777
#define MSG  "proton-udp"

/* Prosty parser adresu IPv4 (bez walidacji zakresow - to demo) */
static uint32_t parse_ip(const char *s)
{
    uint32_t ip = 0, part = 0;
    for (; *s; s++) {
        if (*s == '.') {
            ip = (ip << 8) | (part & 0xFF);
            part = 0;
        } else if (*s >= '0' && *s <= '9') {
            part = part * 10 + (uint32_t)(*s - '0');
        } else {
            return 0;
        }
    }
    return (ip << 8) | (part & 0xFF);
}

void _start(int argc, char **argv)
{
    struct netinfo ni;
    if (netinfo(&ni) < 0) {
        print("[udptest] system nie ma jeszcze adresu IP\n");
        sys_exit(1);
    }

    uint32_t target = argc > 1 ? parse_ip(argv[1]) : ni.ip;
    if (!target) {
        print("[udptest] zly adres\n");
        sys_exit(1);
    }

    int32_t sock = udp_open(0);         /* port efemeryczny */
    if (sock < 0) {
        print("[udptest] brak wolnego gniazda\n");
        sys_exit(1);
    }

    print("[udptest] wysylam '" MSG "' do ");
    print_ip(target);
    print(":7777\n");

    if (udp_send(sock, target, PORT, MSG, sizeof(MSG) - 1) < 0) {
        print("[udptest] wyslanie nie powiodlo sie\n");
        udp_close(sock);
        sys_exit(1);
    }

    char buf[128];
    uint32_t from_ip, from_port;
    int32_t n = udp_recv(sock, buf, sizeof(buf) - 1, 3000, &from_ip, &from_port);
    udp_close(sock);

    if (n < 0) {
        print("[udptest] brak odpowiedzi (timeout 3 s)\n");
        sys_exit(1);
    }
    buf[n] = '\0';

    print("[udptest] echo wrocilo: ");
    print(buf);
    print(" (od ");
    print_ip(from_ip);
    print(":");
    print_dec(from_port);
    print(")\n");
    sys_exit(ustreq(buf, MSG) ? 0 : 1);
}
