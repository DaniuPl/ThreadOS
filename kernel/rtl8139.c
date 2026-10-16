/* Driver for the Realtek RTL8139 network card (PCI)
 *
 * The card is "old school": it is configured through I/O ports (BAR0),
 * while packets travel by DMA to buffers in kernel memory. Because
 * ThreadOS has an identity mapping, a buffer's virtual address is also
 * the physical address we hand to the card.
 *
 * Reception: one contiguous ring buffer (8 KiB + 16 B + 1500 B - that
 * much slack is required by WRAP mode, in which the card may write the
 * end of a packet past the end of the ring). Every packet is preceded
 * by a 4-byte header: 2 B of status and 2 B of length (including the
 * CRC). CAPR tells the card how far we have read.
 *
 * Transmission: 4 descriptors (TSAD0-3 / TSD0-3) used in turn; each
 * has its own 2 KiB buffer. The OWN bit in TSD clears once the card
 * has finished sending.
 */

#include "include/rtl8139.h"
#include "include/pci.h"
#include "include/io.h"
#include "include/isr.h"
#include "include/pic.h"
#include "include/printk.h"
#include "include/string.h"
#include "include/net.h"

#define RTL_VENDOR 0x10EC
#define RTL_DEVICE 0x8139

/* Registers (offsets from the I/O base) */
#define REG_MAC0      0x00
#define REG_TSD0      0x10   /* TX descriptor status/length (4 x 4 B) */
#define REG_TSAD0     0x20   /* TX buffer address (4 x 4 B) */
#define REG_RBSTART   0x30   /* the RX buffer address */
#define REG_CMD       0x37
#define REG_CAPR      0x38   /* how far we have read the RX ring */
#define REG_IMR       0x3C
#define REG_ISR       0x3E
#define REG_TCR       0x40
#define REG_RCR       0x44
#define REG_CONFIG1   0x52

#define CMD_RESET     0x10
#define CMD_RX_ENABLE 0x08
#define CMD_TX_ENABLE 0x04
#define CMD_BUF_EMPTY 0x01

#define ISR_ROK       0x0001
#define ISR_TOK       0x0004

#define RX_BUF_SIZE   8192
#define RX_BUF_PAD    16
#define RX_BUF_WRAP   1500
#define TX_DESCS      4
#define TX_BUF_SIZE   2048

#define TSD_OWN       0x2000

static uint16_t io_base;
static int present;
static uint8_t mac[6];
static uint32_t rx_packets, tx_packets;

/* DMA buffers in .bss - the kernel sits in the identity mapping, so
   their virtual address == the physical one. */
static uint8_t rx_buffer[RX_BUF_SIZE + RX_BUF_PAD + RX_BUF_WRAP]
    __attribute__((aligned(16)));
static uint8_t tx_buffer[TX_DESCS][TX_BUF_SIZE] __attribute__((aligned(16)));

static uint16_t rx_offset;     /* the read position in the RX ring */
static int tx_next;            /* the next TX descriptor to use */
static uint8_t tx_used[TX_DESCS];  /* has the descriptor been used before */

/* Scratch for a packet glued from the end and the start of the ring */
static uint8_t rx_scratch[2048];

int rtl8139_present(void)
{
    return present;
}

const uint8_t *rtl8139_mac(void)
{
    return mac;
}

uint32_t rtl8139_rx_packets(void)
{
    return rx_packets;
}

uint32_t rtl8139_tx_packets(void)
{
    return tx_packets;
}

int rtl8139_send(const void *frame, uint16_t len)
{
    if (!present || len > TX_BUF_SIZE)
        return -1;

    uint32_t flags = irq_save();

    int desc = tx_next;
    /* A descriptor is free once the card has handed it back (OWN=1).
       A fresh, never used descriptor is nothing to wait for. */
    if (tx_used[desc]) {
        int free = 0;
        for (int spin = 0; spin < 200000; spin++) {
            if (inl(io_base + REG_TSD0 + desc * 4) & TSD_OWN) {
                free = 1;
                break;
            }
        }
        if (!free) {
            irq_restore(flags);
            return -1;
        }
    }

    memcpy(tx_buffer[desc], frame, len);
    /* Ethernet requires a minimum of 60 B (without the CRC) - we pad with
   zeros */
    uint16_t sent = len;
    if (sent < 60) {
        memset(tx_buffer[desc] + sent, 0, 60 - sent);
        sent = 60;
    }

    outl(io_base + REG_TSAD0 + desc * 4,
         (uint32_t)(uintptr_t)tx_buffer[desc]);
    outl(io_base + REG_TSD0 + desc * 4, sent);   /* the write clears OWN -> go */

    tx_used[desc] = 1;
    tx_next = (desc + 1) % TX_DESCS;
    tx_packets++;

    irq_restore(flags);
    return 0;
}

static void rtl8139_receive(void)
{
    while (!(inb(io_base + REG_CMD) & CMD_BUF_EMPTY)) {
        uint8_t *p = rx_buffer + rx_offset;
        uint16_t status = (uint16_t)(p[0] | (p[1] << 8));
        uint16_t len = (uint16_t)(p[2] | (p[3] << 8));   /* including the CRC */

        if (len < 4 || len > RX_BUF_WRAP + 18)
            break;                       /* rubbish - better to back out */

        uint16_t payload = len - 4;      /* we cut the CRC off */
        if ((status & 0x01) && payload <= sizeof(rx_scratch)) {
            memcpy(rx_scratch, p + 4, payload);
            rx_packets++;
            net_rx(rx_scratch, payload);
        }

        /* The next packet starts after alignment to 4 bytes */
        rx_offset = (uint16_t)((rx_offset + len + 4 + 3) & ~3u);
        rx_offset %= RX_BUF_SIZE;
        outw(io_base + REG_CAPR, (uint16_t)(rx_offset - RX_BUF_PAD));
    }
}

static void rtl8139_irq(struct regs *r)
{
    (void)r;
    uint16_t isr = inw(io_base + REG_ISR);
    if (!isr)
        return;
    outw(io_base + REG_ISR, isr);       /* acknowledge the interrupt */

    if (isr & ISR_ROK)
        rtl8139_receive();
}

int rtl8139_init(void)
{
    struct pci_dev dev;
    if (!pci_find_device(RTL_VENDOR, RTL_DEVICE, &dev))
        return 0;
    if (!(dev.bar0 & 0x01))             /* we expect an I/O port BAR */
        return 0;

    pci_enable_bus_master(&dev);
    io_base = (uint16_t)(dev.bar0 & 0xFFFC);

    outb(io_base + REG_CONFIG1, 0x00);  /* wake the card up */

    outb(io_base + REG_CMD, CMD_RESET);
    for (int i = 0; i < 1000000; i++)
        if (!(inb(io_base + REG_CMD) & CMD_RESET))
            break;
    if (inb(io_base + REG_CMD) & CMD_RESET)
        return 0;                       /* the card did not come up */

    for (int i = 0; i < 6; i++)
        mac[i] = inb(io_base + REG_MAC0 + i);

    rx_offset = 0;
    tx_next = 0;
    memset(tx_used, 0, sizeof(tx_used));
    /* The card addresses memory with 32 bits. The buffers lie in the
       kernel image, that is just above one megabyte, so the address
       fits with room to spare - but the cast has to be explicit,
       because in long mode a pointer is twice as wide as what the card
       can accept. */
    outl(io_base + REG_RBSTART, (uint32_t)(uintptr_t)rx_buffer);

    /* AB|AM|APM|AAP (accept broadcast, multicast, our own and other
       frames), WRAP, the maximum DMA burst (bits 8-10) and the "whole
       packet" FIFO threshold (bits 13-15); bits 11-12 = 00 -> an 8 KiB
       ring */
    outl(io_base + REG_RCR, 0x0F | (1 << 7) | (7 << 8) | (7 << 13));
    outl(io_base + REG_TCR, 0x03000700); /* the default IFG plus a 16 B DMA burst */

    outw(io_base + REG_IMR, ISR_ROK | ISR_TOK);
    outb(io_base + REG_CMD, CMD_RX_ENABLE | CMD_TX_ENABLE);

    irq_install_handler(dev.irq, rtl8139_irq);
    pic_unmask(dev.irq);

    present = 1;
    return 1;
}
