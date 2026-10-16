#ifndef THREADOS_USBKBD_H
#define THREADOS_USBKBD_H

/* USB keyboard (HID class, boot protocol)
 *
 * Recognises the device on the USB bus, switches it into the boot
 * protocol - the one the BIOS uses, with a fixed eight-byte report -
 * and hands the pressed keys to the same place PS/2 keys go. For the
 * rest of the system it makes no difference which keyboard is plugged
 * in.
 */

/* Looks for a keyboard on the bus and starts the polling thread.
   1 = found. */
int usbkbd_init(void);
/* Whether one was found and is being read */
int usbkbd_present(void);

#endif
