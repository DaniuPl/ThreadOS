/* The virt machine's network card: virtio-net over MMIO
 *
 * Two queues: the receive queue holds buffers the card fills with
 * frames as they come, the transmit queue frames the card is to send.
 * Each buffer starts with virtio's 12-byte header - all zeros going
 * out, nothing asked of the card's checksums or segmentation, and
 * ignored coming in: the stack checks its own sums.
 *
 * A frame that arrives raises the card's interrupt, which only wakes
 * the receiving thread: the stack (kernel/net.c) is not run from an
 * interrupt, but under its lock (netglue.c), by that thread. A frame to
 * send is copied into a free transmit buffer and the card told; the
 * buffers it has finished with are taken back at the next send.
 *
 * This is kernel/include/netdev.h for the ARM64 kernel - the layer the
 * PC's drivers stand behind - with one card to answer for.
 */

#include <stdint.h>
#include "arm64.h"
#include "netglue.h"
#include "thread.h"
#include "virtio.h"
#include "../kernel/include/net.h"
#include "../kernel/include/netdev.h"
#include "../kernel/include/pmm.h"
#include "../kernel/include/printk.h"
#include "../kernel/include/string.h"

#define VIRTIO_ID_NET  1
#define NET_F_MAC      (1u << 5)
#define HDR            12        /* struct virtio_net_hdr, VERSION_1 */
#define BUF            2048      /* a frame and its header, two a page */
#define RX_BUFS        64
#define TX_BUFS        32
#define FRAME_MAX      1514

static struct virtio_dev *dev;
static struct virtq rxq, txq;
static uint8_t *rxbuf[RX_BUFS], *txbuf[TX_BUFS];
static uint8_t txbusy[TX_BUFS];
static uint8_t mac[6];
static uint32_t rx_count, tx_count;
static struct thread *rx_thread;
static volatile int rx_kick;
/* Airplane mode (the phone's settings): the card neither sends nor
   hears, as a phone's radio switched off */
static volatile int quiet;

void vnet_airplane(int on)
{
    quiet = on;
}

static void rx_irq(void)
{
    virtio_ack(dev);
    rx_kick = 1;
    if (rx_thread)
        thread_wake(rx_thread);
}

/* Every frame the card has put in a buffer, to the stack; the buffer
   back to the card */
static void rx_main(void *arg)
{
    (void)arg;
    for (;;) {
        /* The interrupt wakes it; a look every tenth of a second all the
           same, so a lost one costs a delay, never the network */
        thread_wait(&rx_kick, counter_now() + timer_freq() / 10);
        rx_kick = 0;
        net_enter();
        int id, given = 0;
        uint32_t len;
        while ((id = virtio_take(&rxq, &len)) >= 0) {
            if (id < RX_BUFS) {
                if (len > HDR && len <= BUF) {
                    rx_count++;
                    if (!quiet)
                        net_rx(rxbuf[id] + HDR, (uint16_t)(len - HDR));
                }
                virtio_give(dev, 0, &rxq, (unsigned)id, rxbuf[id], BUF);
                given = 1;
            }
        }
        if (given)
            virtio_notify(dev, 0);
        net_leave();
    }
}

/* Two buffers to a page */
static int buffers(uint8_t **bufs, int n)
{
    for (int i = 0; i < n; i += 2) {
        uint32_t pa = pmm_alloc_frame();
        if (!pa)
            return -1;
        memset((void *)(uintptr_t)pa, 0, PAGE_SIZE);
        bufs[i] = (uint8_t *)(uintptr_t)pa;
        if (i + 1 < n)
            bufs[i + 1] = (uint8_t *)(uintptr_t)pa + BUF;
    }
    return 0;
}

int vnet_init(struct virtio_dev *devs, int n)
{
    for (int i = 0; i < n && !dev; i++)
        if (devs[i].device_id == VIRTIO_ID_NET)
            dev = &devs[i];
    if (!dev)
        return -1;
    if (virtio_begin_with(dev, NET_F_MAC) < 0 ||
        virtio_queue(dev, 0, &rxq, RX_BUFS) < 0 ||
        virtio_queue(dev, 1, &txq, TX_BUFS) < 0 ||
        buffers(rxbuf, RX_BUFS) < 0 || buffers(txbuf, TX_BUFS) < 0) {
        printk("Network: the virtio card did not start\n");
        dev = 0;
        return -1;
    }
    if (dev->features_lo & NET_F_MAC) {
        for (int i = 0; i < 6; i++)
            mac[i] = virtio_cfg8(dev, (unsigned)i);
    } else {
        /* Locally administered, made up: QEMU's own prefix otherwise */
        static const uint8_t own[6] = { 0x02, 0x54, 0x4F, 0x53, 0x00, 0x01 };
        memcpy(mac, own, 6);
    }
    virtio_ready(dev);
    for (unsigned i = 0; i < RX_BUFS && i < rxq.size; i++)
        virtio_give(dev, 0, &rxq, i, rxbuf[i], BUF);
    virtio_notify(dev, 0);
    rx_thread = thread_create("netrx", rx_main, 0);
    gic_enable(dev->irq, rx_irq);
    printk("Network: virtio-net, MAC %x:%x:%x:%x:%x:%x\n", mac[0], mac[1],
           mac[2], mac[3], mac[4], mac[5]);
    return 0;
}

/* --- kernel/include/netdev.h --------------------------------------------- */

int netdev_init(void)
{
    return dev != 0;
}

int netdev_present(void)
{
    return dev != 0;
}

const char *netdev_name(void)
{
    return dev ? "VirtIO-net" : "none";
}

const uint8_t *netdev_mac(void)
{
    return mac;
}

/* Under the stack's lock, as every caller of it is */
int netdev_send(const void *frame, uint16_t len)
{
    if (!dev || !frame || len > FRAME_MAX || quiet)
        return -1;
    int slot = -1;
    for (int tries = 0; slot < 0 && tries < 100000; tries++) {
        int id;
        while ((id = virtio_take(&txq, 0)) >= 0)
            if (id < TX_BUFS)
                txbusy[id] = 0;
        for (int i = 0; i < TX_BUFS && i < txq.size && slot < 0; i++)
            if (!txbusy[i])
                slot = i;
    }
    if (slot < 0)
        return -1;
    txbusy[slot] = 1;
    memset(txbuf[slot], 0, HDR);
    memcpy(txbuf[slot] + HDR, frame, len);
    virtio_offer(dev, 1, &txq, (unsigned)slot, txbuf[slot], HDR + len, 0);
    virtio_notify(dev, 1);
    tx_count++;
    return 0;
}

uint32_t netdev_rx_packets(void)
{
    return rx_count;
}

uint32_t netdev_tx_packets(void)
{
    return tx_count;
}

int netdev_link(void)
{
    return -1;                   /* unknown, as for every card but one */
}
