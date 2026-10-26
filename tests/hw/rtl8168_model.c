/* The RTL8168 driver against a model of the chip, on the host
 *
 * No emulator has this card, so the card is written here instead -
 * from Realtek's description of it and what Linux's r8169 relies on,
 * not from our driver - and the driver (kernel/rtl8168.c, compiled
 * as it is) is run against it:
 *
 *   registers behind I/O ports; a reset that takes a few reads to end;
 *   the descriptor addresses and size limits taken only while the
 *   configuration registers are unlocked (9346CR);
 *   two rings of descriptors in the host's memory, with OWN and EOR;
 *   TPPoll: the card takes the frames the driver handed it, in order;
 *   frames from the wire go into the receive ring only where the
 *   driver handed a descriptor back, and only as RxConfig allows -
 *   broadcast, our address, never another's;
 *   the PHY behind PHYAR on the older chips and behind OCP from the
 *   8168G on (each ignores the other road), possibly powered down;
 *   from the 8168G on, receiving gated until MISC says otherwise.
 *
 * Usage: rtl8168_model <chip version, hex> <initial BMCR, hex>
 * Prints "ok: ..." or "FAIL: ..." per check.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "include/pci.h"
#include "include/rtl8168.h"

#define IO_BASE 0xC000

/* --- the card ---------------------------------------------------------- */

static uint8_t regs[256];
static uint32_t xid;
static int g_family;
static int unlocked;
static int reset_reads;            /* reads left before the reset ends */
static uint16_t bmcr;
static int phy_writes;
static uint32_t phy_reply;         /* what PHYAR/OCP read back */
static unsigned rx_at, tx_at;      /* where the card is in each ring */
static int missed, filtered;

static const uint8_t card_mac[6] = { 0x00, 0xE0, 0x4C, 0x68, 0x01, 0x02 };

struct desc {
    uint32_t opts1, opts2;
    uint64_t addr;
};

static uint32_t reg32(int r)
{
    return regs[r] | regs[r + 1] << 8 | regs[r + 2] << 16 |
           (uint32_t)regs[r + 3] << 24;
}

static struct desc *ring(int r)
{
    uint64_t a = reg32(r) | (uint64_t)reg32(r + 4) << 32;
    return (struct desc *)(uintptr_t)a;
}

static int phy_up(void)
{
    return !(bmcr & 0x0800);
}

/* What went out on the wire */
static uint8_t wire[512][2048];
static uint16_t wire_len[512];
static int wire_n;

static void transmit(void)
{
    if (!(regs[0x37] & 0x04))
        return;                        /* the transmitter is off */
    struct desc *t = ring(0x20);
    for (;;) {
        struct desc *d = &t[tx_at];
        if (!(d->opts1 & (1u << 31)))
            break;
        uint32_t len = d->opts1 & 0x3FFF;
        int whole = (d->opts1 & (3u << 28)) == (3u << 28);
        if (whole && phy_up() && wire_n < 512) {
            memcpy(wire[wire_n], (void *)(uintptr_t)d->addr, len);
            wire_len[wire_n++] = (uint16_t)len;
        }
        int eor = (d->opts1 >> 30) & 1;
        d->opts1 &= ~(1u << 31);
        tx_at = eor ? 0 : tx_at + 1;
        if (tx_at > 4096)
            abort();                   /* a ring without EOR: runs away */
    }
}

/* A frame arrives from the wire: what the card does with it */
static void arrive(const uint8_t *f, uint16_t len, int bad_crc)
{
    if (!(regs[0x37] & 0x08) || !phy_up())
        return;
    if (g_family && (reg32(0xF0) & (1u << 19))) {
        filtered++;                    /* gated */
        return;
    }
    uint32_t rcr = reg32(0x44);
    int bcast = !memcmp(f, "\xff\xff\xff\xff\xff\xff", 6);
    int mine = !memcmp(f, regs, 6);
    if (!(rcr & 1) && !(bcast && (rcr & 8)) && !(mine && (rcr & 2))) {
        filtered++;
        return;
    }
    struct desc *r = ring(0xE4);
    struct desc *d = &r[rx_at];
    if (!(d->opts1 & (1u << 31))) {
        missed++;                      /* the driver has not given it back */
        return;
    }
    uint32_t size = d->opts1 & 0x3FFF;
    int eor = (d->opts1 >> 30) & 1;
    uint32_t rms = regs[0xDA] | regs[0xDB] << 8;
    if ((uint32_t)len + 4 > size || (uint32_t)len + 4 > rms) {
        missed++;
        return;
    }
    memcpy((void *)(uintptr_t)d->addr, f, len);
    memset((uint8_t *)(uintptr_t)d->addr + len, 0xCC, 4);     /* the CRC */
    d->opts1 = (eor ? 1u << 30 : 0) | (3u << 28) | (len + 4u) |
               (bad_crc ? (1u << 21) | (1u << 19) : 0);
    rx_at = eor ? 0 : rx_at + 1;
}

uint32_t model_in(uint16_t port, int size)
{
    int r = port - IO_BASE;
    if (r < 0 || r + size > 256)
        abort();
    if (r == 0x37 && reset_reads > 0 && --reset_reads == 0)
        regs[0x37] &= ~0x10;
    if (r == 0x6C)
        regs[0x6C] = phy_up() ? 0x13 : 0x00;   /* link, 1000, full */
    if (r == 0x60 || r == 0xB8)
        return phy_reply;
    uint32_t v = 0;
    for (int i = 0; i < size; i++)
        v |= (uint32_t)regs[r + i] << (8 * i);
    return v;
}

static void phy_access(uint32_t v, int ocp)
{
    /* The road this chip does not have: nothing happens */
    if (ocp != g_family) {
        phy_reply = 0;
        return;
    }
    int reg = ocp ? (int)((((v >> 16) & 0x7FFF) << 1) - 0xA400) / 2
                  : (int)((v >> 16) & 0x1F);
    if (v & (1u << 31)) {
        if (reg == 0) {
            bmcr = (uint16_t)v & ~0x0200;      /* (restart: self-clearing) */
            phy_writes++;
        }
        phy_reply = 0;                         /* done */
    } else {
        phy_reply = (1u << 31) | (reg == 0 ? bmcr : 0x7949);
    }
}

void model_out(uint16_t port, uint32_t value, int size)
{
    int r = port - IO_BASE;
    if (r < 0 || r + size > 256)
        abort();
    if (r == 0x60 || r == 0xB8) {
        phy_access(value, r == 0xB8);
        return;
    }
    if ((r >= 0x20 && r < 0x28) || (r >= 0xE4 && r < 0xEC) || r == 0xDA ||
        r == 0xEC) {
        if (!unlocked)
            return;                    /* locked: the write goes nowhere */
    }
    if (r == 0x3E) {                   /* ISR: a one clears the bit */
        regs[0x3E] &= ~(uint8_t)value;
        regs[0x3F] &= ~(uint8_t)(value >> 8);
        return;
    }
    if (r >= 0x40 && r < 0x44 && size == 4)
        value = (value & ~(0x7cfu << 20)) | xid << 20;   /* version: read-only */
    for (int i = 0; i < size; i++)
        regs[r + i] = (uint8_t)(value >> (8 * i));
    if (r == 0x50)
        unlocked = (value & 0xC0) == 0xC0;
    if (r == 0x37 && (value & 0x10)) {
        reset_reads = 3;
        rx_at = tx_at = 0;
        regs[0x37] = 0x10;
    }
    if (r == 0x38 && (value & 0x40))
        transmit();
}

/* --- what the driver sees around it ------------------------------------ */

int pci_find_device(uint16_t vendor, uint16_t device, struct pci_dev *out)
{
    if (vendor != 0x10EC || device != 0x8168)
        return 0;
    memset(out, 0, sizeof(*out));
    out->vendor = vendor;
    out->device = device;
    out->bar[0] = out->bar0 = IO_BASE | 1;
    return 1;
}

static int bus_master;
void pci_enable_bus_master(const struct pci_dev *dev)
{
    (void)dev;
    bus_master = 1;
}

static int threads;
struct task *task_create(const char *name, void (*entry)(void *), void *arg)
{
    (void)name;
    (void)entry;
    (void)arg;
    threads++;
    return 0;
}

void task_sleep(uint32_t ms)
{
    (void)ms;
}

static uint8_t got[512][2048];
static uint16_t got_len[512];
static int got_n;

void net_rx(const void *frame, uint16_t len)
{
    if (got_n < 512) {
        memcpy(got[got_n], frame, len);
        got_len[got_n++] = len;
    }
}

/* --- the checks -------------------------------------------------------- */

static int failures;

static void check(const char *what, int ok)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok)
        failures++;
}

static void frame(uint8_t *f, const uint8_t *to, int n, uint16_t len)
{
    memcpy(f, to, 6);
    memcpy(f + 6, "\x52\x54\x00\x12\x34\x56", 6);
    f[12] = 0x08;
    f[13] = 0x00;
    for (int i = 14; i < len; i++)
        f[i] = (uint8_t)(n * 7 + i);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <chip version> <BMCR>\n", argv[0]);
        return 2;
    }
    xid = (uint32_t)strtoul(argv[1], 0, 16);
    bmcr = (uint16_t)strtoul(argv[2], 0, 16);
    int was_down = !phy_up();
    /* The 8168G family and later, as Realtek numbers them */
    g_family = xid == 0x4c0 || xid == 0x4c1 || xid == 0x509 || xid == 0x5c8 ||
               xid == 0x500 || xid == 0x501 || xid == 0x502 ||
               (xid & 0x7c8) == 0x540 || xid == 0x6c0;
    memcpy(regs, card_mac, 6);
    regs[0x43] = (uint8_t)(xid >> 4);
    regs[0x42] = (uint8_t)((xid & 0xF) << 4);
    if (g_family)
        regs[0xF2] = 0x08;             /* RXDV_GATED_EN, as it comes up */

    check("the card is found and started", rtl8168_init() == 1);
    check("bus mastering on, the receive thread made",
          bus_master && threads == 1);
    check("its address is the one in IDR0-5",
          !memcmp(rtl8168_mac(), card_mac, 6));
    check(was_down ? "the PHY, left powered down, is powered up"
                   : "the PHY, up already, is left alone",
          was_down ? phy_up() && phy_writes == 1 : phy_writes == 0);
    check("the transmitter and the receiver are on",
          (regs[0x37] & 0x0C) == 0x0C);
    uint32_t rcr = reg32(0x44);
    check("RxConfig: broadcast and our address, not everybody's",
          (rcr & 0x0A) == 0x0A && !(rcr & 0x01));
    if (g_family)
        check("receiving is no longer gated (8168G and later)",
              !(reg32(0xF0) & (1u << 19)));
    check("there is a link", rtl8168_link() == 1);

    /* Sending: more frames than the ring has descriptors, any length */
    uint8_t f[2048];
    int sent_ok = 1;
    for (int n = 0; n < 100; n++) {
        uint16_t len = (uint16_t)(n == 0 ? 20 : 60 + (n * 37) % 1440);
        frame(f, (const uint8_t *)"\xff\xff\xff\xff\xff\xff", n, len);
        if (rtl8168_send(f, len) != 0)
            sent_ok = 0;
    }
    int same = wire_n == 100;
    for (int n = 0; same && n < 100; n++) {
        uint16_t len = (uint16_t)(n == 0 ? 20 : 60 + (n * 37) % 1440);
        frame(f, (const uint8_t *)"\xff\xff\xff\xff\xff\xff", n, len);
        uint16_t want = len < 60 ? 60 : len;
        same = wire_len[n] == want && !memcmp(wire[n], f, len);
        for (int i = len; same && i < want; i++)
            same = wire[n][i] == 0;
    }
    check("100 frames sent through a ring of 32 reach the wire, whole, "
          "in order; a short one padded with zeros to 60 bytes",
          sent_ok && same && rtl8168_tx_packets() == 100);

    /* Receiving: frames to us, to everybody, and to somebody else */
    int ok = 1, expect = 0;
    for (int n = 0; n < 150; n++) {
        uint16_t len = (uint16_t)(60 + (n * 53) % 1454);
        const uint8_t *to = n % 3 == 0 ? (const uint8_t *)"\xff\xff\xff\xff\xff\xff"
                          : n % 3 == 1 ? card_mac
                                       : (const uint8_t *)"\x00\x11\x22\x33\x44\x55";
        frame(f, to, n, len);
        arrive(f, len, 0);
        if (n % 3 != 2)
            expect++;
        if (n % 10 == 9)
            rtl8168_poll();
    }
    rtl8168_poll();
    for (int n = 0, k = 0; ok && n < 150; n++) {
        if (n % 3 == 2)
            continue;
        uint16_t len = (uint16_t)(60 + (n * 53) % 1454);
        frame(f, n % 3 ? card_mac : (const uint8_t *)"\xff\xff\xff\xff\xff\xff",
              n, len);
        ok = k < got_n && got_len[k] == len && !memcmp(got[k], f, len);
        k++;
    }
    check("100 frames for us, through a ring of 64, reach the network "
          "code whole and in order, without their CRC; none for another "
          "address", ok && got_n == expect && missed == 0 && filtered == 50);

    /* A damaged frame is not passed on, and its descriptor goes back */
    got_n = 0;
    frame(f, card_mac, 1, 100);
    arrive(f, 100, 1);
    frame(f, card_mac, 2, 100);
    arrive(f, 100, 0);
    rtl8168_poll();
    check("a frame with a bad CRC is dropped; the one after it arrives",
          got_n == 1 && got[0][14] == (uint8_t)(2 * 7 + 14));

    /* A burst larger than the ring before the driver looks: what did
       not fit is lost, and the ring works on afterwards */
    got_n = 0;
    missed = 0;
    for (int n = 0; n < 70; n++) {
        frame(f, card_mac, n, 200);
        arrive(f, 200, 0);
    }
    rtl8168_poll();
    int first = got_n, lost = missed;
    for (int n = 0; n < 10; n++) {
        frame(f, card_mac, 100 + n, 300);
        arrive(f, 300, 0);
    }
    rtl8168_poll();
    check("a burst of 70 into a ring of 64: 64 taken, 6 lost, and the "
          "ring takes more afterwards",
          first == 64 && lost == 6 && got_n == 74 && missed == 6);

    printf("%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
