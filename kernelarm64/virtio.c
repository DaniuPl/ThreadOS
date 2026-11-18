/* virtio over MMIO
 *
 * The virt machine's devices that are not part of the board come as
 * virtio: a small window of registers per device (virtio,mmio in the
 * device tree, 32 slots) and queues in the machine's memory through
 * which buffers go back and forth. This is the modern interface - the
 * virtio 1 specification's, version 2 of the MMIO registers; QEMU gives
 * it with -global virtio-mmio.force-legacy=false.
 *
 * A queue is three rings in memory: descriptors (where a buffer is and
 * how long), the available ring (which descriptors the driver offers)
 * and the used ring (which ones the device has finished with). Each
 * takes a page of its own here, which holds up to 64 entries of any
 * of them.
 */

#include <stdint.h>
#include "virtio.h"
#include "../kernel/include/pmm.h"
#include "../kernel/include/string.h"

#define MAGIC            0x000
#define VERSION          0x004
#define DEVICE_ID        0x008
#define DEVICE_FEATURES  0x010
#define DEVICE_FEAT_SEL  0x014
#define DRIVER_FEATURES  0x020
#define DRIVER_FEAT_SEL  0x024
#define QUEUE_SEL        0x030
#define QUEUE_NUM_MAX    0x034
#define QUEUE_NUM        0x038
#define QUEUE_READY      0x044
#define QUEUE_NOTIFY     0x050
#define INTERRUPT_STATUS 0x060
#define INTERRUPT_ACK    0x064
#define STATUS           0x070
#define QUEUE_DESC_LO    0x080
#define QUEUE_DESC_HI    0x084
#define QUEUE_AVAIL_LO   0x090
#define QUEUE_AVAIL_HI   0x094
#define QUEUE_USED_LO    0x0a0
#define QUEUE_USED_HI    0x0a4
#define CONFIG           0x100

#define ST_ACKNOWLEDGE   1
#define ST_DRIVER        2
#define ST_DRIVER_OK     4
#define ST_FEATURES_OK   8

#define F_VERSION_1_HI   1        /* feature 32: bit 0 of the high word */

#define QUEUE_MAX        64

static uint32_t rd(struct virtio_dev *d, unsigned off)
{
    return *(volatile uint32_t *)(d->regs + off);
}

static void wr(struct virtio_dev *d, unsigned off, uint32_t v)
{
    *(volatile uint32_t *)(d->regs + off) = v;
}

int virtio_probe(const uint64_t *bases, const uint32_t *irqs, int n,
                 struct virtio_dev *out, int max)
{
    int found = 0;
    for (int i = 0; i < n && found < max; i++) {
        struct virtio_dev d = { (volatile uint8_t *)(uintptr_t)bases[i], 0,
                                irqs[i], 0 };
        if (rd(&d, MAGIC) != 0x74726976 || rd(&d, VERSION) != 2)
            continue;
        d.device_id = rd(&d, DEVICE_ID);
        if (!d.device_id)
            continue;                    /* an empty slot */
        out[found++] = d;
    }
    return found;
}

int virtio_begin_with(struct virtio_dev *d, uint32_t want_lo)
{
    wr(d, STATUS, 0);
    wr(d, STATUS, ST_ACKNOWLEDGE);
    wr(d, STATUS, ST_ACKNOWLEDGE | ST_DRIVER);
    wr(d, DEVICE_FEAT_SEL, 1);
    if (!(rd(d, DEVICE_FEATURES) & F_VERSION_1_HI))
        return -1;
    wr(d, DEVICE_FEAT_SEL, 0);
    uint32_t offered = rd(d, DEVICE_FEATURES);
    d->features_lo = offered & want_lo;
    wr(d, DRIVER_FEAT_SEL, 0);
    wr(d, DRIVER_FEATURES, d->features_lo);
    wr(d, DRIVER_FEAT_SEL, 1);
    wr(d, DRIVER_FEATURES, F_VERSION_1_HI);
    wr(d, STATUS, ST_ACKNOWLEDGE | ST_DRIVER | ST_FEATURES_OK);
    if (!(rd(d, STATUS) & ST_FEATURES_OK))
        return -1;
    return 0;
}

int virtio_begin(struct virtio_dev *d)
{
    return virtio_begin_with(d, 0);
}

static void *page(void)
{
    uint32_t a = pmm_alloc_frame();
    if (a)
        memset((void *)(uintptr_t)a, 0, PAGE_SIZE);
    return (void *)(uintptr_t)a;
}

int virtio_queue(struct virtio_dev *d, unsigned index, struct virtq *q,
                 unsigned want)
{
    wr(d, QUEUE_SEL, index);
    uint32_t max = rd(d, QUEUE_NUM_MAX);
    if (!max)
        return -1;
    if (want > max)
        want = max;
    if (want > QUEUE_MAX)
        want = QUEUE_MAX;
    q->size = (uint16_t)want;
    q->desc = page();
    q->avail = page();
    q->used = page();
    q->last_used = 0;
    if (!q->desc || !q->avail || !q->used)
        return -1;
    uint64_t a;
    wr(d, QUEUE_NUM, want);
    a = (uint64_t)(uintptr_t)q->desc;
    wr(d, QUEUE_DESC_LO, (uint32_t)a);
    wr(d, QUEUE_DESC_HI, (uint32_t)(a >> 32));
    a = (uint64_t)(uintptr_t)q->avail;
    wr(d, QUEUE_AVAIL_LO, (uint32_t)a);
    wr(d, QUEUE_AVAIL_HI, (uint32_t)(a >> 32));
    a = (uint64_t)(uintptr_t)q->used;
    wr(d, QUEUE_USED_LO, (uint32_t)a);
    wr(d, QUEUE_USED_HI, (uint32_t)(a >> 32));
    wr(d, QUEUE_READY, 1);
    return 0;
}

void virtio_ready(struct virtio_dev *d)
{
    wr(d, STATUS, ST_ACKNOWLEDGE | ST_DRIVER | ST_FEATURES_OK | ST_DRIVER_OK);
}

int virtio_give(struct virtio_dev *d, unsigned index, struct virtq *q,
                unsigned desc, void *buf, uint32_t len)
{
    return virtio_offer(d, index, q, desc, buf, len, 1);
}

int virtio_offer(struct virtio_dev *d, unsigned index, struct virtq *q,
                 unsigned desc, void *buf, uint32_t len, int device_writes)
{
    (void)d;
    (void)index;
    q->desc[desc].addr = (uint64_t)(uintptr_t)buf;
    q->desc[desc].len = len;
    q->desc[desc].flags = device_writes ? VQ_DESC_WRITE : 0;
    q->desc[desc].next = 0;
    q->avail->ring[q->avail->idx % q->size] = (uint16_t)desc;
    /* The entry is in the ring before the device can see the index */
    __asm__ volatile("dmb ish" ::: "memory");
    q->avail->idx++;
    return (int)desc;
}

void virtio_notify(struct virtio_dev *d, unsigned index)
{
    __asm__ volatile("dsb sy" ::: "memory");
    wr(d, QUEUE_NOTIFY, index);
}

int virtio_take(struct virtq *q, uint32_t *len)
{
    if (q->last_used == q->used->idx)
        return -1;
    __asm__ volatile("dmb ish" ::: "memory");
    uint16_t slot = q->last_used % q->size;
    int id = (int)q->used->ring[slot].id;
    if (len)
        *len = q->used->ring[slot].len;
    q->last_used++;
    return id;
}

void virtio_ack(struct virtio_dev *d)
{
    wr(d, INTERRUPT_ACK, rd(d, INTERRUPT_STATUS));
}

uint8_t virtio_cfg8(struct virtio_dev *d, unsigned off)
{
    return *(volatile uint8_t *)(d->regs + CONFIG + off);
}

uint32_t virtio_cfg32(struct virtio_dev *d, unsigned off)
{
    return (uint32_t)virtio_cfg8(d, off) |
           (uint32_t)virtio_cfg8(d, off + 1) << 8 |
           (uint32_t)virtio_cfg8(d, off + 2) << 16 |
           (uint32_t)virtio_cfg8(d, off + 3) << 24;
}

void virtio_cfg_write8(struct virtio_dev *d, unsigned off, uint8_t v)
{
    *(volatile uint8_t *)(d->regs + CONFIG + off) = v;
}
