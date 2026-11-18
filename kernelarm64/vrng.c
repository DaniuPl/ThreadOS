/* The virt machine's entropy device: virtio-rng over MMIO
 *
 * One queue: the driver offers a buffer, the device fills it with bytes
 * from the host's own generator. They go into the kernel's pool
 * (kernel/random.c) - added, never used as they are, so a device that
 * lies costs nothing but itself. Read at boot, and again at every
 * program's request for random bytes (vrng_stir), by the thread that
 * asks.
 */

#include <stdint.h>
#include "arm64.h"
#include "netglue.h"
#include "virtio.h"
#include "../kernel/include/printk.h"
#include "../kernel/include/random.h"
#include "../kernel/include/spinlock.h"
#include "../kernel/include/string.h"

#define VIRTIO_ID_RNG 4
#define CHUNK 64

static struct virtio_dev *dev;
static struct virtq q;
static uint8_t buf[CHUNK] __attribute__((aligned(64)));
static struct spinlock lock = SPINLOCK_FREE;

/* CHUNK bytes from the device into the pool: 1, or 0 when it said
   nothing in time */
static int stir(void)
{
    irqflags_t f = spin_lock_irq(&lock);
    int got = 0;
    if (dev) {
        virtio_give(dev, 0, &q, 0, buf, CHUNK);
        virtio_notify(dev, 0);
        uint64_t give_up = counter_now() + timer_freq() / 10;
        uint32_t len = 0;
        while (virtio_take(&q, &len) < 0 && counter_now() < give_up)
            ;
        if (len) {
            random_add(buf, len > CHUNK ? CHUNK : len);
            memset(buf, 0, sizeof(buf));
            got = 1;
        }
    }
    spin_unlock_irq(&lock, f);
    return got;
}

int vrng_init(struct virtio_dev *devs, int n)
{
    for (int i = 0; i < n && !dev; i++)
        if (devs[i].device_id == VIRTIO_ID_RNG)
            dev = &devs[i];
    if (!dev)
        return -1;
    if (virtio_begin(dev) < 0 || virtio_queue(dev, 0, &q, 4) < 0) {
        dev = 0;
        return -1;
    }
    virtio_ready(dev);
    int ok = stir() && stir();
    printk("Random: virtio entropy device, %s\n",
           ok ? "128 bytes into the pool" : "but it gave nothing");
    return ok ? 0 : -1;
}

void vrng_stir(void)
{
    stir();
}
