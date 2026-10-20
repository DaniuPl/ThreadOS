#ifndef THREADOS_USBHC_H
#define THREADOS_USBHC_H

#include <stdint.h>

/* USB host controller - the selection layer
 *
 * Just like blk.c chooses between ATA, AHCI and NVMe, and netdev.c
 * between RTL8139 and e1000: nobody above cares which controller sits
 * in the slot. We try xHCI first (newer boards have nothing else any
 * more), then EHCI with its companion, then UHCI.
 *
 * The border runs where the DIFFERENCE ends. Resetting a port, giving
 * a device an address and finding the endpoint zero packet size look
 * completely different in the two controllers - in UHCI the driver
 * assigns the address with an ordinary request, in xHCI the controller
 * does it while allocating a slot. All of that is therefore on this
 * side of the border, under the single word 'attach'. Above it there
 * is only the conversation with the device, which is the same
 * regardless of the controller - and that is why usbkbd.c does not
 * know what it is talking through.
 */

/* Looks for a controller and starts it. 1 = ready. */
int usbhc_init(void);

/* Name of the controller found - to be printed at startup */
const char *usbhc_name(void);

/* The controller one socket belongs to. The same as usbhc_name() except
   on an EHCI machine, where the slow devices are on the companion. */
const char *usbhc_port_hc(int port);

/* When there is no controller: why. NULL when there is one. */
const char *usbhc_why(void);

/* How many ports the controller has */
int usbhc_ports(void);

/* Attaches the device on a port: reset, address, endpoint zero packet
   size. Returns a device handle (>= 0) or -1 when there is nothing on
   the port or it could not be woken. */
int usbhc_attach(int port);

/* "This device is mine now." Said by a driver that has configured a
   device and started talking to it: after that the port answers
   "nothing here" to every other driver, so nobody reads descriptors
   down a wire somebody else is already using. */
void usbhc_claim(int port);

/* A control transfer to a device. 'setup' is the 8-byte request, data
   may go either way (in=1 reads). Returns the number of data bytes
   or -1. */
int usbhc_control(int dev, const void *setup, void *data, int len, int in);

/* Declares an interrupt endpoint for use. xHCI has to learn about it
   before anything is sent through it; UHCI only remembers. 0 = done. */
int usbhc_int_endpoint(int dev, int ep, int maxpacket, int interval);

/* A single look into an interrupt endpoint. Returns the number of
   bytes, 0 when the device has nothing to say, -1 on error. */
int usbhc_int_in(int dev, int ep, void *data, int len);

/* --- devices that come and go ---------------------------------------
 *
 * A driver that can take something plugged in later says so with
 * usbhc_on_plug: its function is called with the port number when
 * something new appears, and returns 1 when it took it. The other one
 * is called when a device disappears, so that whoever was using it
 * stops pretending it is there.
 *
 * usbhc_watch starts the thread that watches the ports - once, however
 * many drivers ask for it.
 */
void usbhc_on_plug(int (*fn)(int port));
void usbhc_on_unplug(void (*fn)(int port));
void usbhc_watch(void);

/* Is anything in this port right now? -1 = this controller cannot say. */
int usbhc_connected(int port);

/* Forgets a port, so that the next device in it is greeted afresh. */
void usbhc_forget(int port);

/* Bulk endpoints - used by mass storage and Bluetooth ACL data.
   Declared in pairs (in and out), because one device uses both
   directions. Available on xHCI and EHCI; UHCI answers -1. */
int usbhc_bulk_endpoint(int dev, int in_ep, int out_ep, int maxpacket);

/* Bulk transfers. They wait for the answer. The number of bytes moved,
   or -1. */
int usbhc_bulk_out(int dev, int ep, const void *data, int len);
int usbhc_bulk_in(int dev, int ep, void *data, int len);

/* The two requests every driver above here makes, spelled once.
 *
 * A control transfer is eight bytes of setup and then perhaps some
 * data, and building those eight bytes is the same arithmetic whether
 * a keyboard or a mouse is asking. It lived in the keyboard driver
 * while the keyboard was the only device; a second one would have
 * copied it, and two copies of a wire format is how they drift. */
int usbhc_descriptor(int dev, uint8_t type, uint8_t index,
                     void *buf, int len);
/* A request with no data stage - SET_CONFIGURATION, SET_IDLE,
   SET_PROTOCOL. */
int usbhc_request(int dev, uint8_t type, uint8_t req,
                  uint16_t value, uint16_t index);

#endif
