#ifndef THREADOS_EHCI_H
#define THREADOS_EHCI_H

#include <stdint.h>

/* USB host controller (EHCI) - USB 2.0, the transport layer
 *
 * EHCI speaks to high-speed devices only. Everything slower - the
 * keyboards and mice of this world, which are USB 1.1 devices to this
 * day - it hands over to a "companion" controller (UHCI on Intel
 * boards) by flipping one bit in the port register. So an EHCI machine
 * is really two controllers sharing the same sockets, and usbhc.c runs
 * both: a stick is read through this file, the keyboard in the next
 * socket through uhci.c.
 *
 * Like the other two controllers it is polled, not interrupt-driven.
 */

/* Looks for the controller on the PCI bus and starts it. 1 = ready. */
int ehci_init(void);

/* Why there is no controller, NULL when there is one */
const char *ehci_why(void);

int ehci_ports(void);

/* Resets a root port and says what is on it:
     0  nothing (or nothing that answered)
     3  a high-speed device, ready to be addressed (the speed is in the
        xHCI numbering usbhc.c already uses)
    -1  a slower device - the port has been handed to the companion
        controller, and it will appear there */
int ehci_port_reset(int port);

/* Is a device of OURS in the port: 1/0. A port handed to the companion
   is not ours and says 0. */
int ehci_port_connected(int port);

/* A control transfer. 'setup' is the 8-byte request, data may go
   either way (in=1 reads). Returns the number of data bytes or -1. */
int ehci_control(int addr, int maxpacket, const void *setup,
                 void *data, int len, int in);

/* A bulk transfer, waited for. 'toggle' is the endpoint's data toggle,
   kept by the caller and updated here. Returns bytes moved or -1. */
int ehci_bulk(int addr, int ep, int maxpacket, void *data, int len,
              int in, int *toggle);

/* Declares an interrupt IN endpoint: a queue head in the periodic
   schedule, visited every millisecond. Returns a handle or -1. */
int ehci_int_endpoint(int addr, int ep, int maxpacket);

/* One look into it. Returns the number of bytes, 0 when the device has
   nothing to say yet, -1 on error. Never waits. */
int ehci_int_in(int handle, void *data, int len);

/* Takes the endpoint out of the schedule - the device has gone. */
void ehci_int_release(int handle);

#endif
