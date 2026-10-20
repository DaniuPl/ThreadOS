/* A small VirtIO network driver for the legacy PCI transport.
 *
 * The driver negotiates only the device MAC address and uses split
 * descriptor rings. Checksumming and segmentation remain in the
 * network stack, so every buffer is one ordinary Ethernet frame.
 */

#include <stdint.h>
#include "include/virtio_net.h"
#include "include/io.h"
#include "include/isr.h"
#include "include/net.h"
#include "include/pci.h"
#include "include/pic.h"
#include "include/pmm.h"
#include "include/printk.h"
#include "include/string.h"

#define VIRTIO_VENDOR          0x1AF4
#define VIRTIO_NET_LEGACY      0x1000
#define VIRTIO_NET_TRANSITIONAL 0x1041

#define REG_DEVICE_FEATURES 0x00
#define REG_GUEST_FEATURES  0x04
#define REG_QUEUE_PFN       0x08
#define REG_QUEUE_SIZE      0x0C
#define REG_QUEUE_SELECT    0x0E
#define REG_QUEUE_NOTIFY    0x10
#define REG_STATUS          0x12
#define REG_ISR             0x13
#define REG_DEVICE_CONFIG   0x14

#define STATUS_ACK          0x01
#define STATUS_DRIVER       0x02
#define STATUS_DRIVER_OK    0x04
#define STATUS_FAILED       0x80

#define FEATURE_NET_MAC     (1u << 5)
#define DESC_F_WRITE        2u

#define MAX_QUEUE_SIZE      1024
#define RX_BUFFER_COUNT     32
#define RX_BUFFER_SIZE      2048
#define TX_BUFFER_SIZE      2048
#define NET_HEADER_SIZE     10
#define ETH_HEADER_SIZE     14
#define TX_WAIT_SPINS       200000

struct virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct virtq_used_elem {
    uint32_t id;
    uint32_t len;
};

struct virtq {
    uint32_t phys;
    uint32_t pages;
    uint16_t size;
    uint16_t avail_next;
    uint16_t used_next;
    struct virtq_desc *desc;
    volatile uint16_t *avail_idx;
    volatile uint16_t *avail_ring;
    volatile uint16_t *used_idx;
    volatile struct virtq_used_elem *used_ring;
};

static uint16_t io_base;
static struct virtq rxq, txq;
static uint8_t rx_buffers[RX_BUFFER_COUNT][RX_BUFFER_SIZE]
    __attribute__((aligned(16)));
static uint8_t tx_buffer[TX_BUFFER_SIZE] __attribute__((aligned(16)));
static uint8_t mac[6];
static uint16_t rx_count;
static int present;
static uint32_t rx_packets, tx_packets;

static void dma_barrier(void)
{
    __asm__ volatile("" : : : "memory");
}

static void queue_release(struct virtq *q)
{
    if (q->phys) {
        for (uint32_t i = 0; i < q->pages; i++)
            pmm_free_frame(q->phys + i * PAGE_SIZE);
    }
    memset(q, 0, sizeof(*q));
}

static int queue_init(struct virtq *q, uint16_t index)
{
    outw(io_base + REG_QUEUE_SELECT, index);
    uint16_t size = inw(io_base + REG_QUEUE_SIZE);
    if (!size || size > MAX_QUEUE_SIZE || (size & (size - 1)))
        return 0;

    uint32_t desc_bytes = (uint32_t)size * sizeof(struct virtq_desc);
    uint32_t avail_bytes = 6u + (uint32_t)size * sizeof(uint16_t);
    uint32_t used_offset =
        (desc_bytes + avail_bytes + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint32_t used_bytes =
        6u + (uint32_t)size * sizeof(struct virtq_used_elem);
    uint32_t total = used_offset + used_bytes;
    q->pages = (total + PAGE_SIZE - 1) / PAGE_SIZE;
    q->phys = pmm_alloc_contiguous(q->pages);
    if (!q->phys) {
        memset(q, 0, sizeof(*q));
        return 0;
    }

    uint8_t *memory = (uint8_t *)(uintptr_t)q->phys;
    memset(memory, 0, q->pages * PAGE_SIZE);
    q->size = size;
    q->desc = (struct virtq_desc *)memory;
    q->avail_idx = (volatile uint16_t *)(memory + desc_bytes + 2);
    q->avail_ring = (volatile uint16_t *)(memory + desc_bytes + 4);
    q->used_idx = (volatile uint16_t *)(memory + used_offset + 2);
    q->used_ring =
        (volatile struct virtq_used_elem *)(memory + used_offset + 4);

    outl(io_base + REG_QUEUE_PFN, q->phys >> 12);
    return inl(io_base + REG_QUEUE_PFN) == (q->phys >> 12);
}

static int reset_device(void)
{
    outb(io_base + REG_STATUS, 0);
    for (int i = 0; i < TX_WAIT_SPINS; i++) {
        if (!inb(io_base + REG_STATUS))
            return 1;
        io_wait();
    }
    return 0;
}

static int init_failed(void)
{
    outb(io_base + REG_STATUS, STATUS_FAILED);
    if (reset_device()) {
        queue_release(&txq);
        queue_release(&rxq);
    } else {
        printk("[virtio-net] device did not reset; keeping DMA memory reserved\n");
    }
    return 0;
}

static void receive_packets(void)
{
    uint16_t completed = *rxq.used_idx;
    dma_barrier();
    while (rxq.used_next != completed) {
        uint16_t slot = rxq.used_next % rxq.size;
        uint32_t id = rxq.used_ring[slot].id;
        uint32_t len = rxq.used_ring[slot].len;
        rxq.used_next++;

        if (id >= rx_count)
            continue;
        if (len >= NET_HEADER_SIZE + ETH_HEADER_SIZE &&
            len <= RX_BUFFER_SIZE &&
            rx_buffers[id][0] == 0 && rx_buffers[id][1] == 0) {
            rx_packets++;
            net_rx(rx_buffers[id] + NET_HEADER_SIZE,
                   (uint16_t)(len - NET_HEADER_SIZE));
        }

        rxq.avail_ring[rxq.avail_next % rxq.size] = (uint16_t)id;
        rxq.avail_next++;
    }
    dma_barrier();
    *rxq.avail_idx = rxq.avail_next;
    outw(io_base + REG_QUEUE_NOTIFY, 0);
}

static void virtio_net_irq(struct regs *r)
{
    (void)r;
    uint8_t isr = inb(io_base + REG_ISR);
    if (present && (isr & 1))
        receive_packets();
}

int virtio_net_init(void)
{
    struct pci_dev dev;
    if (!pci_find_device(VIRTIO_VENDOR, VIRTIO_NET_LEGACY, &dev) &&
        !pci_find_device(VIRTIO_VENDOR, VIRTIO_NET_TRANSITIONAL, &dev))
        return 0;
    if (!(dev.bar0 & 1) || dev.irq == 0 || dev.irq >= 16)
        return 0;

    uint32_t base = dev.bar0 & ~3u;
    if (!base || base > 0xFFEFu)
        return 0;
    io_base = (uint16_t)base;
    pci_enable_bus_master(&dev);
    if (!reset_device())
        return 0;

    outb(io_base + REG_STATUS, STATUS_ACK);
    outb(io_base + REG_STATUS, STATUS_ACK | STATUS_DRIVER);
    uint32_t features = inl(io_base + REG_DEVICE_FEATURES);
    if (!(features & FEATURE_NET_MAC))
        return init_failed();
    outl(io_base + REG_GUEST_FEATURES, FEATURE_NET_MAC);
    for (int i = 0; i < 6; i++)
        mac[i] = inb(io_base + REG_DEVICE_CONFIG + i);

    if (!queue_init(&rxq, 0) || !queue_init(&txq, 1))
        return init_failed();

    rx_count = rxq.size < RX_BUFFER_COUNT ? rxq.size : RX_BUFFER_COUNT;
    for (uint16_t i = 0; i < rx_count; i++) {
        rxq.desc[i].addr = (uint64_t)(uintptr_t)rx_buffers[i];
        rxq.desc[i].len = RX_BUFFER_SIZE;
        rxq.desc[i].flags = DESC_F_WRITE;
        rxq.avail_ring[i] = i;
    }
    rxq.avail_next = rx_count;
    dma_barrier();
    *rxq.avail_idx = rxq.avail_next;

    irq_install_handler(dev.irq, virtio_net_irq);
    outb(io_base + REG_STATUS, STATUS_ACK | STATUS_DRIVER |
                                STATUS_DRIVER_OK);
    present = 1;
    pic_unmask(dev.irq);
    outw(io_base + REG_QUEUE_NOTIFY, 0);
    return 1;
}

int virtio_net_present(void)
{
    return present;
}

const uint8_t *virtio_net_mac(void)
{
    return mac;
}

int virtio_net_send(const void *frame, uint16_t len)
{
    if (!present || !frame || len > TX_BUFFER_SIZE - NET_HEADER_SIZE)
        return -1;

    irqflags_t flags = irq_save();
    int free = 0;
    for (int i = 0; i < TX_WAIT_SPINS; i++) {
        if (*txq.used_idx == txq.used_next) {
            free = 1;
            break;
        }
        io_wait();
    }
    if (!free) {
        irq_restore(flags);
        return -1;
    }

    memset(tx_buffer, 0, NET_HEADER_SIZE);
    memcpy(tx_buffer + NET_HEADER_SIZE, frame, len);
    txq.desc[0].addr = (uint64_t)(uintptr_t)tx_buffer;
    txq.desc[0].len = NET_HEADER_SIZE + len;
    txq.desc[0].flags = 0;
    txq.desc[0].next = 0;
    txq.avail_ring[txq.avail_next % txq.size] = 0;
    txq.avail_next++;
    dma_barrier();
    *txq.avail_idx = txq.avail_next;
    uint16_t expected = txq.used_next;
    outw(io_base + REG_QUEUE_NOTIFY, 1);

    for (int i = 0; i < TX_WAIT_SPINS; i++) {
        uint16_t completed = *txq.used_idx;
        if (completed != expected) {
            dma_barrier();
            txq.used_next = completed;
            tx_packets++;
            irq_restore(flags);
            return 0;
        }
        io_wait();
    }
    irq_restore(flags);
    return -1;
}

uint32_t virtio_net_rx_packets(void)
{
    return rx_packets;
}

uint32_t virtio_net_tx_packets(void)
{
    return tx_packets;
}
