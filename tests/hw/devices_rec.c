/* The Device Manager's registry for a model on the host: see
   devices_rec.h. The driver is compiled against the kernel's own
   include/devices.h, so these are the calls it really makes. */
#include <string.h>
#include "devices_rec.h"
#include "include/devices.h"

struct dev_record dev_rec = { .state = -1 };

static void copy(char *dst, const char *src, size_t n)
{
    strncpy(dst, src ? src : "", n - 1);
    dst[n - 1] = 0;
}

void device_claim(const struct pci_dev *d, const char *driver)
{
    dev_rec.claims++;
    dev_rec.bus = d->bus;
    dev_rec.slot = d->slot;
    dev_rec.func = d->func;
    copy(dev_rec.driver, driver, sizeof(dev_rec.driver));
    dev_rec.up = 0;
    dev_rec.state = -1;
    dev_rec.why[0] = 0;
}

void device_up(const struct pci_dev *d)
{
    (void)d;
    dev_rec.up = 1;
}

void device_trouble(const struct pci_dev *d, int state, const char *why)
{
    (void)d;
    dev_rec.state = state;
    copy(dev_rec.why, why, sizeof(dev_rec.why));
}

void device_live(const struct pci_dev *d, int (*state)(char *why, int n))
{
    (void)d;
    dev_rec.live = state;
}

/* The two the drivers never call: the list is the real registry's */
int devices_get(int index, struct device_info *out)
{
    (void)index;
    (void)out;
    return 0;
}

void devices_print(void)
{
}
