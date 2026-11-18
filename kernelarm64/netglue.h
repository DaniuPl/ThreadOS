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

#endif
