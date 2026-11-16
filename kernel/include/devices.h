#ifndef THREADOS_DEVICES_H
#define THREADOS_DEVICES_H

#include "pci.h"
#include "device_info.h"

/* Every device in the machine and what became of it: the Device
 * Manager's list (Settings, Devices) and 'devices' in the shell.
 *
 * The PCI bus is walked each time, so the list has everything on it
 * whether or not anything drives it. What a driver did is what the
 * driver says here: it claims the device when it finds it and says
 * when the device is up, so a driver that gives up half way leaves
 * its device standing as failed, which is the truth.
 */

/* A driver has found its device and is starting it. Until device_up()
   the device counts as failed - the paths a start can fail on are
   many, and every one of them then says so without a word. */
void device_claim(const struct pci_dev *d, const char *driver);
void device_up(const struct pci_dev *d);
/* Something more particular than "it did not start": DEV_FIRMWARE
   when the maker's firmware is missing, DEV_FAILED with the reason */
void device_trouble(const struct pci_dev *d, int state, const char *why);
/* A driver whose device's state lives in the driver itself - the
   Wi-Fi card, which starts in a thread of its own and can stop later -
   is asked each time: the function returns a DEV_* and fills 'why' */
void device_live(const struct pci_dev *d, int (*state)(char *why, int n));

/* Device 'index' of the list, PCI first, then USB: 1 = filled in,
   0 = past the end */
int devices_get(int index, struct device_info *out);
/* The list, a line for each, for the shell */
void devices_print(void);

#endif
