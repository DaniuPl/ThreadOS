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
#include "include/task.h"
#include "include/pci.h"
#include "include/devices.h"
#include "include/printk.h"
#include "include/string.h"
#include "include/pit.h"
#include "include/io.h"
#include "include/pmm.h"

/* The registers sit in memory, so they have to be mapped first. The
   address comes from a BAR rather than from anything fixed - that is
   the only thing in this file that depends on the processor mode. */
#include "../kernel64/paging64.h"
#define MAP_PHYS(a, n)   paging64_map_phys((a), (n))

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
#define STS_HSE         0x0004      /* host system error: it lost the bus */
#define STS_CNR         0x0800      /* the controller is not ready yet */
#define STS_HCE         0x1000      /* host controller error: it gave up */

/* The USB Legacy Support capability, in the controller's own registers */
#define LEG_ID          1
#define LEG_BIOS_OWNED  (1u << 16)
#define LEG_OS_OWNED    (1u << 24)
/* In its control word: what to keep (the reserved bits) when switching
   every SMI enable off, and the three SMI events to acknowledge */
#define LEG_SMI_KEEP    ((0x7u << 1) | (0xFFu << 5) | (0x7u << 17))
#define LEG_SMI_EVENTS  (0x7u << 29)

/* Intel's port routing, in the PCI configuration of its xHCI */
#define INTEL_XUSB2PR    0xD0       /* USB 2.0 wires: 1 = to xHCI */
#define INTEL_USB2PRM    0xD4       /* ...which of them may be moved */
#define INTEL_USB3_PSSEN 0xD8       /* SuperSpeed terminations on */
#define INTEL_USB3PRM    0xDC       /* ...which of them may be switched */

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
#define TRB_ISOCH       5           /* one TD of an isochronous endpoint */
#define TRB_STOP_EP     15
#define TRB_EV_TRANSFER 32         /* event: a transfer is finished */
#define TRB_EV_COMMAND 33         /* event: a command has been executed */

#define TRB_IOC         (1u << 5)   /* tell us when it is done */
#define TRB_IDT         (1u << 6)   /* the data sits inside the TRB itself */
#define TRB_ISP         (1u << 2)   /* a shorter packet also ends the transfer */
#define TRB_TC          (1u << 1)   /* the jump flips the cycle bit */

#define CODE_OK      1
#define CODE_NO_SLOTS   9           /* every device slot is taken */
#define CODE_SHORT      13          /* less arrived than we asked for - that is not an error */
#define CODE_BANDWIDTH  8           /* no room on the bus for that endpoint */
#define TRB_SIA         (1u << 31)  /* isochronous: as soon as possible */

/* Devices at once. Four was enough for a keyboard and a stick in QEMU;
   a laptop has a webcam, a card reader, a fingerprint reader and the
   Bluetooth half of its wireless card on this bus before anybody plugs
   anything in, and the fifth device simply got no slot. */
#define SLOTS          16
#define TRB_CHAIN      16          /* of which the last is a jump back to the start */
#define CTX_BYTES      2112        /* the largest possible context set */
#define DEV_CTX_BYTES  2048        /* a device context: 32 contexts of 64 B at most */

struct trb {
    volatile uint32_t p0, p1, status, control;
};

/* A ring from the writer's side: where to put the next TRB and with
   which cycle bit */
struct ring {
    struct trb *trb;
    uint32_t index;
    uint32_t cycle;
    uint32_t size;          /* TRBs, the jump back included */
};

static uintptr_t base;              /* the capability part */
static uintptr_t op;                /* the operational part */
static uintptr_t rt;                /* the runtime part */
static volatile uint32_t *doorbells;
static int nports;
static int ctx_size = 32;        /* 32 or 64 B per context - HCCPARAMS1 says which */

/* Where these lie matters to a real controller, though not to QEMU's:
   a context may not cross a page, and a ring may not cross 64 KiB
   (table 6-1 of the specification). Aligning each to its own size
   keeps every one inside. An input context is 2112 bytes, so each
   gets a page; two 2048-byte device contexts share one. */
static uint64_t dcbaa[SLOTS + 1] __attribute__((aligned(256)));
static struct trb cmd_trb[TRB_CHAIN] __attribute__((aligned(256)));
/* The event ring is longer than the others: a camera's endpoint
   finishes up to eight TDs a millisecond, and the controller is read
   every ten. A full event ring is where a controller starts missing
   service. Aligned to its size, it cannot cross 64 KiB. */
#define EVT_TRBS       1024
static struct trb evt_trb[EVT_TRBS] __attribute__((aligned(16384)));
static uint32_t erst[4] __attribute__((aligned(64)));
static uint8_t dev_ctx[SLOTS][DEV_CTX_BYTES] __attribute__((aligned(4096)));
static uint8_t in_ctx[SLOTS][4096] __attribute__((aligned(4096)));

/* Scratchpad pages: memory the controller keeps state of its own in.
   QEMU's asks for none. Intel's ask for dozens, and one that asked and
   got nothing has nowhere to put a device - it fails the first Enable
   Slot or stops with an internal error, and every socket on the
   machine looks empty. The array of their addresses goes into entry 0
   of the context array, which is why slots are counted from 1. */
#define SCRATCH_MAX 256
static uint64_t scratch[SCRATCH_MAX] __attribute__((aligned(2048)));
static int nscratch;

/* Transfer rings: [0] endpoint zero, [1] the first interrupt one, [2]
   bulk in, [3] bulk out, [4] and [5] two more interrupt ones. A ring
   per endpoint, because that is what the controller expects: it keeps
   its own read position in each of them and a shared ring would make
   two endpoints trip over each other.

   More than one interrupt endpoint, because a keyboard with media keys
   is two devices in one: the keys on one interface, the loudness and
   the playback keys on another, each with its own endpoint. */
#define RINGS 6
#define INT_EPS 3
static int int_ring(int k) { return k == 0 ? 1 : 3 + k; }
static struct trb ep_trb[SLOTS][RINGS][TRB_CHAIN] __attribute__((aligned(256)));

static struct ring cmd_pier;
static struct ring ep_pier[SLOTS][RINGS];
static uint32_t evt_index, evt_cycle;
static int int_dci[SLOTS][INT_EPS];   /* each interrupt endpoint's number */
static int armed[SLOTS][INT_EPS];     /* is a read pending on it */
static int slot_speed[SLOTS];  /* what speed the device answered at */

/* Buffer for control transfer data: the controller reaches for it itself */
/* One buffer per device, not one for the whole controller.
 *
 * While a keyboard was the only thing on this bus, one was enough. Two
 * devices have two reads in flight at once, and one buffer means each
 * one's report lands where the other is about to read it - which shows
 * up as a keyboard that goes silent the moment a mouse is plugged in. */
/* A page each: a camera's configuration descriptor - every format,
   frame size and alternate setting it has - runs to a kilobyte or two,
   and has to be read whole. Aligned to its size, it never crosses the
   64 KiB line one TRB may not cross. */
static uint8_t buf[SLOTS][4096] __attribute__((aligned(4096)));
static uint8_t ibuf[SLOTS][INT_EPS][64] __attribute__((aligned(64)));

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
struct comp { int code, left, dci; };
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

/* 64-bit registers may be written in two halves, low half first
   (xHCI 5.1), and that is what we do: it needs nothing but the 32-bit
   access every other register gets */
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

static void ring_setup_n(struct ring *p, struct trb *trb, uint32_t n)
{
    memset(trb, 0, sizeof(struct trb) * n);
    p->trb = trb;
    p->index = 0;
    p->cycle = 1;
    p->size = n;

    /* The last TRB is a jump back to the start. The "toggle cycle" bit
       makes the meaning of the cycle bit flip on every turn - and that
       is why a ring never has to be cleared. */
    struct trb *jump = &trb[n - 1];
    jump->p0 = (uint32_t)fiz(trb);
    jump->p1 = (uint32_t)(fiz(trb) >> 32);
    jump->status = 0;
    jump->control = ((uint32_t)TRB_LINK << 10) | TRB_TC;
}

static void ring_setup(struct ring *p, struct trb *trb)
{
    ring_setup_n(p, trb, TRB_CHAIN);
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

/* Puts a TRB into a ring; returns where it went, which is how the
   controller's answer to a command names the command. */
static uint64_t insert(struct ring *p, uint64_t param, uint32_t status,
                       uint32_t control)
{
    irqflags_t flags = spin_lock_irq(&ring_lock);
    struct trb *t = &p->trb[p->index];
    uint64_t at = fiz(t);

    t->p0 = (uint32_t)param;
    t->p1 = (uint32_t)(param >> 32);
    t->status = status;
    /* The cycle bit goes last: only it hands the TRB over to the
       controller, so the rest of the fields must already be in it */
    t->control = control | p->cycle;

    p->index++;
    if (p->index == p->size - 1) {
        struct trb *jump = &p->trb[p->size - 1];
        jump->control = (jump->control & ~1u) | p->cycle;
        p->index = 0;
        p->cycle ^= 1;
    }
    spin_unlock_irq(&ring_lock, flags);
    return at;
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
    if (evt_index == EVT_TRBS) {
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
static int iso_event(int slot, int dci, const struct trb *t);

static void sort_events(void)
{
    struct trb t;
    irqflags_t flags = spin_lock_irq(&ring_lock);

    while (event(&t)) {
        int kind = (int)((t.control >> 10) & 0x3F);
        /* Only a command's answer is kept for later. Port changes are
           dropped here on purpose: the sockets are watched by reading
           their registers. Kept, they filled this queue - a laptop's
           controller sends one for every device that attaches and
           another for every port reset - until the answer to a command
           found no room and was lost, and from the seventh device or
           so every command "never finished". */
        if (kind == TRB_EV_COMMAND) {
            if (other_n < OTHERS)
                other_q[other_n++] = t;
            continue;
        }
        if (kind != TRB_EV_TRANSFER)
            continue;
        int s = (int)((t.control >> 24) & 0xFF);
        if (s < 1 || s > SLOTS)
            continue;
        /* A camera's TDs are not anybody's answer: filed with the rest,
           one would be taken for the reply to a control transfer on
           the same device */
        if (iso_event(s, (int)((t.control >> 16) & 0x1F), &t))
            continue;
        int next = (done[s - 1].head + 1) % COMPS;
        if (next == done[s - 1].tail)
            continue;                    /* nobody is collecting - drop it */
        done[s - 1].q[done[s - 1].head].code = (int)(t.status >> 24);
        done[s - 1].q[done[s - 1].head].left = (int)(t.status & 0xFFFFFF);
        done[s - 1].q[done[s - 1].head].dci = (int)((t.control >> 16) & 0x1F);
        done[s - 1].head = next;
    }
    spin_unlock_irq(&ring_lock, flags);
}

/* Is this endpoint one of the slot's interrupt endpoints? */
static int is_int_dci(int slot, int dci)
{
    for (int k = 0; k < INT_EPS; k++)
        if (int_dci[slot - 1][k] == dci)
            return 1;
    return 0;
}

/* Takes one finished transfer of this slot's on endpoint 'dci', or with
   dci 0 on any endpoint that is not an interrupt one - the control and
   bulk transfers, which wait for their answers one at a time. The
   others stay queued, in their order, for whoever they belong to: a
   report from the media keys must not be taken for the keyboard's. */
static int take_comp(int slot, int dci, struct comp *out)
{
    irqflags_t flags = spin_lock_irq(&ring_lock);
    int got = 0;
    int t = done[slot - 1].tail, h = done[slot - 1].head;

    for (int i = t; i != h; i = (i + 1) % COMPS) {
        struct comp c = done[slot - 1].q[i];
        if (dci ? c.dci != dci : is_int_dci(slot, c.dci))
            continue;
        *out = c;
        /* close the gap, keeping the rest in order */
        for (int j = i; (j + 1) % COMPS != h; j = (j + 1) % COMPS)
            done[slot - 1].q[j] = done[slot - 1].q[(j + 1) % COMPS];
        done[slot - 1].head = (h + COMPS - 1) % COMPS;
        got = 1;
        break;
    }
    spin_unlock_irq(&ring_lock, flags);
    return got;
}

/* How long a transfer is waited for with the core offered to others
   but not slept on: longer than a disk takes to answer, shorter than
   anybody would notice a core kept busy */
#define YIELD_TICKS 3

/* Waits for one of this slot's transfers, filing everybody else's on
   the way rather than dropping them. */
static int wait_slot(int slot, struct comp *out, uint32_t ms)
{
    uint32_t start = pit_ticks();
    uint32_t az = start + (ms + 9) / 10 + 1;
    int spins = 0;

    while (pit_ticks() < az) {
        sort_events();
        if (take_comp(slot, 0, out))
            return 1;
        /* The first few hundred turns are a busy wait, because most
           answers come back in microseconds and giving the core away
           would cost more than the wait itself. After that the core is
           offered to anybody who wants it - and taken straight back
           when nobody does, rather than slept on until the next tick.
           A disk answers in a millisecond or two, and sleeping until
           the next tick for every such answer is up to ten
           milliseconds a command: that was nearly all of the time it
           took to install the system onto a stick. Only an answer later than a few ticks - a
           network card with nothing to say for five seconds - is
           waited for asleep, so that it keeps no core busy. */
        if (++spins <= 200 || !task_scheduler_ready())
            io_wait();
        else if (pit_ticks() - start < YIELD_TICKS)
            task_sleep(0);
        else
            task_sleep(1);
    }
    return 0;
}

/* Waits for the answer to the command placed at 'trb'. An answer to
   any other command can only be a late one - commands take turns, so
   the one that timed out before this is the only other candidate -
   and taking it for this one's would hand a caller a slot number it
   never asked for. It is thrown away. */
static int wait_command(uint64_t trb, struct trb *result, uint32_t ms)
{
    uint32_t az = pit_ticks() + (ms + 9) / 10 + 1;
    while (pit_ticks() < az) {
        sort_events();
        irqflags_t flags = spin_lock_irq(&ring_lock);
        int found = 0;
        while (other_n > 0 && !found) {
            struct trb t = other_q[0];
            for (int j = 1; j < other_n; j++)
                other_q[j - 1] = other_q[j];
            other_n--;
            if ((t.p0 | ((uint64_t)t.p1 << 32)) == trb) {
                *result = t;
                found = 1;
            }
        }
        spin_unlock_irq(&ring_lock, flags);
        if (found)
            return 1;
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

    uint64_t at = insert(&cmd_pier, param, 0, control);
    doorbells[0] = 0;                      /* the command ring doorbell */

    if (!wait_command(at, &t, 1000)) {
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
 * whole diagnosis, so it is written down and printed by 'hardware'. */
static const char *gave_up;

/* The controller this driver claimed: a reason to give up after that
   is the Device Manager's to show as well */
static struct pci_dev claimed_dev;
static int claimed;

static int give_up(const char *why)
{
    gave_up = why;
    if (claimed)
        device_trouble(&claimed_dev, DEV_FAILED, why);
    return 0;
}

const char *xhci_why(void)
{
    return gave_up;
}

/* And why the last device could not be greeted - for the list of
   sockets in 'hardware', where "something is in socket 7 and did not
   answer" is only half of what a person needs to know. */
static const char *attach_failed;

const char *xhci_attach_why(void)
{
    return attach_failed;
}

static int attach_fail(const char *why)
{
    attach_failed = why;
    /* A controller that gave up says so in its status, and then that is
       the reason, whatever step noticed it first */
    if (base && (rd(op + OP_USBSTS) & (STS_HCE | STS_HSE)))
        attach_failed = "the controller stopped with an internal error";
    return -1;
}

/* --- taking the controller from the firmware --------------------------
 *
 * A laptop's firmware drives this controller itself, so that a USB
 * keyboard works in its menus, and through System Management Mode it
 * can go on doing so after the system has started: an SMI on every
 * event, handled behind our back. The way to ask for the controller is
 * the USB Legacy Support capability in the controller's own registers:
 * set "OS owned", wait for the firmware to drop "BIOS owned", and
 * switch its SMIs off. QEMU's controller has no such capability, so
 * none of this ever ran before a laptop. Linux waits a second and then
 * takes the controller anyway; so do we.
 */
static void take_from_firmware(void)
{
    uint32_t at = (rd(base + CAP_HCCPARAMS1) >> 16) << 2;

    for (int hops = 0; at && at < 0x10000 && hops < 32; hops++) {
        uintptr_t cap = base + at;
        uint32_t v = rd(cap);
        if ((v & 0xFF) == LEG_ID) {
            if (v & LEG_BIOS_OWNED) {
                wr(cap, v | LEG_OS_OWNED);
                for (int i = 0; i < 100 && (rd(cap) & LEG_BIOS_OWNED); i++)
                    wait_ms(10);
                if (rd(cap) & LEG_BIOS_OWNED) {
                    printk("[ !! ] xHCI: the firmware would not let go, "
                           "taking the controller anyway\n");
                    wr(cap, (rd(cap) & ~LEG_BIOS_OWNED) | LEG_OS_OWNED);
                }
            }
            /* Every SMI enable off, every pending SMI acknowledged */
            wr(cap + 4, (rd(cap + 4) & LEG_SMI_KEEP) | LEG_SMI_EVENTS);
            return;
        }
        uint32_t next = (v >> 8) & 0xFF;
        if (!next)
            return;
        at += next << 2;
    }
}

/* --- Intel's shared ports ---------------------------------------------
 *
 * On Intel chipsets from Panther Point (2012) to Wildcat Point (2015)
 * the USB 2.0 wires of every socket run to two controllers, EHCI and
 * xHCI, and registers in the xHCI's PCI configuration say which of the
 * two has them. Firmware may leave them with EHCI, for systems that
 * know nothing newer, and then this controller sees an empty socket
 * where the webcam, the Bluetooth adapter and every USB 2.0 stick are.
 * The mask registers next to them say which sockets may be moved;
 * Linux moves exactly those, and so do we. Only where an Intel EHCI is
 * there too: on chipsets without one these offsets are something else.
 */
static void route_intel_ports(const struct pci_dev *d)
{
    struct pci_dev ehci;

    if (d->vendor != 0x8086 || !pci_find_class(0x0C, 0x03, 0x20, &ehci) ||
        ehci.vendor != 0x8086)
        return;

    uint32_t ss = pci_config_read(d->bus, d->slot, d->func, INTEL_USB3PRM);
    pci_config_write(d->bus, d->slot, d->func, INTEL_USB3_PSSEN, ss);
    uint32_t hs = pci_config_read(d->bus, d->slot, d->func, INTEL_USB2PRM);
    pci_config_write(d->bus, d->slot, d->func, INTEL_XUSB2PR, hs);
    printk("[ OK ] xHCI: Intel's shared sockets taken over from EHCI "
           "(USB 2.0: %x, USB 3.0: %x)\n",
           pci_config_read(d->bus, d->slot, d->func, INTEL_XUSB2PR),
           pci_config_read(d->bus, d->slot, d->func, INTEL_USB3_PSSEN));
}

/* Gives the controller the scratchpad pages it asks for (see scratch[]
   above). 0 = it could not be done. */
static int give_scratchpad(void)
{
    uint32_t hcs2 = rd(base + CAP_HCSPARAMS2);
    /* Ten bits split in two: the high five at 25:21, the low five at
       31:27. A driver reading only the low half gives a controller that
       wants 34 pages two of them. */
    int want = (int)((((hcs2 >> 21) & 0x1F) << 5) | ((hcs2 >> 27) & 0x1F));

    if (!want)
        return 1;
    /* The pages are the controller's own size, which is 4 KiB in every
       one we know of - bit 0 of PAGESIZE says so */
    if (!(rd(op + OP_PAGESIZE) & 1))
        return give_up("the controller does not use 4 KiB pages");
    if (want > SCRATCH_MAX)
        return give_up("the controller wants more scratchpad memory "
                       "than this driver keeps");
    while (nscratch < want) {
        uint32_t page = pmm_alloc_frame();
        if (!page)
            return give_up("no memory left for the controller's "
                           "scratchpad");
        memset((void *)(uintptr_t)page, 0, 4096);
        scratch[nscratch++] = page;
    }
    dcbaa[0] = fiz(scratch);
    return 1;
}

int xhci_init(void)
{
    struct pci_dev dev;
    /* Class 0x0C (serial bus), subclass 0x03 (USB),
       programming interface 0x30 (xHCI) */
    if (!pci_find_class(0x0C, 0x03, 0x30, &dev))
        return give_up("there is no xHCI controller on the PCI bus");
    device_claim(&dev, "xhci");
    claimed_dev = dev;
    claimed = 1;

    /* xHCI sits in memory - BAR0, possibly 64-bit (bits 2:1 equal to 2
       mean the upper half of the address is in BAR1) */
    uint64_t addr = dev.bar[0] & ~0xFULL;
    if (dev.bar[0] & 0x01)
        return give_up("the controller's BAR0 is not a memory window");
    if (((dev.bar[0] >> 1) & 0x3) == 0x2)
        addr |= (uint64_t)dev.bar[1] << 32;
    if (!addr)
        return give_up("the firmware gave the controller no address");

    if (MAP_PHYS((uintptr_t)addr, 0x10000) < 0)
        return give_up("could not map the controller's window");
    pci_enable_bus_master(&dev);

    base = (uintptr_t)addr;
    op = base + (rd(base + CAP_CAPLENGTH) & 0xFF);
    rt = base + (rd(base + CAP_RTSOFF) & ~0x1Fu);
    doorbells = (volatile uint32_t *)(base + (rd(base + CAP_DBOFF) & ~0x3u));

    uint32_t hcs1 = rd(base + CAP_HCSPARAMS1);
    nports = (int)((hcs1 >> 24) & 0xFF);
    if (nports > 32)
        nports = 32;
    int max_slots = (int)(hcs1 & 0xFF);
    ctx_size = (rd(base + CAP_HCCPARAMS1) & 0x4) ? 64 : 32;

    /* The firmware lets go first, and the sockets come over to us,
       before anything here touches the controller */
    take_from_firmware();
    route_intel_ports(&dev);

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
        return give_up("the controller will not reset");

    memset(dcbaa, 0, sizeof(dcbaa));
    memset(dev_ctx, 0, sizeof(dev_ctx));
    memset(in_ctx, 0, sizeof(in_ctx));
    for (int s = 0; s < SLOTS; s++)
        for (int e = 0; e < RINGS; e++)
            ring_setup(&ep_pier[s][e], ep_trb[s][e]);

    if (!give_scratchpad())
        return 0;

    wr(op + OP_CONFIG, (uint32_t)(max_slots < SLOTS ? max_slots : SLOTS));
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
    erst[2] = EVT_TRBS;
    erst[3] = 0;
    wr(rt + RT_ERSTSZ, 1);
    wr64(rt + RT_ERDP, fiz(evt_trb) | 0x8);
    wr64(rt + RT_ERSTBA, fiz(erst));

    wr(op + OP_DNCTRL, 0);
    wr(op + OP_USBCMD, CMD_RS);          /* without CMD_INTE - we poll */
    for (int i = 0; i < 200 && (rd(op + OP_USBSTS) & STS_HCH); i++)
        wait_ms(1);
    if (rd(op + OP_USBSTS) & STS_HCH)
        return give_up("the controller will not start after the reset");
    if (rd(op + OP_USBSTS) & (STS_HCE | STS_HSE))
        return give_up("the controller stopped with an internal error "
                       "as soon as it started");

    /* Port power: without it nothing on a port will ever report in */
    for (int i = 0; i < nports; i++) {
        uint32_t v = rd(port_reg(i));
        if (!(v & PORT_PP))
            wr(port_reg(i), portsc_safe(v) | PORT_PP);
    }

    /* Then let the sockets say what is in them. A device that saw the
       reset, or that was just moved over from EHCI, takes a while to
       attach again: QEMU's do it at once, real ones within a hundred
       milliseconds or so. The walk across the sockets at start-up is
       made once, so a device that reports in a moment after it is a
       device nobody greets - wait until the sockets have not changed
       for 100 ms, a second at most. */
    uint32_t was = 0;
    int still = 0;
    for (int t = 0; t < 100 && still < 10; t++) {
        uint32_t now = 0;
        for (int i = 0; i < nports; i++)
            if (rd(port_reg(i)) & PORT_CCS)
                now |= 1u << i;
        still = (t > 0 && now == was) ? still + 1 : 0;
        was = now;
        wait_ms(10);
    }
    device_up(&dev);
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
    attach_failed = (const char *)0;
    if (!(rd(port_reg(port)) & PORT_CCS))
        return -1;                       /* nothing in it: not a failure */
    if (!port_reset(port))
        return attach_fail("the socket did not come out of its reset");

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
    attach_failed = (const char *)0;

    struct trb reply;
    int code = command(0, (uint32_t)TRB_ENABLE_SLOT << 10, &reply);
    if (code == CODE_NO_SLOTS)
        return attach_fail("the controller has no device slot left");
    if (code != CODE_OK)
        return attach_fail(code < 0 ? "the controller did not answer "
                                      "Enable Slot"
                                    : "the controller refused a slot");
    int slot = (int)(reply.control >> 24);
    if (slot < 1 || slot > SLOTS)
        return attach_fail("the controller gave a slot out of range");

    int maxpacket = packet_for_speed(speed);

    dcbaa[slot] = fiz(dev_ctx[slot - 1]);
    memset(dev_ctx[slot - 1], 0, DEV_CTX_BYTES);
    ring_setup(&ep_pier[slot - 1][0], ep_trb[slot - 1][0]);

    slot_speed[slot - 1] = speed;
    make_input(slot, port, speed, maxpacket, route, tt_slot, tt_port);
    if (command(fiz(in_ctx[slot - 1]),
                  ((uint32_t)TRB_ADDRESS_DEV << 10) | ((uint32_t)slot << 24),
                  (struct trb *)0) != CODE_OK) {
        xhci_release(slot);
        return attach_fail("the device would not take an address");
    }

    /* The first eight bytes of the device descriptor say how large the
       packets endpoint zero accepts are. At full speed that is
       sometimes more than eight - and then the controller has to be
       told, otherwise longer answers fall apart across packets. */
    uint8_t setup[8] = { 0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 8, 0x00 };
    uint8_t info8[8];
    if (xhci_control(slot, setup, info8, 8, 1) < 8) {
        xhci_release(slot);
        return attach_fail("the device did not say what it is");
    }
    /* A SuperSpeed device writes the size as a power of two: 9 means
       512. Taken as it stands, a USB 3.0 stick got endpoint zero
       packets of nine bytes. */
    int mps0 = info8[7];
    if (speed >= 4)
        mps0 = info8[7] < 16 ? 1 << info8[7] : 0;
    if (mps0 && mps0 != maxpacket) {
        uint8_t *we = in_ctx[slot - 1];
        uint32_t *control = info(we, 0);
        control[0] = 0;
        control[1] = 0x2;             /* we correct endpoint zero only */
        uint32_t *punkt0 = info(we, 2);
        punkt0[1] = (punkt0[1] & 0xFFFFu) | ((uint32_t)mps0 << 16);
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
    int k = 0;
    while (k < INT_EPS && int_dci[slot - 1][k] && int_dci[slot - 1][k] != number)
        k++;
    if (k == INT_EPS)
        return -1;                       /* this device has enough of them */
    int r = int_ring(k);

    ring_setup(&ep_pier[slot - 1][r], ep_trb[slot - 1][r]);
    armed[slot - 1][k] = 0;

    uint8_t *we = in_ctx[slot - 1];
    memset(we, 0, CTX_BYTES);
    uint32_t *control = info(we, 0);
    control[1] = 0x1u | (1u << number);   /* the device plus this endpoint */

    /* The device context has to be given afresh, because the number of
       endpoints changes - the controller will not work it out itself.
       Its last endpoint is the highest one in use: an endpoint added
       after one with a higher number must not cut that one off. */
    uint32_t *src = info(dev_ctx[slot - 1], 0);
    uint32_t *device = info(we, 1);
    uint32_t last = src[0] >> 27;
    if ((uint32_t)number > last)
        last = (uint32_t)number;
    device[0] = (src[0] & 0x07FFFFFFu) | (last << 27);
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
    uint64_t p = fiz(ep_trb[slot - 1][r]) | 1;
    punkt[2] = (uint32_t)p;
    punkt[3] = (uint32_t)(p >> 32);
    punkt[4] = (uint32_t)maxpacket | ((uint32_t)maxpacket << 16);

    if (command(fiz(we),
                  ((uint32_t)TRB_CONFIG_EP << 10) | ((uint32_t)slot << 24),
                  (struct trb *)0) != CODE_OK)
        return -1;
    int_dci[slot - 1][k] = number;
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
        len > (int)sizeof(ibuf[0][0]))
        return -1;
    int number = dci(ep), k = 0;
    while (k < INT_EPS && int_dci[slot - 1][k] != number)
        k++;
    if (k == INT_EPS)
        return -1;                       /* never set up */

    if (!armed[slot - 1][k]) {
        insert(&ep_pier[slot - 1][int_ring(k)], fiz(ibuf[slot - 1][k]),
               (uint32_t)len, ((uint32_t)TRB_NORMAL << 10) | TRB_IOC | TRB_ISP);
        doorbells[slot] = (uint32_t)number;
        armed[slot - 1][k] = 1;
        return 0;                        /* the answer will come in a moment */
    }

    sort_events();
    struct comp c;
    if (!take_comp(slot, number, &c))
        return 0;                        /* the device is silent - we keep waiting */
    armed[slot - 1][k] = 0;

    if (c.code != CODE_OK && c.code != CODE_SHORT)
        return -1;
    int passed = len - c.left;
    if (passed > 0)
        memcpy(data, ibuf[slot - 1][k], (uint32_t)passed);
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
    /* out_ep 0: an IN endpoint alone - a camera that streams over bulk
       has nothing to be told that way */
    if (!base || slot < 1 || slot > SLOTS || in_ep < 1 || out_ep < 0)
        return -1;

    int in_dci = dci_dir(in_ep, 1);
    int out_dci = out_ep ? dci_dir(out_ep, 0) : 0;
    if (in_dci > 31 || out_dci > 31)
        return -1;

    ring_setup(&ep_pier[slot - 1][2], ep_trb[slot - 1][2]);
    ring_setup(&ep_pier[slot - 1][3], ep_trb[slot - 1][3]);

    uint8_t *we = in_ctx[slot - 1];
    memset(we, 0, CTX_BYTES);
    uint32_t *control = info(we, 0);
    control[1] = 0x1u | (1u << in_dci) | (out_dci ? 1u << out_dci : 0u);

    /* The device context says how many endpoints the controller is to
       keep room for - and that is the HIGHEST number in use, not how
       many there are: an interrupt endpoint above these two, added
       earlier, must not be cut off */
    uint32_t *src = info(dev_ctx[slot - 1], 0);
    uint32_t top = src[0] >> 27;
    if ((uint32_t)in_dci > top)
        top = (uint32_t)in_dci;
    if ((uint32_t)out_dci > top)
        top = (uint32_t)out_dci;
    uint32_t *device = info(we, 1);
    device[0] = (src[0] & 0x07FFFFFFu) | (top << 27);
    device[1] = src[1];

    static const int kinds[2] = { 6, 2 };      /* bulk in, bulk out */
    int dcis[2] = { in_dci, out_dci };
    for (int i = 0; i < (out_dci ? 2 : 1); i++) {
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
 * counts. Each piece is a transfer of its own, so a piece that is not
 * a whole number of packets ends with a short one, and a device takes
 * that as the end of the data. Buffers that move a lot at once are
 * therefore aligned so that they never split (the FAT writer's run of
 * sectors, kernel/fat.c); the split stays, for the buffers that are not.
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

/* --- isochronous IN ------------------------------------------------------
 *
 * A camera sends a picture as a steady stream: a packet (or up to three
 * at high speed) every service interval, whether anybody is reading or
 * not. The endpoint is isochronous - no retries, no handshakes - and
 * the way to keep up is to keep the controller supplied with TDs, one
 * per interval, well ahead of time.
 *
 * So the ring is filled once with a TD for every interval of about the
 * next thirty milliseconds, each with its own buffer, and each TD that
 * comes back is handed out and put straight back where it was: TD i is
 * always ring entry i and buffer i, and since the ring is refilled in
 * the order it is used, the cycle bits alone say whose an entry is.
 * Every TD asks for an event, and the events of this endpoint are
 * sorted here instead of into the device's queue.
 *
 * One stream at a time: one camera. Starting as soon as possible
 * (SIA) rather than at a frame number keeps this away from the
 * controller's frame counter.
 */
#define ISO_TD_MAX 255          /* + the jump back: a page of TRBs */

static struct {
    int running;
    int slot, dci, esit, mps, tds;
    struct ring ring;
    struct trb *trbs;
    uint32_t pages[ISO_TD_MAX];  /* the buffers, a page holding one or more */
    uint32_t slot_bytes;         /* one TD's buffer: a power of two */
    uint32_t next;               /* the oldest TD not yet handed out */
    volatile uint8_t done[ISO_TD_MAX];
    uint8_t code[ISO_TD_MAX];
    uint32_t left[ISO_TD_MAX];
    volatile uint32_t overruns;
} iso;

static uint8_t *iso_buf(uint32_t i)
{
    uint32_t per = 4096 / iso.slot_bytes;
    return (uint8_t *)(uintptr_t)iso.pages[i / per] + (i % per) * iso.slot_bytes;
}

/* Called with the ring lock held, from sort_events: 1 = it was ours */
static int iso_event(int slot, int dci, const struct trb *t)
{
    if (!iso.running || slot != iso.slot || dci != iso.dci)
        return 0;
    uint64_t at = (uint64_t)t->p0 | ((uint64_t)t->p1 << 32);
    uint64_t first = fiz(iso.trbs);
    if (at >= first && at < first + (uint64_t)iso.tds * sizeof(struct trb)) {
        uint32_t i = (uint32_t)((at - first) / sizeof(struct trb));
        iso.code[i] = (uint8_t)(t->status >> 24);
        iso.left[i] = t->status & 0xFFFFFF;
        iso.done[i] = 1;
    } else {
        /* No TD: the ring ran dry (overrun, underrun) or a service
           interval was missed - the controller has stopped looking at
           this endpoint until it is rung again */
        iso.overruns++;
    }
    return 1;
}

static void iso_queue(uint32_t i)
{
    uint32_t packets = (uint32_t)((iso.esit + iso.mps - 1) / iso.mps);
    insert(&iso.ring, fiz(iso_buf(i)), (uint32_t)iso.esit,
           ((uint32_t)TRB_ISOCH << 10) | TRB_IOC | TRB_ISP |
           ((packets - 1) << 16) | TRB_SIA);
}

static void iso_free(void)
{
    uint32_t per = iso.slot_bytes ? 4096 / iso.slot_bytes : 1;
    for (int i = 0; i < iso.tds; i += (int)per)
        if (iso.pages[i / per])
            pmm_free_frame(iso.pages[i / per]);
    if (iso.trbs)
        pmm_free_frame((uint32_t)(uintptr_t)iso.trbs);
    memset(&iso, 0, sizeof(iso));
}

int xhci_iso_open(int slot, int ep, int raw_mps, int binterval)
{
    if (!base || slot < 1 || slot > SLOTS || ep < 1 || iso.running ||
        iso.trbs)
        return -1;
    int speed = slot_speed[slot - 1];
    int mps = raw_mps & 0x7FF;
    int mult = speed == 3 ? 1 + ((raw_mps >> 11) & 3) : 1;
    if (mps < 1 || mult > 3)
        return -1;
    int esit = mps * mult;
    int b = binterval < 1 ? 1 : binterval > 16 ? 16 : binterval;
    /* The interval field counts 2^n microframes. High speed already
       writes it that way (2^(bInterval-1) microframes); full speed
       writes 2^(bInterval-1) frames, three doublings more. */
    int period = b - 1 + (speed >= 3 ? 0 : 3);
    if (period > 15)
        period = 15;

    /* About thirty milliseconds of TDs: the driver is read every ten */
    int us = 125 << period;
    int tds = 30000 / us;
    if (tds < 16)
        tds = 16;
    if (tds > ISO_TD_MAX)
        tds = ISO_TD_MAX;

    iso.slot_bytes = 1024;
    while ((int)iso.slot_bytes < esit)
        iso.slot_bytes <<= 1;
    if (iso.slot_bytes > 4096)
        return -1;
    iso.slot = slot;
    iso.esit = esit;
    iso.mps = mps;
    iso.tds = tds;
    iso.dci = dci_dir(ep, 1);
    uint32_t per = 4096 / iso.slot_bytes;
    for (int i = 0; i < tds; i += (int)per) {
        uint32_t page = pmm_alloc_frame();
        if (!page) {
            iso_free();
            return -1;
        }
        iso.pages[i / per] = page;
    }
    uint32_t ring_page = pmm_alloc_frame();
    if (!ring_page) {
        iso_free();
        return -1;
    }
    iso.trbs = (struct trb *)(uintptr_t)ring_page;
    ring_setup_n(&iso.ring, iso.trbs, (uint32_t)tds + 1);

    uint8_t *we = in_ctx[slot - 1];
    memset(we, 0, CTX_BYTES);
    uint32_t *control = info(we, 0);
    control[1] = 0x1u | (1u << iso.dci);
    uint32_t *src = info(dev_ctx[slot - 1], 0);
    uint32_t last = src[0] >> 27;
    if ((uint32_t)iso.dci > last)
        last = (uint32_t)iso.dci;
    uint32_t *device = info(we, 1);
    device[0] = (src[0] & 0x07FFFFFFu) | (last << 27);
    device[1] = src[1];

    /* Isochronous IN (type 5): no retries (CErr 0); below SuperSpeed
       the extra transactions go in Max Burst and Mult stays 0; the
       payload of a service interval in Max ESIT Payload, and as the
       average TRB length */
    uint32_t *punkt = info(we, iso.dci + 1);
    punkt[0] = ((uint32_t)period << 16) | (((uint32_t)esit >> 16) << 24);
    punkt[1] = (5u << 3) | ((uint32_t)(mult - 1) << 8) |
               ((uint32_t)mps << 16);
    uint64_t p = fiz(iso.trbs) | 1;
    punkt[2] = (uint32_t)p;
    punkt[3] = (uint32_t)(p >> 32);
    punkt[4] = (uint32_t)esit | (((uint32_t)esit & 0xFFFF) << 16);

    int code = command(fiz(we), ((uint32_t)TRB_CONFIG_EP << 10) |
                                ((uint32_t)slot << 24), (struct trb *)0);
    if (code != CODE_OK) {
        iso_free();
        return code == CODE_BANDWIDTH ? -2 : -1;
    }
    return 0;
}

int xhci_iso_start(int slot)
{
    if (!iso.trbs || slot != iso.slot || iso.running)
        return -1;
    for (int i = 0; i < iso.tds; i++)
        iso.done[i] = 0;
    iso.next = 0;
    iso.overruns = 0;
    iso.running = 1;
    for (int i = 0; i < iso.tds; i++)
        iso_queue((uint32_t)i);
    doorbells[slot] = (uint32_t)iso.dci;
    return 0;
}

/* Hands out every TD that has come back, oldest first, and puts each
   back on the ring. A TD skipped by the controller (a missed service
   interval leaves no event of its own) is handed out as lost, once a
   later one is known to be done. The number handed out, or -1. */
int xhci_iso_poll(int slot, void (*fn)(void *arg, const uint8_t *data,
                                        int len, int lost), void *arg)
{
    if (!iso.running || slot != iso.slot)
        return -1;
    sort_events();
    int handed = 0;
    for (int n = 0; n < iso.tds; n++) {
        uint32_t i = iso.next;
        int skipped = 0;
        if (!iso.done[i]) {
            int later = 0;
            for (int k = 1; k < iso.tds && !later; k++)
                later = iso.done[(i + (uint32_t)k) % (uint32_t)iso.tds];
            if (!later)
                break;
            /* (another thread may have sorted its event in meanwhile:
               then it was late, not lost) */
            skipped = !iso.done[i];
        }
        if (skipped) {
            fn(arg, (const uint8_t *)0, 0, 1);
        } else if (iso.code[i] == CODE_OK || iso.code[i] == CODE_SHORT) {
            int len = iso.esit - (int)iso.left[i];
            fn(arg, iso_buf(i), len < 0 ? 0 : len, 0);
        } else {
            fn(arg, (const uint8_t *)0, 0, 1);
        }
        iso.done[i] = 0;
        iso_queue(i);
        iso.next = (i + 1) % (uint32_t)iso.tds;
        handed++;
    }
    /* Rung every time: harmless while it runs, and what starts it
       again after the ring ran dry */
    doorbells[slot] = (uint32_t)iso.dci;
    return handed;
}

int xhci_iso_lost(void)
{
    return (int)iso.overruns;
}

int xhci_iso_close(int slot)
{
    if (!iso.trbs || slot != iso.slot)
        return -1;
    if (iso.running) {
        command(0, ((uint32_t)TRB_STOP_EP << 10) | ((uint32_t)iso.dci << 16) |
                   ((uint32_t)slot << 24), (struct trb *)0);
        /* ... and dropped, so its share of the bus is given back */
        uint8_t *we = in_ctx[slot - 1];
        memset(we, 0, CTX_BYTES);
        uint32_t *control = info(we, 0);
        control[0] = 1u << iso.dci;
        control[1] = 0x1u;
        uint32_t *src = info(dev_ctx[slot - 1], 0);
        uint32_t *device = info(we, 1);
        device[0] = src[0];
        device[1] = src[1];
        command(fiz(we), ((uint32_t)TRB_CONFIG_EP << 10) |
                         ((uint32_t)slot << 24), (struct trb *)0);
    }
    irqflags_t flags = spin_lock_irq(&ring_lock);
    iso.running = 0;
    spin_unlock_irq(&ring_lock, flags);
    iso_free();
    return 0;
}

/* Gives a slot back to the controller. Without this a machine that
   somebody plugs a stick into four times has no slots left - and the
   fifth one is simply not seen, which looks like a broken socket. */
int xhci_release(int slot)
{
    if (!base || slot < 1 || slot > SLOTS)
        return -1;
    if (iso.trbs && iso.slot == slot) {
        /* the camera went: nothing to stop, its memory to give back */
        irqflags_t flags = spin_lock_irq(&ring_lock);
        iso.running = 0;
        spin_unlock_irq(&ring_lock, flags);
        iso_free();
    }

    command(0, ((uint32_t)TRB_DISABLE_SLOT << 10) | ((uint32_t)slot << 24),
            (struct trb *)0);
    dcbaa[slot] = 0;
    for (int k = 0; k < INT_EPS; k++)
        armed[slot - 1][k] = int_dci[slot - 1][k] = 0;
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
