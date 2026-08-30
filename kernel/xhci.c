/* USB host controller (xHCI)
 *
 * The third USB controller and the only one on newer boards. It
 * differs from UHCI not in details but in the division of labour.
 *
 * In UHCI the driver arranged transfer descriptors in a frame list
 * which the controller swept every millisecond, and assigned device
 * addresses itself. Here it is the other way round: one talks to the
 * controller with COMMANDS placed into a command ring, and the answers
 * come back through a second ring - of events. Device addresses are
 * assigned by the controller, and we get a slot number.
 *
 * A ring is an array of sixteen-byte TRBs whose last element is a jump
 * back to the start. Which TRBs are already ours and which the
 * controller's is settled by a single bit - the cycle bit. On every
 * turn of the ring its meaning flips, so nothing ever has to be
 * cleared or counted: the controller processes TRBs for as long as
 * their cycle bit matches its own.
 *
 * We wait by polling, without interrupts - for the same reason as with
 * UHCI: a thread polls the keyboard anyway, and one road fewer is one
 * place fewer where something can go wrong.
 *
 * The structures have to lie in physically contiguous memory and at an
 * address the controller understands directly - that is, in the
 * identity mapping, where the virtual address equals the physical one.
 */

#include <stdint.h>
#include "include/xhci.h"
#include "include/pci.h"
#include "include/printk.h"
#include "include/string.h"
#include "include/pit.h"
#include "include/io.h"

/* The registers sit in memory, so they have to be mapped first. The
   address comes from a BAR rather than from anything fixed - that is
   the only thing in this file that depends on the processor mode. */
#ifdef __x86_64__
#include "../kernel64/paging64.h"
#define MAP_PHYS(a, n)   paging64_map_phys((a), (n))
#else
#include "include/paging.h"
#define MAP_PHYS(a, n)   paging_map_phys((a), (n))
#endif

/* --- registers ------------------------------------------------------- */

/* The capability part (from the base) */
#define CAP_CAPLENGTH   0x00        /* 8 bits: where the operational part begins */
#define CAP_HCSPARAMS1  0x04
#define CAP_HCSPARAMS2  0x08
#define CAP_HCCPARAMS1  0x10
#define CAP_DBOFF       0x14
#define CAP_RTSOFF      0x18

/* The operational part (from base + CAPLENGTH) */
#define OP_USBCMD       0x00
#define OP_USBSTS       0x04
#define OP_PAGESIZE     0x08
#define OP_DNCTRL       0x14
#define OP_CRCR         0x18
#define OP_DCBAAP       0x30
#define OP_CONFIG       0x38
#define OP_PORTSC       0x400       /* every 0x10 per port */

#define CMD_RS          0x0001
#define CMD_HCRST       0x0002
#define CMD_INTE        0x0004

#define STS_HCH         0x0001      /* the controller is stopped */
#define STS_CNR         0x0800      /* the controller is not ready yet */

#define PORT_CCS        (1u << 0)   /* something is plugged in */
#define PORT_PED        (1u << 1)   /* the port is enabled */
#define PORT_PR         (1u << 4)   /* port reset */
#define PORT_PP         (1u << 9)   /* port power */
#define PORT_SPEED_SH   10
#define PORT_ZMIANY     0x00FE0000u /* change bits: they are cleared by writing a one */

/* The runtime part (from base + RTSOFF), interrupter number 0 */
#define RT_IMAN         0x20
#define RT_IMOD         0x24
#define RT_ERSTSZ       0x28
#define RT_ERSTBA       0x30
#define RT_ERDP         0x38

/* TRB types */
#define TRB_NORMAL      1
#define TRB_SETUP       2
#define TRB_DATA        3
#define TRB_STATUS      4
#define TRB_LINK        6
#define TRB_ENABLE_SLOT 9
#define TRB_ADDRESS_DEV 11
#define TRB_CONFIG_EP   12
#define TRB_EVAL_CTX    13
#define TRB_EV_TRANSFER 32         /* event: a transfer is finished */
#define TRB_EV_COMMAND 33         /* event: a command has been executed */

#define TRB_IOC         (1u << 5)   /* tell us when it is done */
#define TRB_IDT         (1u << 6)   /* the data sits inside the TRB itself */
#define TRB_ISP         (1u << 2)   /* a shorter packet also ends the transfer */
#define TRB_TC          (1u << 1)   /* the jump flips the cycle bit */

#define CODE_OK      1
#define CODE_SHORT      13          /* less arrived than we asked for - that is not an error */

#define SLOTS          4           /* this many devices at once is enough for us */
#define TRB_CHAIN      16          /* of which the last is a jump back to the start */
#define CTX_BYTES      2112        /* the largest possible context set */

struct trb {
    volatile uint32_t p0, p1, status, control;
};

/* A ring from the writer's side: where to put the next TRB and with
   which cycle bit */
struct ring {
    struct trb *trb;
    uint32_t index;
    uint32_t cycle;
};

static uintptr_t base;              /* the capability part */
static uintptr_t op;                /* the operational part */
static uintptr_t rt;                /* the runtime part */
static volatile uint32_t *doorbells;
static int nports;
static int ctx_size = 32;        /* 32 or 64 B per context - HCCPARAMS1 says which */

static uint64_t dcbaa[SLOTS + 1] __attribute__((aligned(64)));
static struct trb cmd_trb[TRB_CHAIN] __attribute__((aligned(64)));
static struct trb evt_trb[TRB_CHAIN] __attribute__((aligned(64)));
static uint32_t erst[4] __attribute__((aligned(64)));
static uint8_t dev_ctx[SLOTS][CTX_BYTES] __attribute__((aligned(64)));
static uint8_t in_ctx[SLOTS][CTX_BYTES] __attribute__((aligned(64)));
/* Transfer rings: [0] endpoint zero, [1] the interrupt one */
static struct trb ep_trb[SLOTS][2][TRB_CHAIN] __attribute__((aligned(64)));

static struct ring cmd_pier;
static struct ring ep_pier[SLOTS][2];
static uint32_t evt_index, evt_cycle;
static int armed[SLOTS];       /* is a read pending on the interrupt endpoint */

/* Buffer for control transfer data: the controller reaches for it itself */
static uint8_t buf[512] __attribute__((aligned(64)));

/* --- register access -------------------------------------------------- */

static uint32_t rd(uintptr_t addr)
{
    return *(volatile uint32_t *)addr;
}

static void wr(uintptr_t addr, uint32_t v)
{
    *(volatile uint32_t *)addr = v;
}

/* 64-bit registers may be written in two halves, and that is exactly
   what we do - otherwise the same code would not work in 32-bit mode */
static void wr64(uintptr_t addr, uint64_t v)
{
    wr(addr, (uint32_t)v);
    wr(addr + 4, (uint32_t)(v >> 32));
}

static uint64_t fiz(const void *p)
{
    return (uint64_t)(uintptr_t)p;
}

static void wait_ms(uint32_t ms)
{
    uint32_t az = pit_ticks() + (ms + 9) / 10 + 1;
    while (pit_ticks() < az)
        io_wait();
}

static uintptr_t port_reg(int port)
{
    return op + OP_PORTSC + (uintptr_t)port * 0x10;
}

/* A write to PORTSC has to avoid the bits that a write CLEARS: the
   change notifications and the port enable itself. Otherwise a
   read-modify-write would disable the port while merely setting
   something else. */
static uint32_t portsc_safe(uint32_t v)
{
    return v & ~(PORT_ZMIANY | PORT_PED);
}

/* --- rings ------------------------------------------------------------ */

static void ring_setup(struct ring *p, struct trb *trb)
{
    memset(trb, 0, sizeof(struct trb) * TRB_CHAIN);
    p->trb = trb;
    p->index = 0;
    p->cycle = 1;

    /* The last TRB is a jump back to the start. The "toggle cycle" bit
       makes the meaning of the cycle bit flip on every turn - and that
       is why a ring never has to be cleared. */
    struct trb *jump = &trb[TRB_CHAIN - 1];
    jump->p0 = (uint32_t)fiz(trb);
    jump->p1 = (uint32_t)(fiz(trb) >> 32);
    jump->status = 0;
    jump->control = ((uint32_t)TRB_LINK << 10) | TRB_TC;
}

static void insert(struct ring *p, uint64_t param, uint32_t status,
                 uint32_t control)
{
    struct trb *t = &p->trb[p->index];
    t->p0 = (uint32_t)param;
    t->p1 = (uint32_t)(param >> 32);
    t->status = status;
    /* The cycle bit goes last: only it hands the TRB over to the
       controller, so the rest of the fields must already be in it */
    t->control = control | p->cycle;

    p->index++;
    if (p->index == TRB_CHAIN - 1) {
        struct trb *jump = &p->trb[TRB_CHAIN - 1];
        jump->control = (jump->control & ~1u) | p->cycle;
        p->index = 0;
        p->cycle ^= 1;
    }
}

/* Takes one event off the event ring. 1 = there was one. */
static int event(struct trb *result)
{
    struct trb *t = &evt_trb[evt_index];
    if ((t->control & 1u) != evt_cycle)
        return 0;                        /* not ours yet */

    result->p0 = t->p0;
    result->p1 = t->p1;
    result->status = t->status;
    result->control = t->control;

    evt_index++;
    if (evt_index == TRB_CHAIN) {
        evt_index = 0;
        evt_cycle ^= 1;
    }
    /* The controller has to know how far we have read - otherwise
       after a full turn it would overwrite events nobody saw */
    wr64(rt + RT_ERDP, fiz(&evt_trb[evt_index]) | 0x8);
    return 1;
}

/* Waits for an event of a given type. Others along the way are lost,
   and that is how it should be: port change notifications, which
   nobody ordered and nobody asks about, also travel through this
   ring. */
static int wait_for(int kind, struct trb *result, uint32_t ms)
{
    uint32_t az = pit_ticks() + (ms + 9) / 10 + 1;
    while (pit_ticks() < az) {
        struct trb t;
        if (!event(&t)) {
            io_wait();
            continue;
        }
        if ((int)((t.control >> 10) & 0x3F) == kind) {
            *result = t;
            return 1;
        }
    }
    return 0;
}

/* Places a command and waits for its result. Returns the completion
   code (CODE_OK when all is well) or -1 when nothing arrived. */
static int command(uint64_t param, uint32_t control, struct trb *reply)
{
    insert(&cmd_pier, param, 0, control);
    doorbells[0] = 0;                      /* the command ring doorbell */

    struct trb t;
    if (!wait_for(TRB_EV_COMMAND, &t, 1000))
        return -1;
    if (reply)
        *reply = t;
    return (int)(t.status >> 24);
}

/* --- device and endpoint contexts ------------------------------------ */

/* Context number 'i' in a set: zero is the control context (in the
   input set only), then the device one, then the endpoints */
static uint32_t *info(uint8_t *set, int i)
{
    return (uint32_t *)(set + (uintptr_t)i * (uintptr_t)ctx_size);
}

/* An endpoint's number in a context: zero has 1, further IN ones have 2n+1 */
static int dci(int ep)
{
    return ep ? ep * 2 + 1 : 1;
}

/* --- bringing it up --------------------------------------------------- */

int xhci_ports(void)
{
    return nports;
}

int xhci_init(void)
{
    struct pci_dev dev;
    /* Class 0x0C (serial bus), subclass 0x03 (USB),
       programming interface 0x30 (xHCI) */
    if (!pci_find_class(0x0C, 0x03, 0x30, &dev))
        return 0;

    /* xHCI sits in memory - BAR0, possibly 64-bit (bits 2:1 equal to 2
       mean the upper half of the address is in BAR1) */
    uint64_t addr = dev.bar[0] & ~0xFULL;
    if (dev.bar[0] & 0x01)
        return 0;                        /* this is not a memory window */
    if (((dev.bar[0] >> 1) & 0x3) == 0x2)
        addr |= (uint64_t)dev.bar[1] << 32;
    if (!addr || addr > 0xFFFFFFFFull)
        return 0;                        /* we would not reach higher anyway */

    if (MAP_PHYS((uint32_t)addr, 0x10000) < 0)
        return 0;
    pci_enable_bus_master(&dev);

    base = (uintptr_t)addr;
    op = base + (rd(base + CAP_CAPLENGTH) & 0xFF);
    rt = base + (rd(base + CAP_RTSOFF) & ~0x1Fu);
    doorbells = (volatile uint32_t *)(base + (rd(base + CAP_DBOFF) & ~0x3u));

    uint32_t hcs1 = rd(base + CAP_HCSPARAMS1);
    nports = (int)((hcs1 >> 24) & 0xFF);
    if (nports > 32)
        nports = 32;
    ctx_size = (rd(base + CAP_HCCPARAMS1) & 0x4) ? 64 : 32;

    /* Stop and zero the controller - after the BIOS it may be in any
       state, and reset is the only state we know */
    wr(op + OP_USBCMD, rd(op + OP_USBCMD) & ~(uint32_t)CMD_RS);
    for (int i = 0; i < 200 && !(rd(op + OP_USBSTS) & STS_HCH); i++)
        wait_ms(1);
    wr(op + OP_USBCMD, CMD_HCRST);
    for (int i = 0; i < 200 && (rd(op + OP_USBCMD) & CMD_HCRST); i++)
        wait_ms(1);
    for (int i = 0; i < 200 && (rd(op + OP_USBSTS) & STS_CNR); i++)
        wait_ms(1);
    if (rd(op + OP_USBCMD) & CMD_HCRST)
        return 0;

    memset(dcbaa, 0, sizeof(dcbaa));
    memset(dev_ctx, 0, sizeof(dev_ctx));
    memset(in_ctx, 0, sizeof(in_ctx));
    for (int s = 0; s < SLOTS; s++)
        for (int e = 0; e < 2; e++)
            ring_setup(&ep_pier[s][e], ep_trb[s][e]);

    wr(op + OP_CONFIG, SLOTS);
    wr64(op + OP_DCBAAP, fiz(dcbaa));

    ring_setup(&cmd_pier, cmd_trb);
    /* Bit zero of CRCR is the cycle bit the controller starts reading
       the command ring with - it has to match ours */
    wr64(op + OP_CRCR, fiz(cmd_trb) | 1);

    memset(evt_trb, 0, sizeof(evt_trb));
    evt_index = 0;
    evt_cycle = 1;
    erst[0] = (uint32_t)fiz(evt_trb);
    erst[1] = (uint32_t)(fiz(evt_trb) >> 32);
    erst[2] = TRB_CHAIN;
    erst[3] = 0;
    wr(rt + RT_ERSTSZ, 1);
    wr64(rt + RT_ERDP, fiz(evt_trb) | 0x8);
    wr64(rt + RT_ERSTBA, fiz(erst));

    wr(op + OP_DNCTRL, 0);
    wr(op + OP_USBCMD, CMD_RS);          /* without CMD_INTE - we poll */
    for (int i = 0; i < 200 && (rd(op + OP_USBSTS) & STS_HCH); i++)
        wait_ms(1);
    if (rd(op + OP_USBSTS) & STS_HCH)
        return 0;

    /* Port power: without it nothing on a port will ever report in */
    for (int i = 0; i < nports; i++) {
        uint32_t v = rd(port_reg(i));
        if (!(v & PORT_PP))
            wr(port_reg(i), portsc_safe(v) | PORT_PP);
    }
    wait_ms(20);
    return 1;
}

/* --- attaching a device ----------------------------------------------- */

/* The default endpoint zero packet size follows from the port speed.
   At full speed it may differ - we correct it once the device says so
   itself in its descriptor. */
static int packet_for_speed(int speed)
{
    switch (speed) {
    case 2:  return 8;                   /* low */
    case 3:  return 64;                  /* high */
    case 4:
    case 5:  return 512;                 /* super */
    default: return 8;                   /* full: we start with eight */
    }
}

static int port_reset(int port)
{
    uintptr_t reg = port_reg(port);
    uint32_t v = rd(reg);
    if (!(v & PORT_CCS))
        return 0;
    if (v & PORT_PED)
        return 1;                        /* the port reported in by itself (USB 3) */

    wr(reg, portsc_safe(v) | PORT_PR);
    for (int i = 0; i < 100; i++) {
        wait_ms(2);
        v = rd(reg);
        if (!(v & PORT_CCS))
            return 0;                    /* the device vanished in the meantime */
        if (v & PORT_PED)
            break;
    }
    if (!(v & PORT_PED))
        return 0;

    wr(reg, portsc_safe(v) | PORT_ZMIANY);   /* clear the notifications */
    wait_ms(10);
    return 1;
}

/* Assembles the input set for a command: what we are adding, the
   device context and the endpoint zero context. */
static void make_input(int slot, int port, int speed, int maxpacket)
{
    uint8_t *we = in_ctx[slot - 1];
    memset(we, 0, CTX_BYTES);

    uint32_t *control = info(we, 0);
    control[1] = 0x3;                 /* we add the device context and endpoint 0 */

    uint32_t *device = info(we, 1);
    device[0] = ((uint32_t)speed << 20) | (1u << 27);  /* one endpoint */
    device[1] = (uint32_t)(port + 1) << 16;

    uint32_t *punkt0 = info(we, 2);
    punkt0[1] = (3u << 1) |              /* three attempts on error */
                (4u << 3) |              /* type: control */
                ((uint32_t)maxpacket << 16);
    uint64_t p = fiz(ep_trb[slot - 1][0]) | 1;   /* the producer's cycle bit */
    punkt0[2] = (uint32_t)p;
    punkt0[3] = (uint32_t)(p >> 32);
    punkt0[4] = 8;                       /* the average TRB length */
}

int xhci_attach(int port)
{
    if (!base || port < 0 || port >= nports)
        return -1;
    if (!port_reset(port))
        return -1;

    struct trb reply;
    if (command(0, (uint32_t)TRB_ENABLE_SLOT << 10, &reply) != CODE_OK)
        return -1;
    int slot = (int)(reply.control >> 24);
    if (slot < 1 || slot > SLOTS)
        return -1;

    int speed = (int)((rd(port_reg(port)) >> PORT_SPEED_SH) & 0xF);
    int maxpacket = packet_for_speed(speed);

    dcbaa[slot] = fiz(dev_ctx[slot - 1]);
    memset(dev_ctx[slot - 1], 0, CTX_BYTES);
    ring_setup(&ep_pier[slot - 1][0], ep_trb[slot - 1][0]);

    make_input(slot, port, speed, maxpacket);
    if (command(fiz(in_ctx[slot - 1]),
                  ((uint32_t)TRB_ADDRESS_DEV << 10) | ((uint32_t)slot << 24),
                  (struct trb *)0) != CODE_OK)
        return -1;

    /* The first eight bytes of the device descriptor say how large the
       packets endpoint zero accepts are. At full speed that is
       sometimes more than eight - and then the controller has to be
       told, otherwise longer answers fall apart across packets. */
    uint8_t setup[8] = { 0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 8, 0x00 };
    uint8_t info8[8];
    if (xhci_control(slot, setup, info8, 8, 1) < 8)
        return -1;
    if (info8[7] && info8[7] != maxpacket) {
        uint8_t *we = in_ctx[slot - 1];
        uint32_t *control = info(we, 0);
        control[0] = 0;
        control[1] = 0x2;             /* we correct endpoint zero only */
        uint32_t *punkt0 = info(we, 2);
        punkt0[1] = (punkt0[1] & 0xFFFFu) | ((uint32_t)info8[7] << 16);
        command(fiz(we),
                  ((uint32_t)TRB_EVAL_CTX << 10) | ((uint32_t)slot << 24),
                  (struct trb *)0);
    }
    return slot;
}

/* --- transfers ---------------------------------------------------------- */

int xhci_control(int slot, const void *setup, void *data, int len, int in)
{
    if (!base || slot < 1 || slot > SLOTS)
        return -1;
    if (len < 0 || len > (int)sizeof(buf))
        return -1;

    const uint8_t *s = setup;
    uint64_t query = 0;
    for (int i = 0; i < 8; i++)
        query |= (uint64_t)s[i] << (i * 8);

    if (len && !in)
        memcpy(buf, data, (uint32_t)len);

    struct ring *p = &ep_pier[slot - 1][0];
    /* Transfer type: 0 no data, 2 data out, 3 data in */
    uint32_t kind = len ? (in ? 3u : 2u) : 0u;
    insert(p, query, 8, ((uint32_t)TRB_SETUP << 10) | TRB_IDT |
                          (kind << 16));
    if (len)
        insert(p, fiz(buf), (uint32_t)len,
             ((uint32_t)TRB_DATA << 10) | TRB_IOC | TRB_ISP |
             ((uint32_t)(in ? 1 : 0) << 16));
    insert(p, 0, 0, ((uint32_t)TRB_STATUS << 10) | TRB_IOC |
                  ((uint32_t)(in ? 0 : 1) << 16));

    doorbells[slot] = 1;                   /* endpoint zero has number 1 */

    int passed = 0;
    if (len) {
        struct trb t;
        if (!wait_for(TRB_EV_TRANSFER, &t, 1000))
            return -1;
        int code = (int)(t.status >> 24);
        if (code != CODE_OK && code != CODE_SHORT)
            return -1;
        /* The controller reports HOW MUCH IS LEFT, not how much went through */
        passed = len - (int)(t.status & 0xFFFFFF);
        if (in)
            memcpy(data, buf, (uint32_t)(passed > 0 ? passed : 0));
    }

    struct trb t;
    if (!wait_for(TRB_EV_TRANSFER, &t, 1000))
        return -1;
    int code = (int)(t.status >> 24);
    if (code != CODE_OK && code != CODE_SHORT)
        return -1;
    return passed;
}

int xhci_int_endpoint(int slot, int ep, int maxpacket, int interval)
{
    if (!base || slot < 1 || slot > SLOTS || ep < 1)
        return -1;
    int number = dci(ep);
    if (number > 31)
        return -1;

    ring_setup(&ep_pier[slot - 1][1], ep_trb[slot - 1][1]);
    armed[slot - 1] = 0;

    uint8_t *we = in_ctx[slot - 1];
    memset(we, 0, CTX_BYTES);
    uint32_t *control = info(we, 0);
    control[1] = 0x1u | (1u << number);   /* the device plus this endpoint */

    /* The device context has to be given afresh, because the number of
       endpoints changes - the controller will not work it out itself */
    uint32_t *src = info(dev_ctx[slot - 1], 0);
    uint32_t *device = info(we, 1);
    device[0] = (src[0] & 0x07FFFFFFu) | ((uint32_t)number << 27);
    device[1] = src[1];

    uint32_t *punkt = info(we, number + 1);
    punkt[0] = (uint32_t)(interval > 0 ? interval : 8) << 16;
    punkt[1] = (3u << 1) |               /* three attempts on error */
               (7u << 3) |               /* type: interrupt in */
               ((uint32_t)maxpacket << 16);
    uint64_t p = fiz(ep_trb[slot - 1][1]) | 1;
    punkt[2] = (uint32_t)p;
    punkt[3] = (uint32_t)(p >> 32);
    punkt[4] = (uint32_t)maxpacket | ((uint32_t)maxpacket << 16);

    if (command(fiz(we),
                  ((uint32_t)TRB_CONFIG_EP << 10) | ((uint32_t)slot << 24),
                  (struct trb *)0) != CODE_OK)
        return -1;
    return 0;
}

/* Looking into an interrupt endpoint.
 *
 * Just as with UHCI: we arm the read once and do not abandon it. The
 * only difference is that there the descriptor lay in a queue, while
 * here the TRB lies in a transfer ring - and we learn of its
 * completion from an event rather than from a bit in the descriptor
 * itself.
 */
int xhci_int_in(int slot, int ep, void *data, int len)
{
    if (!base || slot < 1 || slot > SLOTS || len <= 0 ||
        len > (int)sizeof(buf))
        return -1;

    if (!armed[slot - 1]) {
        insert(&ep_pier[slot - 1][1], fiz(buf), (uint32_t)len,
             ((uint32_t)TRB_NORMAL << 10) | TRB_IOC | TRB_ISP);
        doorbells[slot] = (uint32_t)dci(ep);
        armed[slot - 1] = 1;
        return 0;                        /* the answer will come in a moment */
    }

    struct trb t;
    if (!event(&t))
        return 0;                        /* the device is silent - we keep waiting */
    armed[slot - 1] = 0;
    if ((int)((t.control >> 10) & 0x3F) != TRB_EV_TRANSFER)
        return 0;                        /* a port notification, not our data */

    int code = (int)(t.status >> 24);
    if (code != CODE_OK && code != CODE_SHORT)
        return -1;
    int passed = len - (int)(t.status & 0xFFFFFF);
    if (passed > 0)
        memcpy(data, buf, (uint32_t)passed);
    return passed;
}
