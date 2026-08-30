#ifndef PROTONOS_XHCI_H
#define PROTONOS_XHCI_H

/* USB host controller (xHCI) - the transport layer
 *
 * The third and last of the USB controllers, the only one on newer
 * boards. It differs from UHCI not in details but in the division of
 * labour: there the driver arranged transfer descriptors in a frame
 * list and kept track of device addresses itself; here one talks to
 * the controller with COMMANDS placed into a ring, and the answers
 * come back through a second ring - of events. Device addresses are
 * assigned by the controller.
 *
 * Called only by usbhc.c; usbkbd.c does not know this exists.
 */

/* Looks for the controller on the PCI bus and starts it. 1 = ready. */
int xhci_init(void);

/* How many ports the controller has */
int xhci_ports(void);

/* Port reset, slot allocation and address assignment. Returns the slot
   number (1..MAX) or -1. */
int xhci_attach(int port);

/* A control transfer through a slot's endpoint zero */
int xhci_control(int slot, const void *setup, void *data, int len, int in);

/* Sets up an IN interrupt endpoint with number 'ep' */
int xhci_int_endpoint(int slot, int ep, int maxpacket, int interval);

/* Looks into an interrupt endpoint. >0 = that many bytes, 0 = nothing
   has arrived yet, -1 = error. */
int xhci_int_in(int slot, int ep, void *data, int len);

#endif
