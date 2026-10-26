/* Driver for the Realtek RTL8111/8168 gigabit card (PCIe)
 *
 * The registers are reachable two ways, through I/O ports (BAR0) and
 * through memory (BAR2); the ports need no mapping and are the same in
 * both kernels, so we take them, as for the RTL8139.
 *
 * Traffic goes through two rings of sixteen-byte descriptors. Unlike
 * the e1000's, a descriptor says itself who holds it: the OWN bit, set
 * by us to hand it to the card and cleared by the card when it is done.
 * The ring has no length register either - the last descriptor carries
 * EOR, "end of ring", and the card goes back to the first after it.
 * Sending is a write to TPPoll: "look at the ring again".
 *
 * The family is large - a chip version in TxConfig tells its members
 * apart, and Linux keeps pages of per-version fixes. Of those, two
 * matter to a card that is merely to send and receive: from the 8168G
 * on, receiving is gated (RXDV_GATED_EN in MISC) until the driver
 * opens it, and the PHY may have been left powered down by whoever had
 * the card before - firmware, or another system shutting down.
 *
 * Receiving is polled, by a thread of the driver's: the interrupt of a
 * PCIe card reaches the 8259 only if the machine's firmware routed it
 * there, and on a UEFI laptop nothing promises that.
 */

#include <stdint.h>
#include "include/rtl8168.h"
#include "include/pci.h"
#include "include/io.h"
#include "include/printk.h"
#include "include/string.h"
#include "include/net.h"
#include "include/task.h"

#define RTL_VENDOR      0x10EC

/* Registers (offsets from the I/O base) */
#define REG_IDR0        0x00       /* the MAC address, six bytes */
#define REG_MAR0        0x08       /* the multicast filter, eight bytes */
#define REG_TNPDS       0x20       /* where the transmit ring lies (64 bits) */
#define REG_CMD         0x37
#define REG_TPPOLL      0x38
#define REG_IMR         0x3C
#define REG_ISR         0x3E
#define REG_TCR         0x40       /* TxConfig; its top bits: the chip version */
#define REG_RCR         0x44       /* RxConfig */
#define REG_9346CR      0x50       /* unlocks the configuration registers */
#define REG_PHYAR       0x60       /* the PHY's registers, before the 8168G */
#define REG_PHYSTATUS   0x6C
#define REG_PMCH        0x6F       /* power of the PLL, from the 8168G on */
#define REG_GPHY_OCP    0xB8       /* the PHY's registers, from the 8168G on */
#define REG_RMS         0xDA       /* the largest frame it is to take in */
#define REG_CPCR        0xE0       /* C+ command */
#define REG_RDSAR       0xE4       /* where the receive ring lies (64 bits) */
#define REG_MTPS        0xEC       /* the largest frame to send, in 128 B */
#define REG_MISC        0xF0

#define CMD_RESET       0x10
#define CMD_RX_ENABLE   0x08
#define CMD_TX_ENABLE   0x04
#define TPPOLL_NPQ      0x40       /* the normal priority ring has work */

#define RCR_AAP         0x01       /* every frame (we do not want that) */
#define RCR_APM         0x02       /* frames to our address */
#define RCR_AM          0x04       /* multicast */
#define RCR_AB          0x08       /* broadcast */
/* How it is to take frames in, by generation (Linux's rtl_init_rxcfg):
   no early threshold and unlimited DMA bursts on the older chips; on
   the 8168G and later, 128-byte interrupts, several frames per DMA,
   unlimited bursts and early receive off */
#define RCR_OLD         ((7u << 13) | (7u << 8))
#define RCR_G           ((1u << 15) | (1u << 14) | (1u << 11) | (7u << 8))
/* The gap between frames the standard asks for (96 bit times), and
   unlimited DMA bursts; from the 8168G on, the FIFO sized by the chip */
#define TCR_IFG_DMA     ((3u << 24) | (7u << 8))
#define TCR_AUTO_FIFO   (1u << 7)

#define MISC_RXDV_GATED (1u << 19)
#define PHYSTATUS_LINK  0x02
#define PHYSTATUS_1000  0x10
#define PHYSTATUS_100   0x08
#define PHYSTATUS_10    0x04
#define PHYSTATUS_FULL  0x01

/* The first word of a descriptor */
#define DESC_OWN        (1u << 31) /* the card's */
#define DESC_EOR        (1u << 30) /* the last in the ring */
#define DESC_FS         (1u << 29) /* the first piece of a frame */
#define DESC_LS         (1u << 28) /* the last piece */
#define RX_RES          (1u << 21) /* something was wrong with the frame */
#define DESC_LEN        0x3FFF

#define RX_DESC         64
#define TX_DESC         32
#define BUF_SIZE        2048
#define MIN_FRAME       60         /* without the CRC, which the card adds */

struct desc {
    uint32_t opts1;
    uint32_t opts2;
    uint64_t addr;
} __attribute__((packed));

/* The card wants each ring on a 256-byte boundary. The kernel sits in
   the identity mapping, so an address in .bss is also the physical one
   the card is given. */
static struct desc rx_ring[RX_DESC] __attribute__((aligned(256)));
static struct desc tx_ring[TX_DESC] __attribute__((aligned(256)));
static uint8_t rx_bufs[RX_DESC][BUF_SIZE] __attribute__((aligned(16)));
static uint8_t tx_bufs[TX_DESC][BUF_SIZE] __attribute__((aligned(16)));

static uint16_t io;
static int present;
static uint8_t mac[6];
static uint32_t rx_packets, tx_packets;
static uint32_t rx_current, tx_next;
static uint32_t xid;               /* the chip version, from TxConfig */

int rtl8168_present(void)          { return present; }
int rtl8168_link(void)             { return (inb(io + REG_PHYSTATUS) & PHYSTATUS_LINK) != 0; }
const uint8_t *rtl8168_mac(void)   { return mac; }
uint32_t rtl8168_rx_packets(void)  { return rx_packets; }
uint32_t rtl8168_tx_packets(void)  { return tx_packets; }

/* The 8168G and everything after it (8168EP, 8168H): versions whose
   PHY sits behind OCP and whose receiving starts out gated. The same
   list as Linux's RTL_GIGA_MAC_VER_40 and up. */
static int is_8168g_or_later(void)
{
    static const uint16_t later[] = { 0x4c0, 0x4c1, 0x509, 0x5c8, 0x500,
                                      0x501, 0x502, 0x541, 0x6c0 };
    for (unsigned i = 0; i < sizeof(later) / sizeof(later[0]); i++)
        if ((xid & 0x7cf) == later[i])
            return 1;
    return (xid & 0x7c8) == 0x540;     /* the rest of the 8168H's */
}

/* The PHY's registers: through PHYAR, or from the 8168G on through OCP,
   where its standard registers lie at 0xA400, two bytes apart (the
   address goes in halved). Bit 31 is the handshake: set by whoever
   starts a write, set by the card when a read is done. */
static uint16_t phy_read(int reg)
{
    uint16_t port = is_8168g_or_later() ? REG_GPHY_OCP : REG_PHYAR;
    uint32_t at = is_8168g_or_later() ? (0xA400 + (uint32_t)reg * 2) >> 1
                                      : (uint32_t)(reg & 0x1F);
    outl(io + port, at << 16);
    for (int i = 0; i < 10000; i++) {
        uint32_t v = inl(io + port);
        if (v & (1u << 31))
            return (uint16_t)v;
        io_wait();
    }
    return 0xFFFF;
}

static void phy_write(int reg, uint16_t value)
{
    if (is_8168g_or_later()) {
        uint32_t at = 0xA400 + (uint32_t)reg * 2;
        outl(io + REG_GPHY_OCP, (1u << 31) | ((at >> 1) << 16) | value);
        for (int i = 0; i < 10000 && (inl(io + REG_GPHY_OCP) & (1u << 31)); i++)
            io_wait();
    } else {
        outl(io + REG_PHYAR, (1u << 31) | ((uint32_t)(reg & 0x1F) << 16) | value);
        for (int i = 0; i < 10000 && (inl(io + REG_PHYAR) & (1u << 31)); i++)
            io_wait();
    }
}

/* Takes in what the card has put in the receive ring; how many frames */
int rtl8168_poll(void)
{
    if (!present)
        return 0;
    int n = 0;
    /* (one turn of the ring at most: a card that hands nothing back
       must not keep the thread here) */
    for (int k = 0; k < RX_DESC && !(rx_ring[rx_current].opts1 & DESC_OWN); k++) {
        struct desc *d = &rx_ring[rx_current];
        uint32_t s = d->opts1;
        /* (the length includes the CRC; a frame in pieces would have
           to have outgrown a buffer, and RMS keeps that from happening) */
        uint32_t len = s & DESC_LEN;
        if ((s & DESC_FS) && (s & DESC_LS) && !(s & RX_RES) &&
            len > 4 && len <= BUF_SIZE) {
            rx_packets++;
            net_rx(rx_bufs[rx_current], (uint16_t)(len - 4));
            n++;
        }
        /* Back to the card, as it was given at the start */
        d->opts2 = 0;
        d->opts1 = DESC_OWN | (rx_current == RX_DESC - 1 ? DESC_EOR : 0) |
                   BUF_SIZE;
        rx_current = (rx_current + 1) % RX_DESC;
    }
    return n;
}

int rtl8168_send(const void *frame, uint16_t len)
{
    if (!present || len > BUF_SIZE)
        return -1;
    irqflags_t flags = irq_save();
    uint32_t i = tx_next;
    /* Still the card's: it has not got through the ring yet */
    for (int spin = 0; (tx_ring[i].opts1 & DESC_OWN) && spin < 200000; spin++)
        io_wait();
    if (tx_ring[i].opts1 & DESC_OWN) {
        irq_restore(flags);
        return -1;
    }
    memcpy(tx_bufs[i], frame, len);
    /* Short frames padded here: some of the family send them short */
    if (len < MIN_FRAME) {
        memset(tx_bufs[i] + len, 0, MIN_FRAME - len);
        len = MIN_FRAME;
    }
    tx_ring[i].addr = (uint64_t)(uintptr_t)tx_bufs[i];
    tx_ring[i].opts2 = 0;
    tx_ring[i].opts1 = DESC_OWN | DESC_FS | DESC_LS |
                       (i == TX_DESC - 1 ? DESC_EOR : 0) | len;
    tx_next = (i + 1) % TX_DESC;
    outb(io + REG_TPPOLL, TPPOLL_NPQ);
    tx_packets++;
    irq_restore(flags);
    return 0;
}

static void rx_thread(void *arg)
{
    (void)arg;
    for (;;) {
        if (!rtl8168_poll())
            task_sleep(2);         /* nothing came - do not spin */
    }
}

static int find_card(struct pci_dev *dev)
{
    /* 8168 is the PCIe chip; 8161 the same one under another number */
    return pci_find_device(RTL_VENDOR, 0x8168, dev) ||
           pci_find_device(RTL_VENDOR, 0x8161, dev);
}

int rtl8168_init(void)
{
    struct pci_dev dev;
    if (!find_card(&dev) || !(dev.bar[0] & 1))
        return 0;
    io = (uint16_t)(dev.bar[0] & ~3u);
    pci_enable_bus_master(&dev);

    xid = (inl(io + REG_TCR) >> 20) & 0x7cf;
    if (is_8168g_or_later())
        outb(io + REG_PMCH, inb(io + REG_PMCH) | 0xC0);   /* the PLL up */

    outb(io + REG_CMD, CMD_RESET);
    for (int i = 0; i < 100000 && (inb(io + REG_CMD) & CMD_RESET); i++)
        io_wait();
    if (inb(io + REG_CMD) & CMD_RESET)
        return 0;                      /* the card did not come back */
    outw(io + REG_IMR, 0);             /* polled: no interrupts */
    outw(io + REG_ISR, 0xFFFF);

    for (int i = 0; i < 6; i++)
        mac[i] = inb(io + REG_IDR0 + i);

    /* A PHY left powered down (BMCR bit 11) is powered up and asked to
       find the best the other end can do - auto-negotiation, restarted.
       One that is up is left alone: a restart would cost the link for
       seconds. */
    uint16_t bmcr = phy_read(0);
    if (bmcr != 0xFFFF && (bmcr & 0x0800))
        phy_write(0, (uint16_t)((bmcr & ~0x0800) | 0x1200));

    if (is_8168g_or_later())
        outl(io + REG_MISC, inl(io + REG_MISC) & ~MISC_RXDV_GATED);

    /* Every receive descriptor gets its buffer and goes to the card at
       once; nothing is sent yet, so the transmit ring is all ours */
    for (int i = 0; i < RX_DESC; i++) {
        rx_ring[i].addr = (uint64_t)(uintptr_t)rx_bufs[i];
        rx_ring[i].opts2 = 0;
        rx_ring[i].opts1 = DESC_OWN | (i == RX_DESC - 1 ? DESC_EOR : 0) |
                           BUF_SIZE;
    }
    memset(tx_ring, 0, sizeof(tx_ring));
    rx_current = tx_next = 0;

    outb(io + REG_9346CR, 0xC0);
    outw(io + REG_RMS, BUF_SIZE);
    outb(io + REG_MTPS, 0x3F);
    uint64_t rx = (uint64_t)(uintptr_t)rx_ring, tx = (uint64_t)(uintptr_t)tx_ring;
    outl(io + REG_TNPDS, (uint32_t)tx);
    outl(io + REG_TNPDS + 4, (uint32_t)(tx >> 32));
    outl(io + REG_RDSAR, (uint32_t)rx);
    outl(io + REG_RDSAR + 4, (uint32_t)(rx >> 32));
    outb(io + REG_9346CR, 0x00);

    /* As Linux does it: the ring on first, then what it is to take */
    outb(io + REG_CMD, CMD_RX_ENABLE | CMD_TX_ENABLE);
    int g = is_8168g_or_later();
    outl(io + REG_TCR, TCR_IFG_DMA | (g ? TCR_AUTO_FIFO : 0));
    outl(io + REG_RCR, (g ? RCR_G : RCR_OLD) | RCR_AB | RCR_APM | RCR_AM);
    /* No multicast group of anyone's: all of the filter zero */
    outl(io + REG_MAR0, 0);
    outl(io + REG_MAR0 + 4, 0);

    present = 1;
    uint8_t st = inb(io + REG_PHYSTATUS);
    printk("[ OK ] RTL8168 (chip %x): %x:%x:%x:%x:%x:%x, link %s\n",
           xid, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
           !(st & PHYSTATUS_LINK) ? "down (yet)" :
           st & PHYSTATUS_1000 ? "1000 Mb/s" :
           st & PHYSTATUS_100 ? "100 Mb/s" : "10 Mb/s");
    task_create("rtl8168", rx_thread, 0);
    return 1;
}
