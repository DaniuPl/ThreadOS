#ifndef THREADOS_DEVICE_INFO_H
#define THREADOS_DEVICE_INFO_H

#include <stdint.h>

/* What a program is told about one device in the machine
   (SYS64_DEVICE): the Device Manager in Settings, and 'devices' in the
   shell. The same layout is in user/udevices.h, which is what programs
   include: a program cannot reach the kernel's headers, so the two have
   to be kept alike. */

#define DEV_BUS_PCI 1
#define DEV_BUS_USB 2

/* What became of it. The order is the order of urgency: a device
   whose driver failed, or that has none, is what a person opens the
   list to find, so the list sorts by this. */
#define DEV_FAILED    0     /* its driver tried, and it did not start */
#define DEV_FIRMWARE  1     /* its driver needs the maker's firmware */
#define DEV_NO_DRIVER 2     /* nothing in ThreadOS speaks to it */
#define DEV_SPARE     3     /* there is a driver, but it drives another
                               one: ThreadOS uses one network card, one
                               USB controller */
#define DEV_STARTING  4     /* its driver is still bringing it up: the
                               Wi-Fi card loads its firmware in a
                               thread of its own */
#define DEV_WORKING   5     /* a driver has it */
#define DEV_PLUMBING  6     /* the machine's own wiring: a bridge, the
                               host - nothing to drive */

struct device_info {
    uint8_t bus;            /* DEV_BUS_* */
    uint8_t state;          /* DEV_* */
    uint8_t cls, sub, prog; /* the class: PCI's, or the USB device's (or
                               its first interface's) */
    uint8_t where[3];       /* PCI: bus, slot, function; USB: the socket */
    uint16_t vendor, device;
    char driver[16];        /* the driver's name, "" when there is none */
    char name[40];          /* what it is, as well as it is known */
    char why[64];           /* for DEV_FAILED and DEV_FIRMWARE: what
                               went wrong, or what is missing */
};

#endif
