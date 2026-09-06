/* Choosing a USB host controller
 *
 * Just as blk.c chooses between ATA, AHCI and NVMe: we try xHCI first,
 * because newer boards have nothing else, and then UHCI. Nobody above
 * cares which of them answered.
 *
 * This file is also where the GREETING conversation with a device
 * lives - that is, the one part of device recognition that looks
 * completely different in the two controllers. In UHCI the driver
 * assigns the address with an ordinary request and has to invent it
 * itself; in xHCI the controller does it while allocating a slot and
 * the driver only gets a number. Above that border there is only the
 * conversation with the device, the same regardless of the controller -
 * and that is why usbkbd.c does not know what it is talking through.
 */

#include <stdint.h>
#include "include/usbhc.h"
#include "include/uhci.h"
#include "include/xhci.h"
#include "include/task.h"
#include "include/printk.h"

enum { NONE, XHCI, UHCI };

static int controller;

/* Devices on the UHCI side have to be described by hand: the
   controller remembers nothing about them. With xHCI all that
   knowledge sits inside the controller and a slot number is enough for
   us. */
#define DEVICES 4

struct device {
    int used;
    int addr;
    int lowspeed;
    int maxpacket;
    int ep;
    int ep_maxpacket;
    int toggle;
};

static struct device list[DEVICES];

#define PORTS_MAX 32
static int port_dev[PORTS_MAX];
static int port_known[PORTS_MAX];
static int port_taken[PORTS_MAX];

/* A socket is not the same thing as a port on the controller.
 *
 * The machine has as many sockets as the controller has ports - until
 * somebody plugs a hub into one of them, and then it has more. So the
 * list of sockets grows: the first entries are the controller's own
 * ports, and every hub found adds its ports to the end. A driver
 * walking "the ports" therefore reaches devices behind hubs without
 * knowing that hubs exist, which is the whole point.
 *
 * The route string is how the controller is told the way to a device:
 * four bits per tier of hubs. It is built here, one nibble per tier,
 * because here is where the tree is known. */
struct socket {
    int root;                    /* the controller's port it hangs off */
    uint32_t route;              /* 0 = the root port itself */
    int tier;                    /* 0 = root, 1 = behind one hub, ... */
    int hub;                     /* handle of the hub above, 0 = none */
    int hub_port;                /* which of its ports, counted from 1 */
    int hub_speed;               /* the hub's own speed (xHCI numbering) */
};

static struct socket sock[PORTS_MAX];
static int nsock;

/* The hubs themselves. A hub has an endpoint whose whole purpose is to
   say "something changed on port N" - it is how a hub is meant to be
   watched, instead of asking it about every port twice a second. */
#define HUBS 4
struct hubinfo {
    int dev;                     /* the hub's handle */
    int ep, maxpacket;           /* its status-change endpoint */
    int first;                   /* its ports, as sockets, start here */
    int ports;
};
static struct hubinfo hubs[HUBS];
static int nhubs;
static int dev_speed[PORTS_MAX];         /* per socket, filled on greeting */

static int usbhc_greet(int port);

/* Why there is no controller. The question is asked by 'sprzet', and
   the answer matters most on a real machine: "no xHCI on the PCI bus"
   and "the controller is there but did not start" call for different
   things to be done. */
const char *usbhc_why(void)
{
    if (controller != NONE)
        return (const char *)0;
    return xhci_why();
}

const char *usbhc_name(void)
{
    if (controller == XHCI)
        return "xHCI";
    if (controller == UHCI)
        return "UHCI";
    return "none";
}

int usbhc_init(void)
{
    /* Once is enough, and twice is worse than useless: starting a
       controller RESETS it, and the second driver to ask would wipe
       out the slot and the endpoint the first one is already using -
       which shows up as a keyboard that is found, announced, and then
       silent. */
    if (controller != NONE)
        return 1;

    if (xhci_init()) {
        controller = XHCI;
        /* One socket per port of the controller, to begin with. Hubs
           add theirs as they are found. */
        nsock = xhci_ports();
        if (nsock > PORTS_MAX)
            nsock = PORTS_MAX;
        for (int i = 0; i < nsock; i++) {
            sock[i].root = i;
            sock[i].route = 0;
            sock[i].tier = 0;
            sock[i].hub = 0;
            sock[i].hub_port = 0;
            sock[i].hub_speed = 0;
            port_dev[i] = -1;
        }
        return 1;
    }
    if (uhci_init()) {
        controller = UHCI;
        return 1;
    }
    controller = NONE;
    return 0;
}

/* How many sockets there are to look into: the controller's ports, and
   whatever hubs have added since. The number grows while enumeration
   runs, which is exactly what a loop over ports wants. */
int usbhc_ports(void)
{
    if (controller == XHCI)
        return nsock;
    if (controller == UHCI)
        return uhci_ports();
    return 0;
}

static void query(uint8_t *p, uint8_t typ, uint8_t req,
                      uint16_t value, uint16_t index, uint16_t len)
{
    p[0] = typ;
    p[1] = req;
    p[2] = (uint8_t)value;
    p[3] = (uint8_t)(value >> 8);
    p[4] = (uint8_t)index;
    p[5] = (uint8_t)(index >> 8);
    p[6] = (uint8_t)len;
    p[7] = (uint8_t)(len >> 8);
}

/* Greeting a device on UHCI: the first eight bytes of the descriptor
   say how large the packets endpoint zero accepts are, and then we
   assign an address. Every port gets a different number - if the first
   one had something that is not a keyboard hanging off it, the second
   must not inherit its address. */
static int uhci_greet(int port, struct device *u)
{
    if (!uhci_port_reset(port))
        return -1;

    u->lowspeed = uhci_port_lowspeed(port);
    u->maxpacket = 8;

    uint8_t setup[8];
    uint8_t info[8];
    query(setup, 0x80, 0x06, 0x0100, 0, 8);
    if (uhci_control(0, u->lowspeed, u->maxpacket, setup, info, 8, 1) < 8)
        return -1;
    if (info[7])
        u->maxpacket = info[7];

    int addr = port + 1;
    query(setup, 0x00, 0x05, (uint16_t)addr, 0, 0);
    if (uhci_control(0, u->lowspeed, u->maxpacket, setup, (void *)0, 0, 0) < 0)
        return -1;
    u->addr = addr;
    task_sleep(20);
    return 0;
}

/* Who has already got hold of which port.
 *
 * Two drivers walk these ports - the keyboard's and the mouse's - and
 * each stops at the first device it recognises, having looked at every
 * one before it. That raises two different problems and they need two
 * different answers.
 *
 * Greeting a port RESETS it, so the second walk must not greet a port
 * the first one already greeted: hence the handle, kept and handed
 * back. And a device another driver is already TALKING to must not be
 * looked at either - reading its descriptors while its own poll thread
 * is reading its reports is two conversations down one wire, and what
 * it costs is the keyboard quietly ceasing to deliver keys. So a
 * driver that takes a device says so, and from then on the port
 * answers "nothing here" to everybody else. */


void usbhc_claim(int port)
{
    if (port >= 0 && port < PORTS_MAX)
        port_taken[port] = 1;
}

int usbhc_attach(int port)
{
    if (port < 0 || port >= PORTS_MAX)
        return usbhc_greet(port);
    if (port_taken[port])
        return -1;                   /* somebody else's device */
    if (port_known[port])
        return port_dev[port];

    port_known[port] = 1;
    port_dev[port] = usbhc_greet(port);
    return port_dev[port];
}

/* --- hubs -----------------------------------------------------------
 *
 * A hub is a device like any other until it has been addressed, and
 * something else afterwards: it has ports of its own, and each of them
 * has to be given power and then reset by ASKING THE HUB, with
 * ordinary control transfers. The controller knows nothing about any
 * of it - it only has to be told, at the end, how to reach what was
 * found.
 */
#define HUB_CLASS        0x09
#define REQ_GET_STATUS   0x00
#define REQ_CLEAR_FEATURE 0x01
#define REQ_SET_FEATURE  0x03
#define FEAT_PORT_RESET  4
#define FEAT_PORT_POWER  8
#define FEAT_C_RESET     20              /* clear the "reset finished" flag */

static int hub_status(int dev, int port, uint32_t *status)
{
    uint8_t setup[8];
    uint8_t answer[4];

    query(setup, 0xA3, REQ_GET_STATUS, 0, (uint16_t)port, 4);
    if (usbhc_control(dev, setup, answer, 4, 1) < 4)
        return -1;
    *status = (uint32_t)answer[0] | ((uint32_t)answer[1] << 8) |
              ((uint32_t)answer[2] << 16) | ((uint32_t)answer[3] << 24);
    return 0;
}

/* A request to the hub about one of its ports. Setting a feature turns
   something on (power, reset); clearing one acknowledges something
   that has happened (the reset is over). */
static int hub_feature(int dev, int port, int feature, int set)
{
    uint8_t setup[8];

    query(setup, 0x23, set ? REQ_SET_FEATURE : REQ_CLEAR_FEATURE,
          (uint16_t)feature, (uint16_t)port, 0);
    return usbhc_control(dev, setup, (void *)0, 0, 0);
}

/* Wakes one port of a hub and says what came up on it: 0 = nothing
   there, otherwise the speed in the controller's numbering. */
static int hub_wake_port(int dev, int port)
{
    uint32_t status = 0;

    if (hub_status(dev, port, &status) < 0)
        return 0;
    if (!(status & 0x0001))              /* nothing connected */
        return 0;

    hub_feature(dev, port, FEAT_PORT_RESET, 1);
    for (int i = 0; i < 50; i++) {
        task_sleep(20);
        if (hub_status(dev, port, &status) < 0)
            return 0;
        if (status & 0x0002)             /* enabled = the reset is over */
            break;
    }
    if (!(status & 0x0002))
        return 0;
    hub_feature(dev, port, FEAT_C_RESET, 0);   /* acknowledge it */

    /* The two speed bits of the hub's port status. Low speed is bit 9,
       high speed bit 10, and neither set means full speed - which is
       the one everything used to be. */
    if (status & 0x0200)
        return 2;                        /* low */
    if (status & 0x0400)
        return 3;                        /* high */
    return 1;                            /* full */
}

/* A device that turned out to be a hub: its ports are added to the
   list of sockets, so that whoever walks the list finds what is on
   them. */
static void hub_expand(int socket_index, int dev)
{
    uint8_t setup[8];
    uint8_t desc[16];
    uint8_t cfg[9];

    /* The configuration has to be chosen before the hub will answer
       anything about its ports. */
    if (usbhc_descriptor(dev, 0x02, 0, cfg, 9) < 9)
        return;
    if (usbhc_request(dev, 0x00, 0x09, cfg[5], 0) < 0)
        return;

    /* The hub descriptor is class-specific: type 0x29, and it is the
       only place the number of ports is written down. */
    query(setup, 0xA0, 0x06, 0x2900, 0, 8);
    if (usbhc_control(dev, setup, desc, 8, 1) < 8)
        return;

    int ports = desc[2];
    int characteristics = desc[3] | (desc[4] << 8);
    int power_ms = desc[5] * 2;
    if (ports < 1 || ports > 15)
        return;

    /* The endpoint the hub reports changes on. It is the only
       interrupt endpoint a hub has, and every hub has one. */
    uint8_t full[64];
    int ep = 0, ep_packet = 1, ep_interval = 12;
    int total = cfg[2] | (cfg[3] << 8);
    if (total > (int)sizeof(full))
        total = (int)sizeof(full);
    if (usbhc_descriptor(dev, 0x02, 0, full, total) >= total) {
        for (int i = 0; i + 1 < total;) {
            int dlen = full[i];
            if (dlen < 2)
                break;
            if (full[i + 1] == 0x05 && i + 6 < total &&
                (full[i + 2] & 0x80) && (full[i + 3] & 0x03) == 0x03) {
                ep = full[i + 2] & 0x0F;
                ep_packet = full[i + 4] | (full[i + 5] << 8);
                ep_interval = full[i + 6] ? full[i + 6] : 12;
                break;
            }
            i += dlen;
        }
    }

    int speed = dev_speed[socket_index];
    int multi_tt = 0;                    /* one translator for the lot */
    int think = (characteristics >> 5) & 3;

    if (controller == XHCI)
        xhci_mark_hub(dev, ports, multi_tt, think);

    /* Power first, and then wait as long as the hub asked to be waited
       for - a port that has not finished powering up answers "nothing
       here" and the device behind it is simply never found. */
    for (int p = 1; p <= ports; p++)
        hub_feature(dev, p, FEAT_PORT_POWER, 1);
    task_sleep(power_ms > 20 ? (uint32_t)power_ms : 20);

    for (int p = 1; p <= ports && nsock < PORTS_MAX; p++) {
        struct socket *n = &sock[nsock];
        n->root = sock[socket_index].root;
        n->tier = sock[socket_index].tier + 1;
        /* Four bits per tier, the first tier lowest. Beyond five tiers
           the string runs out - and so does the specification, which
           allows exactly five. */
        n->route = sock[socket_index].route |
                   ((uint32_t)p << (4 * (n->tier - 1)));
        n->hub = dev;
        n->hub_port = p;
        n->hub_speed = speed;
        port_dev[nsock] = -1;
        port_known[nsock] = 0;
        nsock++;
    }

    if (ep && nhubs < HUBS) {
        if (ep_packet > 4)
            ep_packet = 4;               /* a bitmap of up to 15 ports */
        if (usbhc_int_endpoint(dev, ep, ep_packet, ep_interval) == 0) {
            hubs[nhubs].dev = dev;
            hubs[nhubs].ep = ep;
            hubs[nhubs].maxpacket = ep_packet;
            hubs[nhubs].first = nsock - ports;
            hubs[nhubs].ports = ports;
            nhubs++;
        }
    }

    printk("[ OK ] A USB hub in socket %d: %d sockets more\n",
           socket_index + 1, ports);
}

static int usbhc_greet(int port)
{
    if (controller == XHCI) {
        struct socket *sk = &sock[port];
        int slot;

        if (!sk->hub) {
            slot = xhci_attach(sk->root);
            if (slot < 0)
                return -1;
            dev_speed[port] = xhci_speed(sk->root);
        } else {
            int speed = hub_wake_port(sk->hub, sk->hub_port);
            if (!speed)
                return -1;               /* nothing in that socket */
            /* A device slower than the hub above it is reached through
               that hub's translator - but only when the hub is a
               high-speed one. Below that there is nothing to
               translate. */
            int tt_slot = (sk->hub_speed == 3 && speed != 3) ? sk->hub : 0;
            slot = xhci_address(sk->root, sk->route, speed, tt_slot,
                                sk->hub_port);
            if (slot < 0)
                return -1;
            dev_speed[port] = speed;
        }

        /* Is it a hub? Then the machine has more sockets than it had a
           moment ago. */
        uint8_t desc[18];
        if (usbhc_descriptor(slot, 0x01, 0, desc, 18) >= 18 &&
            desc[4] == HUB_CLASS)
            hub_expand(port, slot);

        /* The handle is the slot number; so that it does not get
           confused with UHCI handles, we store it the same way - each
           backend reads its own */
        return slot;
    }
    if (controller != UHCI)
        return -1;

    for (int i = 0; i < DEVICES; i++) {
        if (list[i].used)
            continue;
        struct device u = { 0, 0, 0, 8, 0, 8, 0 };
        if (uhci_greet(port, &u) < 0)
            return -1;
        u.used = 1;
        list[i] = u;
        return i + 1;
    }
    return -1;
}

/* --- what is in the socket, and who wants to know --------------------
 *
 * Enumeration at start-up is one walk across the ports. A machine
 * somebody uses needs the other half of it: a stick pushed in three
 * hours later has to be seen too, and one pulled out has to stop being
 * listed as though it were still there.
 *
 * The shape of it: drivers say what they can recognise, and one thread
 * watches the ports. That is the other way round from the walk at
 * start-up, where each driver looked at every port itself - and it has
 * to be, because now it is the PORT that changes, not the driver that
 * asks.
 */
#define GREETERS 4

static int (*plugged[GREETERS])(int port);
static void (*unplugged[GREETERS])(int port);
static int nplug, nunplug;
static int watching;

void usbhc_on_plug(int (*fn)(int port))
{
    if (nplug < GREETERS)
        plugged[nplug++] = fn;
}

void usbhc_on_unplug(void (*fn)(int port))
{
    if (nunplug < GREETERS)
        unplugged[nunplug++] = fn;
}

int usbhc_connected(int port)
{
    if (controller == XHCI) {
        if (port < 0 || port >= nsock)
            return 0;
        /* A hub's port is not asked about here: the hub says by itself
           when something changes on it, through the endpoint it has
           for exactly that, and the watching thread listens to it.
           Asking each port separately would be a control transfer per
           port twice a second, for an answer that is almost always
           "nothing". */
        if (sock[port].hub)
            return (port_known[port] && port_dev[port] >= 1) ? 1 : 0;
        return xhci_port_connected(sock[port].root);
    }
    /* UHCI keeps the connection bit in its own port register; nobody
       has asked this of it yet, and pretending otherwise would be
       worse than saying so. */
    return -1;
}

/* Forgets everything about a port, so that whatever is plugged in next
   is greeted from the beginning. The slot goes back to the controller
   with it - four sticks in and out would otherwise leave a machine
   with no slots and a socket that looks broken. */
void usbhc_forget(int port)
{
    if (port < 0 || port >= PORTS_MAX)
        return;
    if (controller == XHCI && port_known[port] && port_dev[port] > 0)
        xhci_release(port_dev[port]);
    port_known[port] = 0;
    port_taken[port] = 0;
    port_dev[port] = -1;
}

/* One look at what a hub has to say. Its endpoint answers with a
   bitmap: bit one for port one, and so on. Nothing to say is the
   normal answer and costs one transfer. */
static void watch_hub(struct hubinfo *h, int *was)
{
    uint8_t bits[4] = { 0, 0, 0, 0 };
    int n = usbhc_int_in(h->dev, h->ep, bits, h->maxpacket);

    if (n <= 0)
        return;

    for (int p = 1; p <= h->ports; p++) {
        if (!(bits[p / 8] & (1u << (p % 8))))
            continue;

        int index = h->first + p - 1;
        uint32_t status = 0;

        if (hub_status(h->dev, p, &status) < 0 || index >= PORTS_MAX)
            continue;

        /* Every change the hub is holding has to be acknowledged, not
           just the one we came for: an unacknowledged flag is
           reported again and again, and the endpoint then says
           "something changed" for ever, twice a second, about
           something that changed once. */
        for (int bit = 16; bit <= 20; bit++)
            if (status & (1u << bit))
                hub_feature(h->dev, p, bit, 0);

        int now = (status & 0x0001) ? 1 : 0;
        if (now == was[index])
            continue;
        was[index] = now;

        if (!now) {
            for (int i = 0; i < nunplug; i++)
                unplugged[i](index);
            usbhc_forget(index);
            continue;
        }
        task_sleep(300);                 /* let it settle */
        usbhc_forget(index);
        for (int i = 0; i < nplug; i++)
            if (plugged[i](index))
                break;
    }
}

static void watch_thread(void *arg)
{
    static int was[PORTS_MAX];
    (void)arg;

    for (int i = 0; i < usbhc_ports() && i < PORTS_MAX; i++)
        was[i] = usbhc_connected(i) > 0;

    for (;;) {
        task_sleep(500);

        /* What the hubs have to say - one transfer each, and silence
           is the usual answer. */
        for (int h = 0; h < nhubs; h++)
            watch_hub(&hubs[h], was);

        for (int port = 0; port < usbhc_ports() && port < PORTS_MAX;
             port++) {
            if (sock[port].hub)
                continue;                /* a hub's port - watched above */
            int now = usbhc_connected(port);
            if (now < 0)
                return;                  /* this controller cannot say */
            if (now == was[port])
                continue;
            was[port] = now;

            if (!now) {
                for (int i = 0; i < nunplug; i++)
                    unplugged[i](port);
                usbhc_forget(port);
                continue;
            }

            /* Something new. A device needs a moment after being
               pushed in before it answers anything - the specification
               allows a hundred milliseconds, and a hub gives it
               power in the meantime. */
            task_sleep(300);
            usbhc_forget(port);          /* whatever was here is gone */
            for (int i = 0; i < nplug; i++)
                if (plugged[i](port))
                    break;
        }
    }
}

/* Starts watching. Called by the drivers that can take something
   plugged in later; the first call starts the thread and the rest do
   nothing. */
void usbhc_watch(void)
{
    if (watching || controller == NONE)
        return;
    if (usbhc_connected(0) < 0)
        return;                          /* nothing to watch it with */
    watching = 1;
    task_create("usbhc", watch_thread, 0);
}

int usbhc_control(int dev, const void *setup, void *data, int len, int in)
{
    if (dev < 1)
        return -1;
    if (controller == XHCI)
        return xhci_control(dev, setup, data, len, in);
    if (controller != UHCI || dev > DEVICES)
        return -1;
    struct device *u = &list[dev - 1];
    return uhci_control(u->addr, u->lowspeed, u->maxpacket, setup, data,
                        len, in);
}

int usbhc_int_endpoint(int dev, int ep, int maxpacket, int interval)
{
    if (dev < 1)
        return -1;
    if (controller == XHCI)
        return xhci_int_endpoint(dev, ep, maxpacket, interval);
    if (controller != UHCI || dev > DEVICES)
        return -1;
    /* UHCI has nothing to set up: an endpoint is described to it with
       every transfer separately */
    struct device *u = &list[dev - 1];
    u->ep = ep;
    u->ep_maxpacket = maxpacket;
    u->toggle = 0;
    return 0;
}

int usbhc_int_in(int dev, int ep, void *data, int len)
{
    if (dev < 1)
        return -1;
    if (controller == XHCI)
        return xhci_int_in(dev, ep, data, len);
    if (controller != UHCI || dev > DEVICES)
        return -1;
    struct device *u = &list[dev - 1];
    return uhci_interrupt_in(u->addr, u->lowspeed, ep, u->ep_maxpacket,
                             data, len, &u->toggle);
}

/* Bulk endpoints - a disk, not a keyboard.
 *
 * Only xHCI for now, and that is said out loud rather than hidden:
 * UHCI would need its own queue for bulk and a data toggle kept per
 * endpoint, and the machines that still have UHCI alone are the ones
 * that boot from something else anyway. A driver above gets -1 and
 * knows there is no disk to be had here.
 */
int usbhc_bulk_endpoint(int dev, int in_ep, int out_ep, int maxpacket)
{
    if (dev < 1 || controller != XHCI)
        return -1;
    return xhci_bulk_endpoint(dev, in_ep, out_ep, maxpacket);
}

int usbhc_bulk_out(int dev, int ep, const void *data, int len)
{
    if (dev < 1 || controller != XHCI)
        return -1;
    return xhci_bulk_out(dev, ep, data, len);
}

int usbhc_bulk_in(int dev, int ep, void *data, int len)
{
    if (dev < 1 || controller != XHCI)
        return -1;
    return xhci_bulk_in(dev, ep, data, len);
}

/* --- The requests a device driver makes -----------------------------
 *
 * Eight bytes of setup, laid out as the specification says, and then
 * the transfer. Both of these were in the keyboard driver while the
 * keyboard was the only device on the bus. */
static void setup_packet(uint8_t *p, uint8_t type, uint8_t req,
                         uint16_t value, uint16_t index, uint16_t len)
{
    p[0] = type;
    p[1] = req;
    p[2] = (uint8_t)value;
    p[3] = (uint8_t)(value >> 8);
    p[4] = (uint8_t)index;
    p[5] = (uint8_t)(index >> 8);
    p[6] = (uint8_t)len;
    p[7] = (uint8_t)(len >> 8);
}

int usbhc_descriptor(int dev, uint8_t type, uint8_t index,
                     void *buf, int len)
{
    uint8_t setup[8];
    setup_packet(setup, 0x80, 0x06, (uint16_t)(type << 8 | index), 0,
                 (uint16_t)len);
    return usbhc_control(dev, setup, buf, len, 1);
}

int usbhc_request(int dev, uint8_t type, uint8_t req,
                  uint16_t value, uint16_t index)
{
    uint8_t setup[8];
    setup_packet(setup, type, req, value, index, 0);
    return usbhc_control(dev, setup, (void *)0, 0, 0);
}
