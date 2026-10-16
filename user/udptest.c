/* udptest - a UDP client: it sends a datagram to udpecho and waits for
 * the echo
 *
 * By default it talks to its own system (a loopback), so it works even
 * with no network outside. With an argument it sends to the given
 * address.
 */

#include "usys.h"

#define PORT 7777
#define MSG  "thread-udp"

/* A simple IPv4 address parser (with no range validation - this is a
   demo) */
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
        print("[udptest] the system has no IP address yet\n");
        sys_exit(1);
    }

    uint32_t target = argc > 1 ? parse_ip(argv[1]) : ni.ip;
    if (!target) {
        print("[udptest] a bad address\n");
        sys_exit(1);
    }

    int32_t sock = udp_open(0);         /* an ephemeral port */
    if (sock < 0) {
        print("[udptest] no free socket\n");
        sys_exit(1);
    }

    print("[udptest] sending '" MSG "' do ");
    print_ip(target);
    print(":7777\n");

    if (udp_send(sock, target, PORT, MSG, sizeof(MSG) - 1) < 0) {
        print("[udptest] sending did not work\n");
        udp_close(sock);
        sys_exit(1);
    }

    char buf[128];
    uint32_t from_ip, from_port;
    int32_t n = udp_recv(sock, buf, sizeof(buf) - 1, 3000, &from_ip, &from_port);
    udp_close(sock);

    if (n < 0) {
        print("[udptest] no answer (3 s timeout)\n");
        sys_exit(1);
    }
    buf[n] = '\0';

    print("[udptest] the echo came back: ");
    print(buf);
    print(" (od ");
    print_ip(from_ip);
    print(":");
    print_dec(from_port);
    print(")\n");
    sys_exit(ustreq(buf, MSG) ? 0 : 1);
}
