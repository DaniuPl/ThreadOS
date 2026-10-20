#ifndef THREADOS_VIRTIO_NET_H
#define THREADOS_VIRTIO_NET_H

#include <stdint.h>

int virtio_net_init(void);
int virtio_net_present(void);
const uint8_t *virtio_net_mac(void);
int virtio_net_send(const void *frame, uint16_t len);
uint32_t virtio_net_rx_packets(void);
uint32_t virtio_net_tx_packets(void);

#endif
