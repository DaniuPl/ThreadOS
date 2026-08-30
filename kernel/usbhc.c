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

enum { NONE, XHCI, UHCI };

static int controller;

/* Devices on the UHCI side have to be described by hand: the
   controller remembers nothing about them. With xHCI all that
   knowledge sits inside the controller and a slot number is enough for
   us. */
#define DEVICES 4

struct device {
    int uzyte;
    int addr;
    int lowspeed;
    int maxpacket;
    int ep;
    int ep_maxpacket;
    int toggle;
};

static struct device list[DEVICES];

const char *usbhc_name(void)
{
    if (controller == XHCI)
        return "xHCI";
    if (controller == UHCI)
        return "UHCI";
    return "brak";
}

int usbhc_init(void)
{
    if (xhci_init()) {
        controller = XHCI;
        return 1;
    }
    if (uhci_init()) {
        controller = UHCI;
        return 1;
    }
    controller = NONE;
    return 0;
}

int usbhc_ports(void)
{
    if (controller == XHCI)
        return xhci_ports();
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

int usbhc_attach(int port)
{
    if (controller == XHCI) {
        int slot = xhci_attach(port);
        if (slot < 0)
            return -1;
        /* The handle is the slot number; so that it does not get
           confused with UHCI handles, we store it the same way - each
           backend reads its own */
        return slot;
    }
    if (controller != UHCI)
        return -1;

    for (int i = 0; i < DEVICES; i++) {
        if (list[i].uzyte)
            continue;
        struct device u = { 0, 0, 0, 8, 0, 8, 0 };
        if (uhci_greet(port, &u) < 0)
            return -1;
        u.uzyte = 1;
        list[i] = u;
        return i + 1;
    }
    return -1;
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
