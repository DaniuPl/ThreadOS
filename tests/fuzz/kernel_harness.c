/* Putting the kernel's packet path through damaged data
 *
 * net.c and tcp.c take apart the frames that come in off the network -
 * that is, data anybody at all can hand us. A bug in that place does
 * not end with a process dying, it ends with the whole system falling
 * over, so it is worth running this code through the sanitizers on the
 * host.
 *
 * The kernel's own sources are pulled in directly; the stubs in
 * kstub/include stand in for the hardware, the scheduler and printing.
 *
 * Usage: kernel_harness <file with a frame>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net.h"
#include "tcp.h"

/* net.c and tcp.c are compiled separately (kstub/net.c and
   kstub/tcp.c are links to the kernel's sources, next to the directory
   with the stubs) */

static uint16_t sum16(const uint8_t *p, uint32_t n, uint32_t sum)
{
    for (; n > 1; p += 2, n -= 2)
        sum += (uint32_t)p[0] << 8 | p[1];
    if (n)
        sum += (uint32_t)p[0] << 8;
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

/* The kernel drops a frame whose checksums do not add up before it
   looks any further, and a corrupted frame almost never adds up - so
   every case is also given again with them made right, which takes the
   corruption past them into the IP, ICMP, UDP and TCP code */
static void fix_checksums(uint8_t *f, size_t len)
{
    if (len < 14 + 20 || f[12] != 0x08 || f[13] != 0x00)
        return;
    uint8_t *ip = f + 14;
    uint32_t ihl = (uint32_t)(ip[0] & 0x0F) * 4;
    uint32_t total = (uint32_t)ip[2] << 8 | ip[3];
    if (ihl < 20 || total < ihl || total > len - 14)
        return;
    ip[10] = ip[11] = 0;
    uint16_t c = sum16(ip, ihl, 0);
    ip[10] = (uint8_t)(c >> 8);
    ip[11] = (uint8_t)c;
    uint8_t *p = ip + ihl;
    uint32_t n = total - ihl;
    int at = ip[9] == 6 ? 16 : ip[9] == 17 ? 6 : ip[9] == 1 ? 2 : -1;
    if (at < 0 || n < (uint32_t)at + 2)
        return;
    p[at] = p[at + 1] = 0;
    uint32_t pseudo = 0;
    if (ip[9] != 1) {
        uint32_t len16 = ip[9] == 17 ? ((uint32_t)p[4] << 8 | p[5]) : n;
        if (len16 > n)
            len16 = n;
        n = len16;
        for (int i = 12; i < 20; i += 2)
            pseudo += (uint32_t)ip[i] << 8 | ip[i + 1];
        pseudo += ip[9] + n;
    }
    c = sum16(p, n, pseudo);
    p[at] = (uint8_t)(c >> 8);
    p[at + 1] = (uint8_t)c;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <file with a frame>\n", argv[0]);
        return 2;
    }

    static unsigned char scratch[65536];
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 2;
    }
    size_t len = fread(scratch, 1, sizeof(scratch), f);
    fclose(f);

    /* A buffer of exactly that length - otherwise a read past the end
       of the frame would fit in the slack and go unnoticed */
    unsigned char *frame = malloc(len ? len : 1);
    memcpy(frame, scratch, len);

    /* A system with an address, so packets do not fall out on the very
       first condition */
    net_set_config(IP4(10, 0, 2, 15), IP4(255, 255, 255, 0),
                   IP4(10, 0, 2, 2), IP4(10, 0, 2, 3));

    /* Sockets and connections, so the traffic has somewhere to land */
    net_udp_open(53, -1);
    net_udp_open(7777, -1);
    tcp_listen(5555, -1);

    net_rx(frame, (uint16_t)len);
    net_rx(frame, (uint16_t)len);      /* a second time: the state has changed */
    fix_checksums(frame, len);
    net_rx(frame, (uint16_t)len);      /* and with its checksums made right */

    free(frame);
    return 0;
}
