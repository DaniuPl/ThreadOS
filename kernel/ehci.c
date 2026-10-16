/* USB host controller (EHCI) - USB 2.0
 *
 * Three structures do all the work, and they are the same three ideas
 * UHCI had, grown up:
 *
 *   a qTD (queue element transfer descriptor) describes one piece of a
 *   transfer - a direction, a length, up to five pages of buffer;
 *
 *   a QH (queue head) describes an endpoint - which device, which
 *   endpoint, how big a packet - and carries an "overlay": a copy of
 *   the qTD the controller is working on right now;
 *
 *   two schedules. The ASYNCHRONOUS one is a ring of queue heads the
 *   controller walks whenever it has nothing better to do: control and
 *   bulk transfers go there. The PERIODIC one is a list of 1024
 *   pointers, one per millisecond: interrupt endpoints go there,
 *   because a keyboard has to be asked on a schedule.
 *
 * Choices made here on purpose:
 *
 * One queue head for every control and bulk transfer. Before each one
 * the asynchronous schedule is stopped, the queue head is rewritten
 * for the endpoint in question, and the schedule is started again.
 * Rewriting a queue head the controller may have cached is the one
 * thing the specification forbids, and stopping the schedule is the
 * simplest way of being sure it has not. It costs a little time per
 * transfer, and buys a driver with no per-endpoint bookkeeping.
 *
 * That means the data toggle cannot live in the queue head (it would
 * be overwritten by the next endpoint), so it is kept by the caller and
 * written into each qTD - the "DTC" bit says so to the controller.
 *
 * Interrupt endpoints DO get queue heads of their own, which stay in
 * the periodic schedule for good, and there the controller keeps the
 * toggle itself. A read is one qTD put under the queue head and never
 * taken back: the same lesson uhci.c spells out at length, that a
 * transfer abandoned half-way loses a report and leaves the toggle out
 * of step for ever.
 *
 * Slower devices are not ours. EHCI cannot speak at 12 or 1.5 Mb/s at
 * all: the port is handed to the companion controller with one bit,
 * and the device appears there as though EHCI did not exist.
 *
 * Polled, not interrupt-driven, like the other two controllers. All the
 * structures live in the kernel image, which sits in the identity
 * mapping, so the address the controller is given is the address we
 * use. They must also lie below 4 GiB - EHCI's structures have 32-bit
 * pointers - and the 64-bit kernel checks that rather than assumes it.
 */

#include <stdint.h>
#include "include/ehci.h"
#include "include/pci.h"
#include "include/spinlock.h"
#include "include/printk.h"
#include "include/string.h"
#include "include/pit.h"
#include "include/io.h"

#ifdef __x86_64__
#include "../kernel64/paging64.h"
#define MAP_PHYS(a, n)   paging64_map_phys((a), (n))
#else
#include "include/paging.h"
#define MAP_PHYS(a, n)   paging_map_phys((a), (n))
#endif

/* --- registers ------------------------------------------------------- */

#define CAP_CAPLENGTH   0x00        /* 8 bits: where the operational part begins */
#define CAP_HCSPARAMS   0x04
#define CAP_HCCPARAMS   0x08

#define OP_USBCMD       0x00
#define OP_USBSTS       0x04
#define OP_USBINTR      0x08
#define OP_CTRLDSSEG    0x10        /* the upper 32 bits of every structure */
#define OP_PERIODIC     0x14
#define OP_ASYNC        0x18
#define OP_CONFIGFLAG   0x40
#define OP_PORTSC       0x44        /* every 4 per port */

#define CMD_RS          (1u << 0)
#define CMD_HCRESET     (1u << 1)
#define CMD_PSE         (1u << 4)   /* the periodic schedule runs */
#define CMD_ASE         (1u << 5)   /* the asynchronous one runs */
#define CMD_ITC_1MS     (8u << 16)

#define STS_HALTED      (1u << 12)
#define STS_PSS         (1u << 14)
#define STS_ASS         (1u << 15)

#define PORT_CCS        (1u << 0)   /* something is plugged in */
#define PORT_CSC        (1u << 1)
#define PORT_PED        (1u << 2)   /* enabled - which on EHCI means high speed */
#define PORT_PEDC       (1u << 3)
#define PORT_OCC        (1u << 5)
#define PORT_PR         (1u << 8)
#define PORT_LS_MASK    (3u << 10)  /* the line state before a reset */
#define PORT_LS_K       (1u << 10)  /* K-state: a low-speed device */
#define PORT_PP         (1u << 12)
#define PORT_OWNER      (1u << 13)  /* 1 = the companion controller has it */
/* Change bits are cleared by writing a one: a plain read-modify-write
   would clear every change it happened to read. They are masked out of
   every write that does not mean to clear them. */
#define PORT_W1C        (PORT_CSC | PORT_PEDC | PORT_OCC)

/* Bit 0 of a link: nothing follows. Bits 2:1 = 01: what follows is a
   queue head (rather than an isochronous descriptor). */
#define LINK_T          0x1u
#define LINK_QH         0x2u

/* The token of a qTD - and of a queue head's overlay */
#define TOK_ACTIVE      (1u << 7)
#define TOK_HALTED      (1u << 6)
#define TOK_BUFERR      (1u << 5)
#define TOK_BABBLE      (1u << 4)
#define TOK_XACT        (1u << 3)
#define TOK_PID_OUT     (0u << 8)
#define TOK_PID_IN      (1u << 8)
#define TOK_PID_SETUP   (2u << 8)
#define TOK_CERR3       (3u << 10)  /* three tries before giving up */
#define TOK_IOC         (1u << 15)
#define TOK_BYTES(t)    (((t) >> 16) & 0x7FFFu)
#define TOK_TOGGLE      (1u << 31)

/* A queue head's endpoint characteristics and capabilities */
#define QH_EP(e)        ((uint32_t)(e) << 8)
#define QH_EPS_HIGH     (2u << 12)
#define QH_DTC          (1u << 14)  /* the toggle comes from each qTD */
#define QH_HEAD         (1u << 15)  /* head of the asynchronous ring */
#define QH_MPS(m)       ((uint32_t)(m) << 16)
#define QH_RL(n)        ((uint32_t)(n) << 28)
#define QH_SMASK(m)     ((uint32_t)(m))
#define QH_MULT1        (1u << 30)

/* A qTD may carry five pages: the first from an offset, the rest
   whole. 16 KiB fits whatever the offset is, and is a whole number of
   packets of any size a high-speed endpoint may have. */
#define QTD_CHUNK       16384
#define QTD_MAX         10
#define INT_PIPES       8

/* --- structures -------------------------------------------------------
 *
 * Both in their 64-bit shape (the extra words hold the upper halves of
 * the buffer pointers). A controller that cannot address above 4 GiB
 * reads only the first part and never looks at the rest, so the larger
 * shape is right for either.
 */
struct qtd {
    volatile uint32_t next;
    volatile uint32_t alt;
    volatile uint32_t token;
    volatile uint32_t buf[5];
    volatile uint32_t buf_hi[5];
    uint32_t pad[3];
} __attribute__((aligned(32)));

struct qh {
    volatile uint32_t link;
    volatile uint32_t chars;
    volatile uint32_t caps;
    volatile uint32_t current;
    /* the overlay - the shape of a qTD */
    volatile uint32_t next;
    volatile uint32_t alt;
    volatile uint32_t token;
    volatile uint32_t buf[5];
    volatile uint32_t buf_hi[5];
    uint32_t pad[7];
} __attribute__((aligned(32)));

static uint32_t frame_list[1024] __attribute__((aligned(4096)));
static struct qh async_qh;
static struct qtd chain[QTD_MAX];
static struct qtd stop_td;          /* where a short packet sends the controller */
static uint8_t setup_buf[8] __attribute__((aligned(32)));

static struct qh int_qh[INT_PIPES];
static struct qtd int_td[INT_PIPES];
static uint8_t int_buf[INT_PIPES][64] __attribute__((aligned(32)));

struct pipe {
    int used;
    int linked;                     /* in the periodic schedule */
    int armed;                      /* a qTD is waiting under it */
    int asked;                      /* how many bytes it was asked for */
    int mps;
};
static struct pipe pipes[INT_PIPES];

static uintptr_t base, op;
static int nports;
static int wide;                    /* the controller can address above 4 GiB */
static const char *gave_up = "the controller was never looked for";

/* One transfer on the asynchronous schedule at a time: the queue head
   is shared. A keyboard poll and a stick being read take turns. */
static struct spinlock async_lock = SPINLOCK_FREE;
static volatile uint32_t async_turn;
/* The periodic schedule is changed by whoever declares an endpoint */
static struct spinlock period_lock = SPINLOCK_FREE;

/* --- small things ------------------------------------------------------ */

static uint32_t rd(uintptr_t a)              { return *(volatile uint32_t *)a; }
static void     wr(uintptr_t a, uint32_t v)  { *(volatile uint32_t *)a = v; }

static void wait_ms(uint32_t ms)
{
    uint32_t until = pit_ticks() + (ms + 9) / 10 + 1;
    while (pit_ticks() < until)
        io_wait();
}

/* The address the controller is given for a structure of ours */
static uint32_t phys(const volatile void *p)
{
    return (uint32_t)(uintptr_t)p;
}

static int give_up(const char *why)
{
    gave_up = why;
    base = 0;
    return 0;
}

const char *ehci_why(void)
{
    return base ? (const char *)0 : gave_up;
}

int ehci_ports(void)
{
    return base ? nports : 0;
}

static uintptr_t port_reg(int port)
{
    return op + OP_PORTSC + 4 * (uintptr_t)port;
}

/* Points the five buffer pages of a qTD at a piece of memory. Returns
   0, or -1 when the memory is somewhere the controller cannot reach. */
static int point_at(struct qtd *t, const void *data, int len)
{
    uint64_t a = (uint64_t)(uintptr_t)data;

    for (int i = 0; i < 5; i++) {
        t->buf[i] = 0;
        t->buf_hi[i] = 0;
    }
    if (!len)
        return 0;
    if (((a + (uint64_t)len - 1) >> 32) && !wide)
        return -1;
    t->buf[0] = (uint32_t)a;
    t->buf_hi[0] = (uint32_t)(a >> 32);
    /* The other four are whole pages, following the first */
    uint64_t page = a & ~0xFFFull;
    for (int i = 1; i < 5; i++) {
        page += 4096;
        t->buf[i] = (uint32_t)page;
        t->buf_hi[i] = (uint32_t)(page >> 32);
    }
    return 0;
}

static int fill(struct qtd *t, uint32_t pid, int toggle, const void *data,
                int len)
{
    t->next = LINK_T;
    t->alt = LINK_T;
    if (point_at(t, data, len) < 0)
        return -1;
    t->token = TOK_ACTIVE | TOK_CERR3 | pid | ((uint32_t)len << 16) |
               (toggle ? TOK_TOGGLE : 0);
    return 0;
}

/* --- taking the controller from the firmware --------------------------
 *
 * A BIOS with "USB legacy support" drives the controller itself, so
 * that a USB keyboard works in its menus, and keeps doing so through
 * System Management Interrupts long after an operating system has
 * started - unless the system asks for it. The request is a bit in an
 * extended capability in PCI configuration space: OS owned. The
 * firmware is supposed to answer by letting go of BIOS owned.
 */
static void take_from_firmware(const struct pci_dev *d, uint32_t hcc)
{
    uint32_t at = (hcc >> 8) & 0xFF;

    for (int hops = 0; at >= 0x40 && hops < 16; hops++) {
        uint32_t v = pci_config_read(d->bus, d->slot, d->func, (int)at);
        if ((v & 0xFF) == 1) {                    /* USBLEGSUP */
            if (v & (1u << 16)) {
                pci_config_write(d->bus, d->slot, d->func, (int)at,
                                 v | (1u << 24));
                for (int i = 0; i < 100 && (v & (1u << 16)); i++) {
                    wait_ms(10);
                    v = pci_config_read(d->bus, d->slot, d->func, (int)at);
                }
                if (v & (1u << 16)) {
                    /* A firmware that will not answer gets the same
                       treatment Linux gives it: the bit is taken away. */
                    printk("[ !! ] EHCI: the firmware would not let go, "
                           "taking the controller anyway\n");
                    pci_config_write(d->bus, d->slot, d->func, (int)at,
                                     (v & ~(1u << 16)) | (1u << 24));
                }
            }
            /* No more SMIs from it: every enable off, every pending
               status acknowledged */
            pci_config_write(d->bus, d->slot, d->func, (int)at + 4,
                             0xE0000000u);
            return;
        }
        at = (v >> 8) & 0xFF;
    }
}

/* --- the asynchronous schedule ---------------------------------------- */

static int async_stop(void)
{
    wr(op + OP_USBCMD, rd(op + OP_USBCMD) & ~CMD_ASE);
    for (int i = 0; i < 100; i++) {
        if (!(rd(op + OP_USBSTS) & STS_ASS))
            return 0;
        io_wait();
    }
    for (int i = 0; i < 5 && (rd(op + OP_USBSTS) & STS_ASS); i++)
        wait_ms(1);
    return (rd(op + OP_USBSTS) & STS_ASS) ? -1 : 0;
}

static void async_start(void)
{
    wr(op + OP_ASYNC, phys(&async_qh));
    wr(op + OP_USBCMD, rd(op + OP_USBCMD) | CMD_ASE);
}

static void take_turn(void)
{
    for (;;) {
        irqflags_t flags = spin_lock_irq(&async_lock);
        int free_now = !async_turn;
        if (free_now)
            async_turn = 1;
        spin_unlock_irq(&async_lock, flags);
        if (free_now)
            return;
        io_wait();
    }
}

static void give_turn(void)
{
    async_turn = 0;
}

/* Runs chain[0..n-1] on endpoint 'ep' of device 'addr' and waits.
 *
 * Done means: the last qTD finished; or one of them halted (a stall,
 * or three failed tries); or an IN qTD came back short, which in a bulk
 * read is how a device says "that is all" - the controller has then
 * gone to stop_td and will not touch the rest.
 *
 * Returns 0, or -1 for a halt or a timeout. */
static int run(int addr, int ep, int mps, int n, uint32_t timeout_ms)
{
    if (async_stop() < 0)
        return -1;

    for (int i = 0; i + 1 < n; i++)
        chain[i].next = phys(&chain[i + 1]);
    chain[n - 1].next = LINK_T;

    async_qh.link = phys(&async_qh) | LINK_QH;
    async_qh.chars = QH_RL(4) | QH_MPS(mps) | QH_HEAD | QH_DTC |
                     QH_EPS_HIGH | QH_EP(ep) | (uint32_t)addr;
    async_qh.caps = QH_MULT1;
    async_qh.current = 0;
    async_qh.alt = LINK_T;
    async_qh.token = 0;
    for (int i = 0; i < 5; i++) {
        async_qh.buf[i] = 0;
        async_qh.buf_hi[i] = 0;
    }
    async_qh.next = phys(&chain[0]);

    async_start();

    uint32_t until = pit_ticks() + (timeout_ms + 9) / 10 + 1;
    int result = -1;
    while (pit_ticks() < until) {
        int done = 0;
        for (int i = 0; i < n; i++) {
            uint32_t t = chain[i].token;
            if (t & TOK_HALTED) {
                done = -1;
                break;
            }
            if (t & TOK_ACTIVE)
                break;
            if (i == n - 1 ||
                ((t & (3u << 8)) == TOK_PID_IN && TOK_BYTES(t) &&
                 chain[i].alt != LINK_T)) {
                done = 1;
                break;
            }
        }
        if (done) {
            result = done > 0 ? 0 : -1;
            break;
        }
        io_wait();
    }

    async_stop();
    return result;
}

int ehci_control(int addr, int maxpacket, const void *setup,
                 void *data, int len, int in)
{
    if (!base || maxpacket <= 0 || len < 0 || len > QTD_CHUNK)
        return -1;

    take_turn();
    memcpy(setup_buf, setup, 8);

    int n = 0;
    fill(&chain[n++], TOK_PID_SETUP, 0, setup_buf, 8);
    if (len) {
        /* A data stage that comes back short still has to be followed
           by the status stage: with 'alt' terminated, the controller
           goes on to 'next', which is exactly that. */
        if (fill(&chain[n++], in ? TOK_PID_IN : TOK_PID_OUT, 1, data,
                 len) < 0) {
            give_turn();
            return -1;
        }
    }
    /* The status stage goes the other way from the data, and always
       with toggle one */
    fill(&chain[n++], (len && in) ? TOK_PID_OUT : TOK_PID_IN, 1,
         (void *)0, 0);
    chain[n - 1].token |= TOK_IOC;

    int r = run(addr, 0, maxpacket, n, 1000);
    int got = len ? len - (int)TOK_BYTES(chain[1].token) : 0;
    give_turn();
    return r < 0 ? -1 : got;
}

/* How many packets a piece of a transfer took - which is how far the
   data toggle moved. A full piece is a whole number of packets; one
   that came back short ended with one short (possibly empty) packet. */
static int packets(int asked, int moved, int mps, int in)
{
    if (!in || moved == asked)
        return moved ? (moved + mps - 1) / mps : 1;
    return moved / mps + 1;
}

int ehci_bulk(int addr, int ep, int maxpacket, void *data, int len,
              int in, int *toggle)
{
    if (!base || maxpacket <= 0 || len < 0)
        return -1;

    uint8_t *p = data;
    int left = len, total = 0;

    take_turn();
    do {
        int n = 0, asked[QTD_MAX], round = 0;
        int t = *toggle;

        /* Up to QTD_MAX pieces per round. Each but the last is a whole
           number of packets, so each piece's toggle follows from the
           bytes before it. */
        while (n < QTD_MAX && (left > 0 || (len == 0 && n == 0))) {
            int chunk = left > QTD_CHUNK ? QTD_CHUNK : left;
            if (fill(&chain[n], in ? TOK_PID_IN : TOK_PID_OUT, t,
                     p + round, chunk) < 0) {
                give_turn();
                return -1;
            }
            /* A short read ends the transfer: the controller is sent to
               a descriptor that is not active, and stops there */
            if (in)
                chain[n].alt = phys(&stop_td);
            asked[n] = chunk;
            t ^= packets(chunk, chunk, maxpacket, in) & 1;
            round += chunk;
            left -= chunk;
            n++;
            if (!chunk)
                break;
        }
        chain[n - 1].token |= TOK_IOC;

        if (run(addr, ep, maxpacket, n, 5000) < 0) {
            give_turn();
            return -1;
        }

        /* What actually moved, and where the toggle really is now */
        int moved_round = 0, short_seen = 0;
        t = *toggle;
        for (int i = 0; i < n; i++) {
            uint32_t tok = chain[i].token;
            if (tok & TOK_ACTIVE)
                break;                   /* never reached - after a short one */
            int moved = asked[i] - (int)TOK_BYTES(tok);
            moved_round += moved;
            t ^= packets(asked[i], moved, maxpacket, in) & 1;
            if (moved < asked[i]) {
                short_seen = 1;
                break;
            }
        }
        *toggle = t;
        total += moved_round;
        p += moved_round;
        if (short_seen)
            break;
    } while (left > 0);

    give_turn();
    return total;
}

/* --- the periodic schedule --------------------------------------------
 *
 * Every frame pointer leads to the first interrupt queue head, each
 * queue head to the next, and the last one ends the list. A queue head
 * is added at the end, by pointing the one before it (or the frame
 * list, for the first) at it - a single 32-bit write the controller
 * sees whole or not at all.
 */
static void relink(void)
{
    uint32_t first = LINK_T;
    int last = -1;

    for (int i = 0; i < INT_PIPES; i++) {
        if (!pipes[i].linked)
            continue;
        if (last < 0)
            first = phys(&int_qh[i]) | LINK_QH;
        else
            int_qh[last].link = phys(&int_qh[i]) | LINK_QH;
        last = i;
    }
    if (last >= 0)
        int_qh[last].link = LINK_T;
    for (int f = 0; f < 1024; f++)
        frame_list[f] = first;
}

int ehci_int_endpoint(int addr, int ep, int maxpacket)
{
    if (!base || maxpacket <= 0)
        return -1;

    irqflags_t flags = spin_lock_irq(&period_lock);
    int h = -1;
    for (int i = 0; i < INT_PIPES; i++)
        if (!pipes[i].used) {
            h = i;
            break;
        }
    if (h < 0) {
        spin_unlock_irq(&period_lock, flags);
        return -1;
    }
    pipes[h].used = 1;
    spin_unlock_irq(&period_lock, flags);

    struct qh *q = &int_qh[h];
    if (maxpacket > 64)
        maxpacket = 64;
    q->link = LINK_T;
    /* RL zero: periodic queue heads do not count NAKs. No DTC: the
       controller keeps the toggle in the overlay, and it stays right
       however many reads go through. */
    q->chars = QH_MPS(maxpacket) | QH_EPS_HIGH | QH_EP(ep) |
               (uint32_t)addr;
    /* Microframe 0 of every frame: once a millisecond, which is as
       often as any keyboard is worth asking */
    q->caps = QH_MULT1 | QH_SMASK(0x01);
    q->current = 0;
    q->next = LINK_T;
    q->alt = LINK_T;
    q->token = 0;
    for (int i = 0; i < 5; i++) {
        q->buf[i] = 0;
        q->buf_hi[i] = 0;
    }

    pipes[h].mps = maxpacket;
    pipes[h].armed = 0;

    flags = spin_lock_irq(&period_lock);
    pipes[h].linked = 1;
    relink();
    spin_unlock_irq(&period_lock, flags);
    return h;
}

int ehci_int_in(int h, void *data, int len)
{
    if (!base || h < 0 || h >= INT_PIPES || !pipes[h].used || len <= 0)
        return -1;

    struct pipe *p = &pipes[h];
    struct qh *q = &int_qh[h];
    struct qtd *t = &int_td[h];

    if (len > p->mps)
        len = p->mps;

    if (!p->armed) {
        if (fill(t, TOK_PID_IN, 0, int_buf[h], len) < 0)
            return -1;
        p->asked = len;
        p->armed = 1;
        /* The queue head is idle (its overlay is not active): pointing
           its next pointer at the qTD is all it takes, and the
           controller picks it up on its next visit */
        q->next = phys(t);
        return 0;
    }

    uint32_t tok = t->token;
    if (tok & TOK_ACTIVE)
        return 0;                        /* nothing to say yet - keep waiting */

    p->armed = 0;
    if (tok & TOK_HALTED) {
        /* The queue head halted with it. Clearing the overlay lets the
           endpoint be asked again once the device recovers; the toggle
           starts over, as it does on the device after a stall. */
        q->token = 0;
        return -1;
    }
    int got = p->asked - (int)TOK_BYTES(tok);
    if (got > len)
        got = len;
    memcpy(data, int_buf[h], (uint32_t)got);

    /* And straight away the next read, before returning. Left for the
       next call, the device would go unasked for a whole round of the
       caller's loop - twenty milliseconds and more - while whatever it
       has to say piles up in it. Armed now, the controller asks every
       millisecond in the meantime, and the next report is waiting in
       the buffer when the caller comes back. */
    if (fill(t, TOK_PID_IN, 0, int_buf[h], len) == 0) {
        p->asked = len;
        p->armed = 1;
        q->next = phys(t);
    }
    return got;
}

void ehci_int_release(int h)
{
    if (h < 0 || h >= INT_PIPES || !pipes[h].used)
        return;
    irqflags_t flags = spin_lock_irq(&period_lock);
    pipes[h].linked = 0;
    relink();
    spin_unlock_irq(&period_lock, flags);
    /* The controller may be holding a pointer to it for the rest of
       this frame and the next - two milliseconds and it is ours again */
    wait_ms(2);
    flags = spin_lock_irq(&period_lock);
    pipes[h].used = 0;
    pipes[h].armed = 0;
    spin_unlock_irq(&period_lock, flags);
}

/* --- ports -------------------------------------------------------------- */

/* The port is the companion's from now on. The device on it is
   disconnected from this side and appears on the other; when it is
   pulled out, the port comes back to us by itself. */
static void hand_over(int port)
{
    uintptr_t r = port_reg(port);
    wr(r, (rd(r) & ~PORT_W1C) | PORT_OWNER);
}

int ehci_port_connected(int port)
{
    if (!base || port < 0 || port >= nports)
        return 0;
    uint32_t v = rd(port_reg(port));
    if (v & PORT_OWNER)
        return 0;                        /* the companion's device, not ours */
    return (v & PORT_CCS) ? 1 : 0;
}

int ehci_port_reset(int port)
{
    if (!base || port < 0 || port >= nports)
        return 0;

    uintptr_t r = port_reg(port);
    uint32_t v = rd(r);
    if (!(v & PORT_CCS) || (v & PORT_OWNER))
        return 0;

    /* A low-speed device can be told before the reset: it holds the
       line in the K state. It is the companion's without asking. */
    if ((v & PORT_LS_MASK) == PORT_LS_K) {
        hand_over(port);
        return -1;
    }

    /* The reset itself: 50 ms by the specification, then let go and
       wait for the controller to finish its half */
    wr(r, (v & ~(PORT_W1C | PORT_PED)) | PORT_PR);
    wait_ms(50);
    wr(r, rd(r) & ~(PORT_W1C | PORT_PR));
    for (int i = 0; i < 20 && (rd(r) & PORT_PR); i++)
        wait_ms(1);

    v = rd(r);
    if (!(v & PORT_CCS))
        return 0;
    /* After a reset only a high-speed device leaves the port enabled -
       the chirp that says so happens during the reset itself. A port
       that stays disabled has a full-speed device on it, which is the
       companion's. */
    if (!(v & PORT_PED)) {
        hand_over(port);
        return -1;
    }
    wr(r, v);                            /* writing the change bits back clears them */
    wait_ms(10);                         /* the reset recovery a device is owed */
    return 3;
}

/* --- start-up ----------------------------------------------------------- */

int ehci_init(void)
{
    if (base)
        return 1;

    struct pci_dev dev;
    /* Class 0x0C (serial bus), subclass 0x03 (USB), programming
       interface 0x20 (EHCI) */
    if (!pci_find_class(0x0C, 0x03, 0x20, &dev))
        return give_up("there is no EHCI controller on the PCI bus");
    if (dev.bar[0] & 0x01)
        return give_up("the controller's BAR0 is not a memory window");

    uint64_t addr = dev.bar[0] & ~0xFULL;
    if (((dev.bar[0] >> 1) & 0x3) == 0x2)
        addr |= (uint64_t)dev.bar[1] << 32;
    if (!addr)
        return give_up("the firmware gave the controller no address");
#ifndef __x86_64__
    if (addr > 0xFFFFFFFFull)
        return give_up("the controller sits above 4 GiB (32-bit mode)");
#else
    /* The structures below are handed to the controller as 32-bit
       addresses. They are in the kernel image, which is loaded low -
       but "is loaded low" is a promise about somebody else's code, so
       it is checked. */
    if ((uint64_t)(uintptr_t)frame_list >> 32 ||
        (uint64_t)(uintptr_t)&int_buf[INT_PIPES - 1][63] >> 32)
        return give_up("the driver's own structures lie above 4 GiB");
#endif

    if (MAP_PHYS((uintptr_t)addr, 0x1000) < 0)
        return give_up("could not map the controller's window");
    pci_enable_bus_master(&dev);

    uintptr_t b = (uintptr_t)addr;
    uintptr_t o = b + (rd(b + CAP_CAPLENGTH) & 0xFF);
    uint32_t hcs = rd(b + CAP_HCSPARAMS);
    uint32_t hcc = rd(b + CAP_HCCPARAMS);

    take_from_firmware(&dev, hcc);

    /* Stop it, then reset it: whatever state the firmware left it in,
       reset is the one we know */
    wr(o + OP_USBCMD, rd(o + OP_USBCMD) & ~CMD_RS);
    for (int i = 0; i < 20 && !(rd(o + OP_USBSTS) & STS_HALTED); i++)
        wait_ms(1);
    wr(o + OP_USBCMD, CMD_HCRESET);
    for (int i = 0; i < 30 && (rd(o + OP_USBCMD) & CMD_HCRESET); i++)
        wait_ms(10);
    if (rd(o + OP_USBCMD) & CMD_HCRESET)
        return give_up("the controller will not reset");

    base = b;
    op = o;
    nports = (int)(hcs & 0xF);
    wide = (int)(hcc & 1);

    memset(&async_qh, 0, sizeof(async_qh));
    memset(chain, 0, sizeof(chain));
    memset(&stop_td, 0, sizeof(stop_td));
    memset(int_qh, 0, sizeof(int_qh));
    memset(pipes, 0, sizeof(pipes));

    /* An inactive descriptor with nowhere to go: the place a short
       read is sent to, so that the rest of its chain is left alone */
    stop_td.next = LINK_T;
    stop_td.alt = LINK_T;
    stop_td.token = 0;

    /* The asynchronous ring: one queue head, pointing at itself, and
       marked as the head so the controller knows where a lap ends */
    async_qh.link = phys(&async_qh) | LINK_QH;
    async_qh.chars = QH_HEAD | QH_EPS_HIGH;
    async_qh.caps = QH_MULT1;
    async_qh.next = LINK_T;
    async_qh.alt = LINK_T;

    for (int f = 0; f < 1024; f++)
        frame_list[f] = LINK_T;

    if (wide)
        wr(op + OP_CTRLDSSEG, 0);        /* everything is below 4 GiB */
    wr(op + OP_USBINTR, 0);              /* we poll */
    wr(op + OP_PERIODIC, phys(frame_list));
    wr(op + OP_ASYNC, phys(&async_qh));
    wr(op + OP_USBSTS, 0x3F);            /* acknowledge whatever was pending */
    /* Running, the periodic schedule on, a frame list of 1024 (bits 3:2
       left at zero), interrupts not more often than once a
       millisecond. The asynchronous schedule is started per transfer. */
    wr(op + OP_USBCMD, CMD_ITC_1MS | CMD_PSE | CMD_RS);
    for (int i = 0; i < 20 && (rd(op + OP_USBSTS) & STS_HALTED); i++)
        wait_ms(1);
    if (rd(op + OP_USBSTS) & STS_HALTED)
        return give_up("the controller will not start after the reset");

    /* Every port to us. Until this flag is set, all of them belong to
       the companions - which is how a machine with no EHCI driver
       still has a working keyboard. */
    wr(op + OP_CONFIGFLAG, 1);
    wait_ms(5);

    /* Port power, where the controller leaves it to software */
    if (hcs & (1u << 4)) {
        for (int i = 0; i < nports; i++) {
            uintptr_t r = port_reg(i);
            uint32_t v = rd(r);
            if (!(v & PORT_PP))
                wr(r, (v & ~PORT_W1C) | PORT_PP);
        }
    }
    /* A device needs time after power and after the change of owner
       before it shows up on our side */
    wait_ms(100);
    return 1;
}
