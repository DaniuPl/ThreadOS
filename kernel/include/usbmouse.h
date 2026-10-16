#ifndef THREADOS_USBMOUSE_H
#define THREADOS_USBMOUSE_H

/* A USB mouse in the HID boot protocol. Looks for one on the ports the
   controller layer reports and, having found it, starts a thread that
   feeds its movement into the same pointer the PS/2 driver moves.
   Returns 1 when one was found. */
int usbmouse_init(void);
/* Whether one was found and is being read */
int usbmouse_present(void);

#endif
