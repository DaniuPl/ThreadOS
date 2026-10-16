#ifndef THREADOS_UHCI_H
#define THREADOS_UHCI_H

#include <stdint.h>

/* USB host controller (UHCI) - the transport layer
 *
 * It knows just enough to talk to a device: control transfers
 * (descriptor queries, settings) and interrupt transfers (data
 * arriving from a keyboard). There is nothing about keyboards here -
 * what those bytes mean is decided by usbkbd.c.
 *
 * UHCI is the oldest of the three USB controllers and the simplest: we
 * drive it through I/O ports, and transfer descriptors are arranged in
 * a frame list. Newer boards have only xHCI - but all the code for
 * recognising devices and translating HID reports is independent of
 * the controller and stays unchanged when xHCI is added.
 */

/* Looks for the controller on the PCI bus and starts it. 1 = ready. */
int uhci_init(void);

/* How many ports the controller has and whether anything hangs on one */
int uhci_ports(void);
int uhci_port_reset(int port);          /* 1 = the device is ready to talk */
int uhci_port_lowspeed(int port);
int uhci_port_connected(int port);     /* 1 = something is plugged in */

/* A control transfer. 'setup' is the 8-byte request, data may go
   either way (in=1 reads). Returns the number of data bytes or -1. */
int uhci_control(int addr, int lowspeed, int maxpacket,
                 const void *setup, void *data, int len, int in);

/* A single read from an interrupt endpoint. Returns the number of
   bytes, 0 when the device has nothing to say, -1 on error. */
int uhci_interrupt_in(int addr, int lowspeed, int endpoint, int maxpacket,
                      void *data, int len, int *toggle);

#endif
