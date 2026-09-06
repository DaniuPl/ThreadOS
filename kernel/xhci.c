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
#include "include/spinlock.h"
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
#define TRB_DISABLE_SLOT 10
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
/* Transfer rings: [0] endpoint zero, [1] the interrupt one, [2] bulk
   in, [3] bulk out. A ring per endpoint, because that is what the
   controller expects: it keeps its own read position in each of them
   and a shared ring would make two endpoints trip over each other. */
#define RINGS 4
static struct trb ep_trb[SLOTS][RINGS][TRB_CHAIN] __attribute__((aligned(64)));

static struct ring cmd_pier;
static struct ring ep_pier[SLOTS][RINGS];
static uint32_t evt_index, evt_cycle;
static int armed[SLOTS];       /* is a read pending on the interrupt endpoint */
static int slot_speed[SLOTS];  /* what speed the device answered at */

/* Buffer for control transfer data: the controller reaches for it itself */
/* One buffer per device, not one for the whole controller.
 *
 * While a keyboard was the only thing on this bus, one was enough. Two
 * devices have two reads in flight at once, and one buffer means each
 * one's report lands where the other is about to read it - which shows
 * up as a keyboard that goes silent the moment a mouse is plugged in. */
static uint8_t buf[SLOTS][512] __attribute__((aligned(64)));

/* Transfers that have finished, waiting for the device they belong to.
 *
 * There is ONE event ring for the whole controller, so whoever looks
 * at it first sees everybody's completions, and taking one that is not
 * yours loses that device's report for good. So an event is filed
 * under its slot and waits there.
 *
 * A QUEUE per slot rather than one place, because completions do
 * arrive together: a control transfer finishes twice, once for the
 * data and once for the status, and with a single place the second
 * would overwrite the first and the caller would wait forever for a
 * thing it had already been told. */
#define COMPS 8
struct comp { int code, left; };
static struct {
    struct comp q[COMPS];
    int head, tail;
} done[SLOTS];

/* Non-transfer events - a command finishing, a port changing - kept
   the same way and for the same reason. */
#define OTHERS 8
static struct trb other_q[OTHERS];
static int other_n;

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

/* One lock for the rings, and one turn for a whole command.
 *
 * There used to be one thread doing all of this: enumeration ran at
 * start-up and the keyboard's poll thread began afterwards. Now a
 * thread watches the sockets, so a stick can be greeted while the
 * keyboard is being read - two threads on one controller.
 *
 * The two need different guards. Putting a TRB into a ring and taking
 * an event off it are short - a spinlock is right, and none of it may
 * sleep. A whole COMMAND is not short: it waits for its answer, and
 * two commands in flight at once would find each other's completions,
 * because a command event says which command ring it came from and not
 * whose it was. So commands take turns, and the waiting is done by the
 * same clock the rest of this driver waits on.
 */
static struct spinlock ring_lock = SPINLOCK_FREE;
static volatile uint32_t cmd_turn;

static void insert(struct ring *p, uint64_t param, uint32_t status,
                 uint32_t control)
{
    irqflags_t flags = spin_lock_irq(&ring_lock);
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
    spin_unlock_irq(&ring_lock, flags);
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
/* Empties the event ring, filing everything where its owner will find
   it. A transfer goes under its slot, which is the top byte of the
   control word - and that byte is the only reason two devices can
   share one ring at all. */
static void sort_events(void)
{
    struct trb t;
    irqflags_t flags = spin_lock_irq(&ring_lock);

    while (event(&t)) {
        if ((int)((t.control >> 10) & 0x3F) != TRB_EV_TRANSFER) {
            if (other_n < OTHERS)
                other_q[other_n++] = t;
            continue;
        }
        int s = (int)((t.control >> 24) & 0xFF);
        if (s < 1 || s > SLOTS)
            continue;
        int next = (done[s - 1].head + 1) % COMPS;
        if (next == done[s - 1].tail)
            continue;                    /* nobody is collecting - drop it */
        done[s - 1].q[done[s - 1].head].code = (int)(t.status >> 24);
        done[s - 1].q[done[s - 1].head].left = (int)(t.status & 0xFFFFFF);
        done[s - 1].head = next;
    }
    spin_unlock_irq(&ring_lock, flags);
}

/* Takes one finished transfer of this slot's, if there is one. */
static int take_comp(int slot, struct comp *out)
{
    irqflags_t flags = spin_lock_irq(&ring_lock);
    int got = 0;

    if (done[slot - 1].tail != done[slot - 1].head) {
        *out = done[slot - 1].q[done[slot - 1].tail];
        done[slot - 1].tail = (done[slot - 1].tail + 1) % COMPS;
        got = 1;
    }
    spin_unlock_irq(&ring_lock, flags);
    return got;
}

/* Waits for one of this slot's transfers, filing everybody else's on
   the way rather than dropping them. */
static int wait_slot(int slot, struct comp *out, uint32_t ms)
{
    uint32_t az = pit_ticks() + (ms + 9) / 10 + 1;
    while (pit_ticks() < az) {
        sort_events();
        if (take_comp(slot, out))
            return 1;
        io_wait();
    }
    return 0;
}

static int wait_for(int kind, struct trb *result, uint32_t ms)
{
    uint32_t az = pit_ticks() + (ms + 9) / 10 + 1;
    while (pit_ticks() < az) {
        sort_events();
        for (int i = 0; i < other_n; i++) {
            if ((int)((other_q[i].control >> 10) & 0x3F) != kind)
                continue;
            *result = other_q[i];
            for (int j = i + 1; j < other_n; j++)
                other_q[j - 1] = other_q[j];
            other_n--;
            return 1;
        }
        io_wait();
    }
    return 0;
}

/* Places a command and waits for its result. Returns the completion
   code (CODE_OK when all is well) or -1 when nothing arrived. */
static int command(uint64_t param, uint32_t control, struct trb *reply)
{
    struct trb t;
    int code;

    /* Wait for our turn - at most a second, because a command that
       never finishes must not stop the machine for good. */
    uint32_t until = pit_ticks() + 100;
    while (pit_ticks() < until) {
        irqflags_t flags = spin_lock_irq(&ring_lock);
        int free_now = !cmd_turn;
        if (free_now)
            cmd_turn = 1;
        spin_unlock_irq(&ring_lock, flags);
        if (free_now)
            break;
        io_wait();
    }

    insert(&cmd_pier, param, 0, control);
    doorbells[0] = 0;                      /* the command ring doorbell */

    if (!wait_for(TRB_EV_COMMAND, &t, 1000)) {
        cmd_turn = 0;
        return -1;
    }
    if (reply)
        *reply = t;
    code = (int)(t.status >> 24);
    cmd_turn = 0;
    return code;
}

/* --- device and endpoint contexts ------------------------------------ */

/* Context number 'i' in a set: zero is the control context (in the
   input set only), then the device one, then the endpoints */
static uint32_t *info(uint8_t *set, int i)
{
    return (uint32_t *)(set + (uintptr_t)i * (uintptr_t)ctx_size);
}

/* An endpoint's number in a context: zero has 1, further IN ones have
   2n+1 and OUT ones 2n. The direction is part of the number, because
   in USB an "endpoint 2" going in and an "endpoint 2" going out are
   two different endpoints that merely share a digit. */
static int dci(int ep)
{
    return ep ? ep * 2 + 1 : 1;
}

static int dci_dir(int ep, int in)
{
    return ep ? ep * 2 + (in ? 1 : 0) : 1;
}

/* --- bringing it up --------------------------------------------------- */

int xhci_ports(void)
{
    return nports;
}

/* Where the start-up stopped, if it did.
 *
 * "No USB" is not an answer anybody can act on. A controller that is
 * on the bus but did not start is a different fault from one that is
 * not there at all - and on a real machine that difference is the
 * whole diagnosis, so it is written down and printed by 'sprzet'. */
static const char *gave_up;

static int give_up(const char *why)
{
    gave_up = why;
    return 0;
}

const char *xhci_why(void)
{
    return gave_up;
}

int xhci_init(void)
{
    struct pci_dev dev;
    /* Class 0x0C (serial bus), subclass 0x03 (USB),
       programming interface 0x30 (xHCI) */
    if (!pci_find_class(0x0C, 0x03, 0x30, &dev))
        return give_up("nie ma kontrolera xHCI na PCI");

    /* xHCI sits in memory - BAR0, possibly 64-bit (bits 2:1 equal to 2
       mean the upper half of the address is in BAR1) */
    uint64_t addr = dev.bar[0] & ~0xFULL;
    if (dev.bar[0] & 0x01)
        return give_up("BAR0 kontrolera nie jest oknem w pamieci");
    if (((dev.bar[0] >> 1) & 0x3) == 0x2)
        addr |= (uint64_t)dev.bar[1] << 32;
    if (!addr)
        return give_up("firmware nie przydzielil kontrolerowi adresu");
#ifndef __x86_64__
    /* In 32-bit mode there is nowhere to map it to: the whole address
       space ends where this address begins. UEFI firmware likes to put
       64-bit windows up there, and that is why the same machine has
       USB under the old BIOS and none under UEFI - in this kernel. */
    if (addr > 0xFFFFFFFFull)
        return give_up("kontroler siedzi powyzej 4 GiB (tryb 32-bitowy)");
#endif

    if (MAP_PHYS((uintptr_t)addr, 0x10000) < 0)
        return give_up("nie udalo sie zmapowac okna kontrolera");
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
        return give_up("kontroler nie chce sie zresetowac");

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
        return give_up("kontroler nie chce ruszyc po resecie");

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
/* The description of a device for the controller.
 *
 * Two of these fields are the whole difference between a device in a
 * socket on the machine and one in a socket on a hub plugged into it.
 *
 * The ROUTE STRING says how to get there: four bits per tier of hubs,
 * the first tier in the lowest four. Zero means "straight on the root
 * port", which is what everything was until hubs.
 *
 * The TT fields say who answers for it when it is slower than the hub
 * it hangs off: a full-speed keyboard behind a high-speed hub is
 * reached through that hub's transaction translator, and the
 * controller has to be told which hub and which of its ports. Get
 * this wrong and the device is addressed and then silent, which is a
 * hard thing to look at and see. */
static void make_input(int slot, int port, int speed, int maxpacket,
                       uint32_t route, int tt_slot, int tt_port)
{
    uint8_t *we = in_ctx[slot - 1];
    memset(we, 0, CTX_BYTES);

    uint32_t *control = info(we, 0);
    control[1] = 0x3;                 /* we add the device context and endpoint 0 */

    uint32_t *device = info(we, 1);
    device[0] = (route & 0xFFFFFu) |
                ((uint32_t)speed << 20) | (1u << 27);  /* one endpoint */
    device[1] = (uint32_t)(port + 1) << 16;
    if (tt_slot > 0)
        device[2] = (uint32_t)tt_slot | ((uint32_t)tt_port << 8);

    uint32_t *punkt0 = info(we, 2);
    punkt0[1] = (3u << 1) |              /* three attempts on error */
                (4u << 3) |              /* type: control */
                ((uint32_t)maxpacket << 16);
    uint64_t p = fiz(ep_trb[slot - 1][0]) | 1;   /* the producer's cycle bit */
    punkt0[2] = (uint32_t)p;
    punkt0[3] = (uint32_t)(p >> 32);
    punkt0[4] = 8;                       /* the average TRB length */
}

/* A device on a root port: the controller resets the port itself and
   the route string is empty. */
int xhci_attach(int port)
{
    if (!base || port < 0 || port >= nports)
        return -1;
    if (!port_reset(port))
        return -1;

    int speed = (int)((rd(port_reg(port)) >> PORT_SPEED_SH) & 0xF);
    return xhci_address(port, 0, speed, 0, 0);
}

/* A device anywhere: on a root port (route 0) or behind hubs. The port
   has already been reset by whoever owns it - the controller for a
   root port, the hub above for anything else - and the speed is what
   that reset reported. */
int xhci_address(int port, uint32_t route, int speed, int tt_slot,
                 int tt_port)
{
    if (!base || port < 0 || port >= nports)
        return -1;

    struct trb reply;
    if (command(0, (uint32_t)TRB_ENABLE_SLOT << 10, &reply) != CODE_OK)
        return -1;
    int slot = (int)(reply.control >> 24);
    if (slot < 1 || slot > SLOTS)
        return -1;

    int maxpacket = packet_for_speed(speed);

    dcbaa[slot] = fiz(dev_ctx[slot - 1]);
    memset(dev_ctx[slot - 1], 0, CTX_BYTES);
    ring_setup(&ep_pier[slot - 1][0], ep_trb[slot - 1][0]);

    slot_speed[slot - 1] = speed;
    make_input(slot, port, speed, maxpacket, route, tt_slot, tt_port);
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
    if (len < 0 || len > (int)sizeof(buf[0]))
        return -1;
    uint8_t *mine = buf[slot - 1];

    const uint8_t *s = setup;
    uint64_t query = 0;
    for (int i = 0; i < 8; i++)
        query |= (uint64_t)s[i] << (i * 8);

    if (len && !in)
        memcpy(mine, data, (uint32_t)len);

    struct ring *p = &ep_pier[slot - 1][0];
    /* Transfer type: 0 no data, 2 data out, 3 data in */
    uint32_t kind = len ? (in ? 3u : 2u) : 0u;
    insert(p, query, 8, ((uint32_t)TRB_SETUP << 10) | TRB_IDT |
                          (kind << 16));
    if (len)
        insert(p, fiz(mine), (uint32_t)len,
             ((uint32_t)TRB_DATA << 10) | TRB_IOC | TRB_ISP |
             ((uint32_t)(in ? 1 : 0) << 16));
    insert(p, 0, 0, ((uint32_t)TRB_STATUS << 10) | TRB_IOC |
                  ((uint32_t)(in ? 0 : 1) << 16));

    doorbells[slot] = 1;                   /* endpoint zero has number 1 */

    int passed = 0;
    struct comp c;
    if (len) {
        if (!wait_slot(slot, &c, 1000))
            return -1;
        if (c.code != CODE_OK && c.code != CODE_SHORT)
            return -1;
        /* The controller reports HOW MUCH IS LEFT, not how much went through */
        passed = len - c.left;
        if (in)
            memcpy(data, mine, (uint32_t)(passed > 0 ? passed : 0));
    }

    /* The status stage - this slot's transfer like any other */
    if (!wait_slot(slot, &c, 1000))
        return -1;
    if (c.code != CODE_OK && c.code != CODE_SHORT)
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

    /* The interval in a descriptor and the interval in an endpoint
       context are not the same number, and passing one for the other
       is a bug that hides for years.
     *
     * A high-speed descriptor gives an exponent already (frames =
     * 2^(bInterval-1) microframes); a full- or low-speed one gives
     * MILLISECONDS, and a hub asks for two hundred and fifty-five of
     * them. Written into the context unchanged, that is 2^255
     * microframes - a polling interval longer than the machine's life,
     * which shows up as an endpoint that never answers. And an
     * endpoint that never answers is how a hub stops reporting that
     * somebody plugged something into it. */
    int period;
    if (slot_speed[slot - 1] >= 3) {         /* high speed and above */
        period = interval > 0 ? interval - 1 : 3;
        if (period < 0)
            period = 0;
    } else {                                  /* milliseconds */
        int ms = interval > 0 ? interval : 8;
        period = 3;                           /* 2^3 microframes = 1 ms */
        while ((1 << (period - 3)) < ms && period < 10)
            period++;
    }
    if (period > 15)
        period = 15;

    uint32_t *punkt = info(we, number + 1);
    punkt[0] = (uint32_t)period << 16;
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
        len > (int)sizeof(buf[0]))
        return -1;

    if (!armed[slot - 1]) {
        insert(&ep_pier[slot - 1][1], fiz(buf[slot - 1]), (uint32_t)len,
             ((uint32_t)TRB_NORMAL << 10) | TRB_IOC | TRB_ISP);
        doorbells[slot] = (uint32_t)dci(ep);
        armed[slot - 1] = 1;
        return 0;                        /* the answer will come in a moment */
    }

    sort_events();
    struct comp c;
    if (!take_comp(slot, &c))
        return 0;                        /* the device is silent - we keep waiting */
    armed[slot - 1] = 0;

    if (c.code != CODE_OK && c.code != CODE_SHORT)
        return -1;
    int passed = len - c.left;
    if (passed > 0)
        memcpy(data, buf[slot - 1], (uint32_t)passed);
    return passed;
}

/* --- bulk endpoints ----------------------------------------------------
 *
 * What a disk needs, and what a keyboard did not. The difference from
 * an interrupt endpoint is smaller than it looks: the same rings, the
 * same events, another number in the type field. Two differences do
 * matter.
 *
 * The first: a bulk transfer goes BOTH ways, so a device has two
 * endpoints and each of them needs a ring of its own. They are set up
 * with one command, because every Configure Endpoint command replaces
 * the whole description of the device - sending two commands in a row
 * would leave the second one having forgotten the first.
 *
 * The second: we wait for the answer here instead of returning "not
 * yet". A keyboard is asked whether it has anything to say; a disk is
 * told to read a sector, and there is nothing sensible to do until it
 * has.
 */
int xhci_bulk_endpoint(int slot, int in_ep, int out_ep, int maxpacket)
{
    if (!base || slot < 1 || slot > SLOTS || in_ep < 1 || out_ep < 1)
        return -1;

    int in_dci = dci_dir(in_ep, 1);
    int out_dci = dci_dir(out_ep, 0);
    if (in_dci > 31 || out_dci > 31)
        return -1;

    ring_setup(&ep_pier[slot - 1][2], ep_trb[slot - 1][2]);
    ring_setup(&ep_pier[slot - 1][3], ep_trb[slot - 1][3]);

    uint8_t *we = in_ctx[slot - 1];
    memset(we, 0, CTX_BYTES);
    uint32_t *control = info(we, 0);
    control[1] = 0x1u | (1u << in_dci) | (1u << out_dci);

    /* The device context says how many endpoints the controller is to
       keep room for - and that is the HIGHEST number in use, not how
       many there are. */
    int top = in_dci > out_dci ? in_dci : out_dci;
    uint32_t *src = info(dev_ctx[slot - 1], 0);
    uint32_t *device = info(we, 1);
    device[0] = (src[0] & 0x07FFFFFFu) | ((uint32_t)top << 27);
    device[1] = src[1];

    static const int kinds[2] = { 6, 2 };      /* bulk in, bulk out */
    int dcis[2] = { in_dci, out_dci };
    for (int i = 0; i < 2; i++) {
        uint32_t *punkt = info(we, dcis[i] + 1);
        punkt[0] = 0;                          /* bulk has no interval */
        punkt[1] = (3u << 1) |                 /* three attempts on error */
                   ((uint32_t)kinds[i] << 3) |
                   ((uint32_t)maxpacket << 16);
        uint64_t p = fiz(ep_trb[slot - 1][i == 0 ? 2 : 3]) | 1;
        punkt[2] = (uint32_t)p;
        punkt[3] = (uint32_t)(p >> 32);
        punkt[4] = (uint32_t)maxpacket;
    }

    if (command(fiz(we),
                ((uint32_t)TRB_CONFIG_EP << 10) | ((uint32_t)slot << 24),
                (struct trb *)0) != CODE_OK)
        return -1;
    return 0;
}

/* One transfer, in pieces the controller will accept.
 *
 * A single TRB may not describe a buffer that crosses a 64 KiB
 * boundary - not because of the bus but because of how the controller
 * counts. Nothing here asks for more than four kilobytes at a time,
 * but "nothing here does" is exactly the kind of assumption that
 * breaks a year later, so the split is done properly.
 */
static int bulk_transfer(int slot, int ep, void *data, int len, int in)
{
    if (!base || slot < 1 || slot > SLOTS || len < 0)
        return -1;

    struct ring *p = &ep_pier[slot - 1][in ? 2 : 3];
    uint8_t *q = data;
    int left = len;
    int total = 0;

    while (left > 0) {
        uint64_t phys = fiz(q);
        uint32_t room = 0x10000u - (uint32_t)(phys & 0xFFFFu);
        uint32_t n = (uint32_t)left;
        struct comp c;

        if (n > room)
            n = room;

        insert(p, phys, n, ((uint32_t)TRB_NORMAL << 10) | TRB_IOC | TRB_ISP);
        doorbells[slot] = (uint32_t)dci_dir(ep, in);

        if (!wait_slot(slot, &c, 5000))
            return -1;
        if (c.code != CODE_OK && c.code != CODE_SHORT)
            return -1;

        int passed = (int)n - c.left;
        if (passed < 0)
            passed = 0;
        total += passed;
        /* A packet shorter than asked for ends the transfer - that is
           how a device says "that is all I had". */
        if (passed < (int)n)
            break;

        q += n;
        left -= (int)n;
    }
    return total;
}

int xhci_bulk_out(int slot, int ep, const void *data, int len)
{
    return bulk_transfer(slot, ep, (void *)(uintptr_t)data, len, 0);
}

int xhci_bulk_in(int slot, int ep, void *data, int len)
{
    return bulk_transfer(slot, ep, data, len, 1);
}


/* --- what is in the socket right now ---------------------------------
 *
 * Enumeration at start-up answers "what was plugged in when we
 * booted". A machine somebody uses answers a different question all
 * day: what is plugged in NOW. Both come from the same bit - the
 * controller keeps the state of every port in a register - so
 * noticing a stick being pushed in costs one read, not a mechanism.
 */
int xhci_port_connected(int port)
{
    if (!base || port < 0 || port >= nports)
        return 0;
    return (rd(port_reg(port)) & PORT_CCS) ? 1 : 0;
}

/* Gives a slot back to the controller. Without this a machine that
   somebody plugs a stick into four times has no slots left - and the
   fifth one is simply not seen, which looks like a broken socket. */
int xhci_release(int slot)
{
    if (!base || slot < 1 || slot > SLOTS)
        return -1;

    command(0, ((uint32_t)TRB_DISABLE_SLOT << 10) | ((uint32_t)slot << 24),
            (struct trb *)0);
    dcbaa[slot] = 0;
    armed[slot - 1] = 0;
    done[slot - 1].head = done[slot - 1].tail = 0;
    return 0;
}


/* "This device is a hub."
 *
 * The controller has to be told, and not for tidiness: a slot context
 * without the hub bit describes something with no ports, and a
 * command to address a device BEHIND it is then a command about a
 * route that, as far as the controller is concerned, does not exist.
 * The number of ports goes in with it, and for a high-speed hub the
 * think time of its transaction translator - which is how long it
 * needs between the parts of a split transaction.
 */
int xhci_mark_hub(int slot, int ports, int multi_tt, int think_time)
{
    if (!base || slot < 1 || slot > SLOTS || ports < 1)
        return -1;

    uint8_t *we = in_ctx[slot - 1];
    memset(we, 0, CTX_BYTES);

    uint32_t *control = info(we, 0);
    control[1] = 0x1;                    /* the device context alone */

    /* Everything the controller already knows about this device stays;
       we only add what it did not know. */
    uint32_t *src = info(dev_ctx[slot - 1], 0);
    uint32_t *device = info(we, 1);
    device[0] = src[0] | (1u << 26) | (multi_tt ? (1u << 25) : 0);
    device[1] = (src[1] & 0x00FFFFFFu) | ((uint32_t)ports << 24);
    device[2] = (src[2] & ~0x00030000u) |
                (((uint32_t)think_time & 3) << 16);

    return command(fiz(we),
                   ((uint32_t)TRB_CONFIG_EP << 10) | ((uint32_t)slot << 24),
                   (struct trb *)0) == CODE_OK ? 0 : -1;
}


/* The speed the controller reports for a root port, in its own
   numbering: 1 full, 2 low, 3 high, 4 super. */
int xhci_speed(int port)
{
    if (!base || port < 0 || port >= nports)
        return 0;
    return (int)((rd(port_reg(port)) >> PORT_SPEED_SH) & 0xF);
}
