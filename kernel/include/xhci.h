#ifndef THREADOS_XHCI_H
#define THREADOS_XHCI_H

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

/* Where the start-up stopped, when it did - a sentence for a person,
   or NULL when there is nothing to explain. */
const char *xhci_why(void);

/* Port reset, slot allocation and address assignment. Returns the slot
   number (1..MAX) or -1. */
int xhci_attach(int port);

/* Why the last attach or address failed - a sentence for a person, or
   NULL when none has failed yet. */
const char *xhci_attach_why(void);

/* The same, for a device that is NOT on a root port: behind hubs. The
   port has already been reset by the hub above, so this only gives the
   device a slot and an address - but it has to be told the way there.

   'route' is the route string: four bits per tier of hubs, the first
   tier lowest. 'port' is still the ROOT port the whole tree hangs off.
   'tt_slot' and 'tt_port' name the high-speed hub answering for a
   slower device below it (0 when there is none). */
int xhci_address(int port, uint32_t route, int speed, int tt_slot,
                 int tt_port);

/* Tells the controller that a device is a hub, and how many ports it
   has - without this it will not address anything behind it. */
int xhci_mark_hub(int slot, int ports, int multi_tt, int think_time);

/* A control transfer through a slot's endpoint zero */
int xhci_control(int slot, const void *setup, void *data, int len, int in);

/* Sets up an IN interrupt endpoint with number 'ep' */
int xhci_int_endpoint(int slot, int ep, int maxpacket, int interval);

/* Looks into an interrupt endpoint. >0 = that many bytes, 0 = nothing
   has arrived yet, -1 = error. */
int xhci_int_in(int slot, int ep, void *data, int len);

/* What speed the controller sees on a root port (its own numbering:
   1 full, 2 low, 3 high, 4 super). */
int xhci_speed(int port);

/* Is anything plugged into this port at this moment? */
int xhci_port_connected(int port);

/* Hands a slot back to the controller - after the device has gone. */
int xhci_release(int slot);

/* Declares the pair of bulk endpoints a disk speaks through - both at
   once, because one command replaces the whole description of the
   device. 0 = done. */
int xhci_bulk_endpoint(int slot, int in_ep, int out_ep, int maxpacket);

/* Bulk transfers. These WAIT for the answer, unlike the interrupt one:
   there is nothing sensible to do between telling a disk to read a
   sector and having it. Return the number of bytes transferred or
   -1. The buffer is read by the controller itself, so it has to lie in
   memory it can reach. */
int xhci_bulk_out(int slot, int ep, const void *data, int len);
int xhci_bulk_in(int slot, int ep, void *data, int len);

/* An isochronous IN endpoint - a camera's - for one stream at a time.
   open: declares it ('raw_mps' as the descriptor writes it, extra
   transactions in bits 12:11): 0, -1, or -2 when the bus has no room.
   start: fills the ring and starts it. poll: hands every TD that came
   back to fn (data and length, or lost=1) and refills: the number
   handed out. close: stops and drops it. lost: how often the ring ran
   dry or a service interval passed with no TD. */
int xhci_iso_open(int slot, int ep, int raw_mps, int binterval);
int xhci_iso_start(int slot);
int xhci_iso_poll(int slot, void (*fn)(void *arg, const uint8_t *data,
                                        int len, int lost), void *arg);
int xhci_iso_close(int slot);
int xhci_iso_lost(void);

#endif
