/* unet64 - the network as seen from a program
 *
 * Until now only the kernel used the network: 'ping' and 'ifconfig' are
 * its own commands. This program does it itself, through system calls,
 * and has no special rights while doing so.
 *
 * It checks three things in turn:
 *   - whether it knows its own address (the one from DHCP),
 *   - whether a datagram sent to itself comes back the same,
 *   - whether it can connect to a server and get an answer from it.
 *
 * The server's address and port are given in the arguments; without
 * them it only makes the first two attempts.
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
    int n = put(0, "[unet64] ERROR: ");
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

    /* 1. Our own address - the one that came from the network through
   DHCP */
    if (sys_netinfo(addrs) != 1)
        err("the system has no IP address", 0);

    int n = put(0, "[unet64] my address ");
    n = put_ip(n, addrs[0]);
    n = put(n, ", gateway ");
    n = put_ip(n, addrs[2]);
    n = put(n, "\n");
    out[n] = 0;
    print(out);

    /* 2. A datagram to ourselves. The loopback does not go out on the
          wire, so the attempt does not depend on any network
          outside. */
    int64_t s = sys_udp_open(MY_PORT);
    if (s < 0)
        err("I cannot open a UDP socket", s);

    static const char body[] = "datagram z ring 3";
    struct udp_info o;
    o.sock = (int)s;
    o.ip = addrs[0];
    o.port = MY_PORT;
    o.timeout = 3000;

    if (sys_udp_send(&o, body, ustrlen(body)) < 0)
        err("sending the datagram did not work", -1);

    o.sock = (int)s;
    o.timeout = 3000;
    int64_t got = sys_udp_recv(&o, buf, sizeof(buf));
    if (got <= 0)
        err("the datagram did not come back", got);

    if ((uint64_t)got != ustrlen(body))
        err("a datagram of another length came back", got);
    for (int64_t i = 0; i < got; i++)
        if (buf[i] != body[i])
            err("the datagram's content does not match", i);

    n = put(0, "[unet64] the datagram came back to me (");
    n = putn(n, (uint64_t)got);
    n = put(n, " B) od ");
    n = put_ip(n, o.ip);
    n = put(n, "\n");
    out[n] = 0;
    print(out);
    sys_udp_close((int)s);

    if (argc < 3) {
        print("[unet64] with no server address I stop at UDP\n");
        sys_exit(0);
    }

    /* 3. A TCP connection and an HTTP request - the shortest way to
          check that the stream works in both directions */
    int64_t ip = uparse_ip(argv[1]);
    int64_t port = uatoi(argv[2]);
    if (ip < 0 || port <= 0)
        err("a bad server address or port", 0);

    o.ip = (uint32_t)ip;
    o.port = (uint16_t)port;
    o.timeout = 5000;
    int64_t c = sys_tcp_connect(&o);
    if (c < 0)
        err("I cannot connect", c);

    static const char task[] = "GET / HTTP/1.0\r\n\r\n";
    if (sys_tcp_send((int)c, task, ustrlen(task)) < 0)
        err("sending the request did not work", -1);

    int64_t total = 0;
    for (;;) {
        int64_t k = sys_tcp_recv((int)c, buf, sizeof(buf) - 1);
        if (k <= 0)
            break;
        total += k;
        if (total > 16)
            break;                   /* the header is proof enough */
    }
    sys_tcp_close((int)c);

    if (total <= 0)
        err("the server answered nothing", total);

    buf[16] = 0;
    n = put(0, "[unet64] the server answered (");
    n = putn(n, (uint64_t)total);
    n = put(n, " B): ");
    n = put(n, buf);
    n = put(n, "\n");
    out[n] = 0;
    print(out);

    sys_exit(0);
}
