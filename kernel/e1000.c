/* Driver for the Intel 82540EM card (e1000, PCI)
 *
 * A generation newer than the RTL8139 and conceived completely
 * differently. The registers do not sit in I/O ports but in memory
 * (BAR0), so they first have to be glued onto the mapping - they lie
 * high, beyond what the kernel described at startup.
 *
 * Traffic goes through two RINGS OF DESCRIPTORS, one per direction. A
 * descriptor is sixteen bytes: a buffer address, a length and a status
 * field. Who owns a descriptor follows from the head and tail
 * pointers - the card reads from the head (RDH/TDH) to the tail
 * (RDT/TDT), and we move the tail, handing it the next slots. This is
 * more convenient than the RTL8139's ring of bytes in that packets do
 * not stick together and do not have to be copied from the end of the
 * buffer back to its start.
 *
 * We read the MAC address from EEPROM through the EERD register; when
 * the card will not give it up from there (which happens in virtual
 * machines), we take it from the address filter register, which the
 * firmware has usually already filled in.
 */

#include <stdint.h>
#include "include/e1000.h"
#include "include/pci.h"
#include "include/io.h"
#include "include/isr.h"
#include "include/pic.h"
#include "include/printk.h"
#include "include/string.h"
#include "include/net.h"

/* The registers lie in memory, and that is described by different code
   in each kernel - the only thing in this file that depends on the
   processor mode */
#ifdef __x86_64__
#include "../kernel64/paging64.h"
#define MAP_PHYS(a, n)   paging64_map_phys((a), (n))
#else
#include "include/paging.h"
#define MAP_PHYS(a, n)   paging_map_phys((a), (n))
#endif

#define E1000_VENDOR 0x8086
#define E1000_DEVICE 0x100E        /* 82540EM - the one QEMU pretends to be */

/* Registers (offsets from the base in memory) */
#define REG_CTRL     0x0000
#define REG_STATUS   0x0008
#define REG_EERD     0x0014
#define REG_ICR      0x00C0
#define REG_IMS      0x00D0
#define REG_IMC      0x00D8
#define REG_RCTL     0x0100
#define REG_TCTL     0x0400
#define REG_TIPG     0x0410
#define REG_RDBAL    0x2800
#define REG_RDBAH    0x2804
#define REG_RDLEN    0x2808
#define REG_RDH      0x2810
#define REG_RDT      0x2818
#define REG_TDBAL    0x3800
#define REG_TDBAH    0x3804
#define REG_TDLEN    0x3808
#define REG_TDH      0x3810
#define REG_TDT      0x3818
#define REG_MTA      0x5200        /* the multicast filter table (128 words) */
#define REG_RAL      0x5400
#define REG_RAH      0x5404

#define CTRL_SLU     (1u << 6)     /* bring the link up */
#define CTRL_ASDE    (1u << 5)     /* automatic speed detection */
#define CTRL_RST     (1u << 26)

#define RCTL_EN      (1u << 1)
#define RCTL_BAM     (1u << 15)    /* accept broadcasts */
#define RCTL_SECRC   (1u << 26)    /* strip the CRC - we do not want it anyway */

#define TCTL_EN      (1u << 1)
#define TCTL_PSP     (1u << 3)     /* pad short frames */

#define TXD_CMD_EOP  (1u << 0)     /* end of packet */
#define TXD_CMD_IFCS (1u << 1)     /* add the CRC */
#define TXD_CMD_RS   (1u << 3)     /* report when finished */
#define TXD_STA_DD   (1u << 0)     /* the card handed the descriptor back */

#define RXD_STA_DD   (1u << 0)
#define RXD_STA_EOP  (1u << 1)

#define ICR_RXT0     (1u << 7)     /* frames have arrived */

#define RX_DESC      32
#define TX_DESC      16
#define BUF_SIZE     2048

/* The descriptors have a field layout fixed by the hardware - it is
   the card that reads and writes them, not us */
struct rx_desc {
    uint64_t addr;
    uint16_t len;
    uint16_t sum;
    uint8_t status;
    uint8_t errors;
    uint16_t special;
} __attribute__((packed));

struct tx_desc {
    uint64_t addr;
    uint16_t len;
    uint8_t cso;
    uint8_t command;
    uint8_t status;
    uint8_t css;
    uint16_t special;
} __attribute__((packed));

/* The rings and buffers live in .bss: the kernel sits in the identity
   mapping, so their virtual address is also the physical one we hand
   to the card. The sixteen-byte alignment is required by the
   hardware. */
static struct rx_desc rx_ring[RX_DESC] __attribute__((aligned(16)));
static struct tx_desc tx_ring[TX_DESC] __attribute__((aligned(16)));
static uint8_t rx_bufs[RX_DESC][BUF_SIZE] __attribute__((aligned(16)));
static uint8_t tx_bufs[TX_DESC][BUF_SIZE] __attribute__((aligned(16)));

static volatile uint8_t *base;
static int present;
static uint8_t mac[6];
static uint32_t rx_packets, tx_packets;
static uint32_t rx_current;        /* which descriptor we are looking at */
static uint32_t tx_next;

static void wr(uint32_t reg, uint32_t value)
{
    *(volatile uint32_t *)(base + reg) = value;
}

static uint32_t rd(uint32_t reg)
{
    return *(volatile uint32_t *)(base + reg);
}

int e1000_present(void)          { return present; }
const uint8_t *e1000_mac(void)   { return mac; }
uint32_t e1000_rx_packets(void)  { return rx_packets; }
uint32_t e1000_tx_packets(void)  { return tx_packets; }

/* A word from the EEPROM. Returns -1 when the card does not answer -
   some virtual machines do not emulate that memory at all. */
static int32_t eeprom(uint8_t addr)
{
    wr(REG_EERD, ((uint32_t)addr << 8) | 1);
    for (int i = 0; i < 100000; i++) {
        uint32_t v = rd(REG_EERD);
        if (v & (1u << 4))                     /* the ready bit */
            return (int32_t)((v >> 16) & 0xFFFF);
        io_wait();
    }
    return -1;
}

static void read_mac(void)
{
    int32_t w0 = eeprom(0);
    if (w0 >= 0) {
        int32_t w1 = eeprom(1), w2 = eeprom(2);
        if (w1 >= 0 && w2 >= 0) {
            mac[0] = (uint8_t)w0;
            mac[1] = (uint8_t)(w0 >> 8);
            mac[2] = (uint8_t)w1;
            mac[3] = (uint8_t)(w1 >> 8);
            mac[4] = (uint8_t)w2;
            mac[5] = (uint8_t)(w2 >> 8);
            return;
        }
    }

    /* Without an EEPROM the address filter is what is left - the
       firmware usually writes the card's address there before we get
       to it */
    uint32_t low = rd(REG_RAL), high = rd(REG_RAH);
    mac[0] = (uint8_t)low;
    mac[1] = (uint8_t)(low >> 8);
    mac[2] = (uint8_t)(low >> 16);
    mac[3] = (uint8_t)(low >> 24);
    mac[4] = (uint8_t)high;
    mac[5] = (uint8_t)(high >> 8);
}

int e1000_send(const void *frame, uint16_t len)
{
    if (!present || len > BUF_SIZE)
        return -1;

    uint32_t flags = irq_save();
    uint32_t i = tx_next;

    /* A descriptor is ours once the card has handed it back with the DD
       bit. A fresh, never used one has a status of zero and is free
       too - which is why we wait only when we put something in it. */
    if (tx_ring[i].len && !(tx_ring[i].status & TXD_STA_DD)) {
        int free_one = 0;
        for (int spin = 0; spin < 200000; spin++) {
            if (tx_ring[i].status & TXD_STA_DD) {
                free_one = 1;
                break;
            }
            io_wait();
        }
        if (!free_one) {
            irq_restore(flags);
            return -1;
        }
    }

    memcpy(tx_bufs[i], frame, len);
    tx_ring[i].addr = (uint64_t)(uintptr_t)tx_bufs[i];
    tx_ring[i].len = len;
    tx_ring[i].cso = 0;
    tx_ring[i].css = 0;
    tx_ring[i].special = 0;
    tx_ring[i].status = 0;
    /* Padding short frames and adding the CRC is the card's job - hence
       PSP in TCTL and the IFCS bit here */
    tx_ring[i].command = TXD_CMD_EOP | TXD_CMD_IFCS | TXD_CMD_RS;

    tx_next = (i + 1) % TX_DESC;
    wr(REG_TDT, tx_next);          /* moving the tail = "go" */
    tx_packets++;

    irq_restore(flags);
    return 0;
}

static void e1000_receive(void)
{
    while (rx_ring[rx_current].status & RXD_STA_DD) {
        struct rx_desc *d = &rx_ring[rx_current];
        uint16_t len = d->len;

        if ((d->status & RXD_STA_EOP) && !d->errors && len && len <= BUF_SIZE) {
            rx_packets++;
            net_rx(rx_bufs[rx_current], len);
        }

        d->status = 0;                 /* the descriptor is ours again */
        /* The tail points at the LAST one handed to the card, so it is
           always one behind the one we are looking at */
        wr(REG_RDT, rx_current);
        rx_current = (rx_current + 1) % RX_DESC;
    }
}

static void e1000_irq(struct regs *r)
{
    (void)r;
    uint32_t icr = rd(REG_ICR);        /* reading clears the notifications */
    if (icr & ICR_RXT0)
        e1000_receive();
}

int e1000_init(void)
{
    struct pci_dev dev;
    if (!pci_find_device(E1000_VENDOR, E1000_DEVICE, &dev))
        return 0;
    if (dev.bar0 & 0x01)               /* we expect a memory BAR */
        return 0;

    uint32_t mmio = dev.bar0 & ~0xFu;
    if (!mmio || MAP_PHYS(mmio, 0x20000) < 0)
        return 0;

    pci_enable_bus_master(&dev);
    base = (volatile uint8_t *)(uintptr_t)mmio;

    wr(REG_IMC, 0xFFFFFFFF);           /* silence while we set things up */
    wr(REG_CTRL, rd(REG_CTRL) | CTRL_RST);
    for (int i = 0; i < 100000 && (rd(REG_CTRL) & CTRL_RST); i++)
        io_wait();
    if (rd(REG_CTRL) & CTRL_RST)
        return 0;                      /* the card did not come up */
    wr(REG_IMC, 0xFFFFFFFF);           /* the reset will unmask interrupts */

    wr(REG_CTRL, rd(REG_CTRL) | CTRL_SLU | CTRL_ASDE);

    read_mac();

    /* The multicast filter to zero: we want nobody's group */
    for (int i = 0; i < 128; i++)
        wr(REG_MTA + i * 4, 0);

    /* Our own address into the filter, with the valid bit (31) */
    wr(REG_RAL, (uint32_t)mac[0] | ((uint32_t)mac[1] << 8) |
                ((uint32_t)mac[2] << 16) | ((uint32_t)mac[3] << 24));
    wr(REG_RAH, (uint32_t)mac[4] | ((uint32_t)mac[5] << 8) | (1u << 31));

    /* The receive ring: every descriptor gets a buffer at once and is
       handed to the card. The tail on the last one - they are all
       hers. */
    for (int i = 0; i < RX_DESC; i++) {
        rx_ring[i].addr = (uint64_t)(uintptr_t)rx_bufs[i];
        rx_ring[i].status = 0;
    }
    wr(REG_RDBAL, (uint32_t)(uintptr_t)rx_ring);
    wr(REG_RDBAH, 0);
    wr(REG_RDLEN, RX_DESC * (uint32_t)sizeof(struct rx_desc));
    wr(REG_RDH, 0);
    wr(REG_RDT, RX_DESC - 1);
    rx_current = 0;
    wr(REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);   /* a 2 KiB buffer = 00 */

    /* The transmit ring: empty, the tail equal to the head */
    memset(tx_ring, 0, sizeof(tx_ring));
    wr(REG_TDBAL, (uint32_t)(uintptr_t)tx_ring);
    wr(REG_TDBAH, 0);
    wr(REG_TDLEN, TX_DESC * (uint32_t)sizeof(struct tx_desc));
    wr(REG_TDH, 0);
    wr(REG_TDT, 0);
    tx_next = 0;
    /* The collision threshold and the inter-frame gap - values from the
       card's documentation for full duplex */
    wr(REG_TCTL, TCTL_EN | TCTL_PSP | (0x0F << 4) | (0x40 << 12));
    wr(REG_TIPG, 0x0060200A);

    wr(REG_IMS, ICR_RXT0);
    rd(REG_ICR);                       /* clear whatever has already piled up */

    irq_install_handler(dev.irq, e1000_irq);
    pic_unmask(dev.irq);

    present = 1;
    return 1;
}
