#ifndef THREADOS_ARM64_VIRTIO_H
#define THREADOS_ARM64_VIRTIO_H

#include <stdint.h>

/* virtio over MMIO, the modern interface (version 2) - virtio.c */

struct virtq {
    uint16_t size;
    volatile struct vq_desc {
        uint64_t addr;
        uint32_t len;
        uint16_t flags, next;
    } *desc;
    volatile struct vq_avail {
        uint16_t flags, idx;
        uint16_t ring[];
    } *avail;
    volatile struct vq_used {
        uint16_t flags, idx;
        struct { uint32_t id, len; } ring[];
    } *used;
    uint16_t last_used;          /* how far this side has read the used ring */
};

#define VQ_DESC_WRITE 2          /* the device writes the buffer */

struct virtio_dev {
    volatile uint8_t *regs;
    uint32_t device_id;
    uint32_t irq;
};

/* The transports the device tree lists, with something on them: up to
   'max' found, the number returned. Only modern ones (version 2). */
int virtio_probe(const uint64_t *bases, const uint32_t *irqs, int n,
                 struct virtio_dev *out, int max);
/* Reset, the features agreed (VERSION_1 and nothing else), queues
   made: 0 = ready for virtio_queue() and then virtio_ready() */
int virtio_begin(struct virtio_dev *d);
int virtio_queue(struct virtio_dev *d, unsigned index, struct virtq *q,
                 unsigned want);
void virtio_ready(struct virtio_dev *d);
/* A buffer the device is to fill, put on the queue; then the device is
   told. Returns the descriptor's index. */
int virtio_give(struct virtio_dev *d, unsigned index, struct virtq *q,
                unsigned desc, void *buf, uint32_t len);
void virtio_notify(struct virtio_dev *d, unsigned index);
/* The next buffer the device has finished with: its descriptor, or -1 */
int virtio_take(struct virtq *q, uint32_t *len);
/* Device configuration, bytes from offset */
uint8_t virtio_cfg8(struct virtio_dev *d, unsigned off);
uint32_t virtio_cfg32(struct virtio_dev *d, unsigned off);
void virtio_cfg_write8(struct virtio_dev *d, unsigned off, uint8_t v);

#endif
