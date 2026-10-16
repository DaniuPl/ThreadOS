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

    free(frame);
    return 0;
}
