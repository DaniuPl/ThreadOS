#ifndef THREADOS_ARM64_NETGLUE_H
#define THREADOS_ARM64_NETGLUE_H

#include "virtio.h"

/* netglue.c: the network stack (kernel/net.c, tcp.c, dhcp.c) is run by
   one thread at a time, between these two; task_sleep inside lets go */
void net_enter(void);
void net_leave(void);

/* vnet.c: the virtio network card among the devices found, set up with
   its receiving thread - 0, or -1 when there is none */
int vnet_init(struct virtio_dev *devs, int n);
/* Airplane mode: 1, the card sends nothing and what it hears is let go */
void vnet_airplane(int on);

/* vrng.c: the virtio entropy device, if there is one, into the pool
   (kernel/random.c) - at once, and again at each vrng_stir */
int vrng_init(struct virtio_dev *devs, int n);
void vrng_stir(void);

#endif
