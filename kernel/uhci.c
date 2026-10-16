/* USB host controller (UHCI)
 *
 * Control goes through I/O ports, while transfers are described by
 * structures in memory: a transfer descriptor (TD) says in which
 * direction and how many bytes, a queue head (QH) links them into a
 * chain, and the frame list - 1024 pointers read by the controller
 * every millisecond - points at that queue.
 *
 * We wait by polling, without interrupts. A thread polls the keyboard
 * every dozen or so milliseconds anyway, and having no interrupts
 * means the same code also works where interrupts are off.
 *
 * All the structures have to lie in physically contiguous memory and
 * at an address the controller understands directly - that is, in the
 * identity mapping, where the virtual address equals the physical one.
 */

#include <stdint.h>
#include "include/uhci.h"
#include "include/pci.h"
#include "include/pmm.h"
#include "include/io.h"
#include "include/printk.h"
#include "include/string.h"
#include "include/pit.h"

/* Registers (offsets from the I/O port base) */
#define REG_CMD      0x00
#define REG_STS      0x02
#define REG_INTR     0x04
#define REG_FRNUM    0x06
#define REG_FLBASE   0x08
#define REG_SOF      0x0C
#define REG_PORTSC1  0x10

#define CMD_RS       0x0001         /* the controller is running */
#define CMD_HCRESET  0x0002
#define CMD_GRESET   0x0004
#define CMD_MAXP     0x0080         /* packets up to 64 B */

#define STS_HALTED   0x0020         /* the controller has stopped */

#define PORT_CONNECT     0x0001
#define PORT_CONNECT_CHG 0x0002
#define PORT_ENABLE      0x0004
#define PORT_ENABLE_CHG  0x0008
#define PORT_LOWSPEED    0x0100
#define PORT_RESET       0x0200

/* A transfer descriptor - 32 B, aligned to 16 */
struct td {
    volatile uint32_t link;
    volatile uint32_t status;
    volatile uint32_t token;
    volatile uint32_t buffer;
    uint32_t pad[4];
} __attribute__((packed, aligned(16)));

struct qh {
    volatile uint32_t head;
    volatile uint32_t element;
    uint32_t pad[2];
} __attribute__((packed, aligned(16)));

#define LINK_TERMINATE 0x00000001u
#define LINK_QH        0x00000002u

#define TD_ACTIVE      (1u << 23)
#define TD_STALLED     (1u << 22)
#define TD_LOWSPEED    (1u << 26)
#define TD_ERRCNT      (3u << 27)
#define TD_ACTLEN_MASK 0x7FFu

#define PID_SETUP 0x2D
#define PID_IN    0x69
#define PID_OUT   0xE1

#define TD_MAX 16                   /* enough for a control transfer */

static uint16_t base;               /* the controller's I/O port base */
static int nports;
static uint32_t *frame_list;
static struct qh *queue;
static struct td *tds;
static int armed;          /* is a read descriptor waiting in the queue */

static uint16_t rd16(int off)        { return inw(base + off); }
static void     wr16(int off, uint16_t v) { outw(base + off, v); }
static void     wr32(int off, uint32_t v) { outl(base + off, v); }

/* The controller needs time to reset a port; the PIT ticks every 10 ms */
static void wait_ms(uint32_t ms)
{
    uint32_t until = pit_ticks() + (ms + 9) / 10 + 1;
    while (pit_ticks() < until)
        io_wait();
}

int uhci_ports(void)
{
    return nports;
}

/* Is anything in the port. The connect bit says so directly; it is
   the question the socket-watching thread asks twice a second, and on
   an EHCI machine this controller is where the keyboard ends up. */
int uhci_port_connected(int port)
{
    if (!base || port < 0 || port >= nports)
        return 0;
    return (rd16(REG_PORTSC1 + port * 2) & PORT_CONNECT) ? 1 : 0;
}

int uhci_port_lowspeed(int port)
{
    return (rd16(REG_PORTSC1 + port * 2) & PORT_LOWSPEED) ? 1 : 0;
}

int uhci_port_reset(int port)
{
    int off = REG_PORTSC1 + port * 2;
    if (!(rd16(off) & PORT_CONNECT))
        return 0;

    wr16(off, PORT_RESET);
    wait_ms(50);
    wr16(off, 0);
    wait_ms(10);

    /* Enabling a port may take several attempts - a device recovers
       from a reset at its own pace */
    for (int i = 0; i < 10; i++) {
        uint16_t st = rd16(off);
        if (!(st & PORT_CONNECT))
            return 0;
        if (st & (PORT_CONNECT_CHG | PORT_ENABLE_CHG)) {
            wr16(off, st & ~PORT_RESET);   /* clear the change notifications */
            continue;
        }
        if (st & PORT_ENABLE)
            return 1;
        wr16(off, st | PORT_ENABLE);
        wait_ms(10);
    }
    return 0;
}

/* Assembles a chain of descriptors and waits for the controller to
   work through it. Returns 0 or -1; 'actlen' receives the number of
   bytes transferred. */
static int run_chain(int count, uint32_t *actlen)
{
    for (int i = 0; i < count; i++)
        tds[i].link = (i + 1 < count) ? ((uint32_t)(uintptr_t)&tds[i + 1] | 0x4)
                                      : LINK_TERMINATE;

    queue->element = (uint32_t)(uintptr_t)&tds[0];

    uint32_t total = 0;
    for (uint32_t spin = 0; spin < 2000000u; spin++) {
        int done = 1;
        for (int i = 0; i < count; i++) {
            uint32_t st = tds[i].status;
            if (st & TD_ACTIVE) {
                done = 0;
                break;
            }
            if (st & TD_STALLED) {
                queue->element = LINK_TERMINATE;
                return -1;
            }
        }
        if (done)
            break;
    }

    queue->element = LINK_TERMINATE;
    for (int i = 0; i < count; i++) {
        if (tds[i].status & TD_ACTIVE)
            return -1;                    /* nothing arrived in time */
        if ((tds[i].token & 0xFF) != PID_SETUP) {
            uint32_t len = (tds[i].status + 1) & TD_ACTLEN_MASK;
            total += len;
        }
    }
    if (actlen)
        *actlen = total;
    return 0;
}

static void fill_td(int i, int pid, int addr, int endpoint, int toggle,
                    int len, void *buf, int lowspeed)
{
    /* By default the last in the chain. Without this the field stays
       zero, and zero is for the controller a VALID pointer to address
       0 - it would go there for the next descriptor, take rubbish for
       a command and stop. run_chain replaces it with a pointer to the
       next one. */
    tds[i].link = LINK_TERMINATE;
    tds[i].status = TD_ACTIVE | TD_ERRCNT | (lowspeed ? TD_LOWSPEED : 0);
    tds[i].token = (uint32_t)pid | ((uint32_t)addr << 8) |
                   ((uint32_t)endpoint << 15) | ((uint32_t)toggle << 19) |
                   ((uint32_t)((len ? len - 1 : 0x7FF) & 0x7FF) << 21);
    tds[i].buffer = (uint32_t)(uintptr_t)buf;
}

int uhci_control(int addr, int lowspeed, int maxpacket,
                 const void *setup, void *data, int len, int in)
{
    if (!base || maxpacket <= 0)
        return -1;

    /* The request goes from a buffer the controller has to be able to see */
    static uint8_t setup_buf[8] __attribute__((aligned(16)));
    memcpy(setup_buf, setup, 8);

    int n = 0;
    fill_td(n++, PID_SETUP, addr, 0, 0, 8, setup_buf, lowspeed);

    int toggle = 1;
    int left = len;
    uint8_t *p = data;
    while (left > 0 && n < TD_MAX - 1) {
        int chunk = left > maxpacket ? maxpacket : left;
        fill_td(n++, in ? PID_IN : PID_OUT, addr, 0, toggle, chunk, p,
                lowspeed);
        toggle ^= 1;
        p += chunk;
        left -= chunk;
    }

    /* The status stage goes the opposite way from the data */
    fill_td(n++, in ? PID_OUT : PID_IN, addr, 0, 1, 0, (void *)0, lowspeed);

    uint32_t got = 0;
    if (run_chain(n, &got) < 0)
        return -1;
    return (int)got;
}

/* A read from an interrupt endpoint.
 *
 * We arm the descriptor once and NEVER abandon it. That is the crux:
 * if we took it off the queue when there was no data, the controller
 * could finish the transfer just after our decision. The report would
 * then land in a buffer nobody reads, and the device would flip its
 * data toggle - and from then on it would disagree with ours forever.
 * It showed itself the way such bugs usually do: it worked, it worked,
 * and then the keyboard went silent for good.
 *
 * An armed descriptor is retried by the controller itself every frame
 * while the device has nothing to say. We only look in to see whether
 * it has finished - and do not wait a moment. So we collect one report
 * per round, but none is lost.
 */
int uhci_interrupt_in(int addr, int lowspeed, int endpoint, int maxpacket,
                      void *data, int len, int *toggle)
{
    if (!base || len <= 0 || len > maxpacket)
        return -1;

    /* Should the controller have stopped (a list processing error),
       merely printing about it helps nobody - we bring it back up */
    if (rd16(REG_STS) & STS_HALTED) {
        wr16(REG_STS, 0x3F);
        wr16(REG_CMD, CMD_RS | CMD_MAXP);
        armed = 0;
    }

    if (!armed) {
        fill_td(0, PID_IN, addr, endpoint, toggle ? *toggle : 0, len, data,
                lowspeed);
        queue->element = (uint32_t)(uintptr_t)&tds[0];
        armed = 1;
        return 0;                        /* the answer will come in a moment */
    }

    uint32_t st = tds[0].status;
    if (st & TD_ACTIVE)
        return 0;                        /* the device is silent - we keep waiting */

    /* The descriptor is spent: the controller has already moved the
       queue pointer past it, that is to the end of the list */
    armed = 0;
    if (st & TD_STALLED)
        return -1;
    if (toggle)
        *toggle ^= 1;
    return (int)((st + 1) & TD_ACTLEN_MASK);
}

int uhci_init(void)
{
    struct pci_dev dev;
    /* Class 0x0C (serial bus), subclass 0x03 (USB),
       programming interface 0x00 (UHCI) */
    if (!pci_find_class(0x0C, 0x03, 0x00, &dev))
        return 0;

    /* UHCI sits in I/O port space - for it that is BAR4 */
    if (!(dev.bar[4] & 0x01))
        return 0;
    base = (uint16_t)(dev.bar[4] & 0xFFFC);
    if (!base)
        return 0;

    pci_enable_bus_master(&dev);
    /* Turn off legacy emulation (the BIOS pretending a USB device is a
       PS/2 keyboard) - otherwise the controller is not ours */
    pci_config_write(dev.bus, dev.slot, dev.func, 0xC0, 0x8F00);

    wr16(REG_CMD, CMD_HCRESET);
    for (int i = 0; i < 100 && (rd16(REG_CMD) & CMD_HCRESET); i++)
        io_wait();
    wr16(REG_INTR, 0);                   /* no interrupts - we poll */

    uint32_t mem = pmm_alloc_frame();
    uint32_t work = pmm_alloc_frame();
    if (!mem || !work) {
        base = 0;
        return 0;
    }
    frame_list = (uint32_t *)(uintptr_t)mem;
    queue = (struct qh *)(uintptr_t)work;
    tds = (struct td *)(uintptr_t)(work + 64);
    memset((void *)(uintptr_t)work, 0, PAGE_SIZE);

    queue->head = LINK_TERMINATE;
    queue->element = LINK_TERMINATE;

    /* The same queue in every frame: the controller looks into it every
       millisecond, and we put a transfer there when there is one to
       put */
    for (int i = 0; i < 1024; i++)
        frame_list[i] = (uint32_t)(uintptr_t)queue | LINK_QH;

    wr32(REG_FLBASE, mem);
    wr16(REG_FRNUM, 0);
    outb(base + REG_SOF, 0x40);
    wr16(REG_STS, 0x3F);                 /* clear the old notifications */
    wr16(REG_CMD, CMD_RS | CMD_MAXP);

    nports = 2;                          /* UHCI always has two ports */
    return 1;
}
