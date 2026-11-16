/* The devices in the machine, as a program sees them: what each one is
   and what became of it. The layout is kernel/include/device_info.h's
   and has to stay alike. */
#ifndef THREADOS_USER_UDEVICES_H
#define THREADOS_USER_UDEVICES_H

#include "uapi.h"

#define DEV_BUS_PCI 1
#define DEV_BUS_USB 2

#define DEV_FAILED    0
#define DEV_FIRMWARE  1
#define DEV_NO_DRIVER 2
#define DEV_SPARE     3
#define DEV_STARTING  4
#define DEV_WORKING   5
#define DEV_PLUMBING  6

struct device_info {
    uint8_t bus;
    uint8_t state;
    uint8_t cls, sub, prog;
    uint8_t where[3];
    uint16_t vendor, device;
    char driver[16];
    char name[40];
    char why[64];
};

/* Device 'index' of the list, PCI first, then USB: 1 = filled in,
   0 = past the end */
static inline int sys_device(int index, struct device_info *out)
{
    return (int)syscall64(SYS64_DEVICE, index, (int64_t)(uintptr_t)out);
}

#endif
