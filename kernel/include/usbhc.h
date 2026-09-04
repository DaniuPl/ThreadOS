#ifndef PROTONOS_USBHC_H
#define PROTONOS_USBHC_H

#include <stdint.h>

/* USB host controller - the selection layer
 *
 * Just like blk.c chooses between ATA, AHCI and NVMe, and netdev.c
 * between RTL8139 and e1000: nobody above cares which controller sits
 * in the slot. We try xHCI first (newer boards have nothing else any
 * more), then UHCI.
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

/* How many ports the controller has */
int usbhc_ports(void);

/* Attaches the device on a port: reset, address, endpoint zero packet
   size. Returns a device handle (>= 0) or -1 when there is nothing on
   the port or it could not be woken. */
int usbhc_attach(int port);

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
