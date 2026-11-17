/* Minimal PCI access (mechanism 1, ports 0xCF8/0xCFC)
 *
 * Drivers use these helpers to find PCI devices by ID or class, inspect
 * BARs and enable bus mastering for DMA.
 */

#include "include/pci.h"
#include "include/io.h"

#define PCI_ADDR 0xCF8
#define PCI_DATA 0xCFC

#define PCI_CMD_IO      0x0001
#define PCI_CMD_MEM     0x0002
#define PCI_CMD_MASTER  0x0004

uint32_t pci_config_read(int bus, int slot, int func, int off)
{
    uint32_t addr = 0x80000000u | (bus << 16) | (slot << 11) |
                    (func << 8) | (off & 0xFC);
    outl(PCI_ADDR, addr);
    return inl(PCI_DATA);
}

void pci_config_write(int bus, int slot, int func, int off, uint32_t value)
{
    uint32_t addr = 0x80000000u | (bus << 16) | (slot << 11) |
                    (func << 8) | (off & 0xFC);
    outl(PCI_ADDR, addr);
    outl(PCI_DATA, value);
}

uint32_t pci_find_vga_lfb(void)
{
    for (int dev = 0; dev < 32; dev++) {
        uint32_t id = pci_config_read(0, dev, 0, 0x00);
        if ((id & 0xFFFF) == 0xFFFF)
            continue;                       /* no device */

        uint32_t class = pci_config_read(0, dev, 0, 0x08);
        if ((class >> 24) != 0x03)          /* base class 0x03 = display */
            continue;

        uint32_t bar0 = pci_config_read(0, dev, 0, 0x10);
        if (bar0 & 0x01)                    /* an I/O BAR, not a memory one */
            continue;
        return bar0 & 0xFFFFFFF0u;          /* the address = the BAR without the flag bits */
    }
    return 0;
}

static void fill(struct pci_dev *out, int bus, int slot, int func,
                 uint32_t id)
{
    out->bus = bus;
    out->slot = slot;
    out->func = func;
    out->vendor = (uint16_t)(id & 0xFFFF);
    out->device = (uint16_t)(id >> 16);
    for (int i = 0; i < 6; i++)
        out->bar[i] = pci_config_read(bus, slot, func, 0x10 + i * 4);
    out->bar0 = out->bar[0];
    out->irq = (uint8_t)(pci_config_read(bus, slot, func, 0x3C) & 0xFF);
}

/* Does the slot have more than one function (bit 7 of the header
   field)? Controllers built into the bridge can sit on function 2 or
   3 - scanning only the zero ones will never find them. */
static int multifunction(int bus, int slot)
{
    return (pci_config_read(bus, slot, 0, 0x0C) >> 16) & 0x80;
}

/* --- Finding a device ----------------------------------------------------
 *
 * On a PC board the controllers built into the chipset sit on bus 0,
 * and everything on a PCI Express lane - a laptop's Ethernet, its
 * Wi-Fi, an NVMe disk - sits on a bus of its own behind a root port,
 * which to PCI is a bridge. The finders used to look on bus 0 alone:
 * in QEMU every device is there and they found everything, while on a
 * laptop the Device Manager listed the network card and the Wi-Fi as
 * cards nothing used, because their drivers never saw them.
 *
 * So the finders walk the buses the way the firmware numbered them:
 * bus 0 whole first, then the buses behind its bridges, in the order
 * the bridges came, and the buses behind those. Breadth first, so that
 * where a board has two of a kind the chipset's own is still the one
 * found first, as it always was.
 */

typedef int (*visit_fn)(int bus, int slot, int func, uint32_t id, void *ctx);

static int walk(visit_fn visit, void *ctx)
{
    uint8_t queue[256];
    /* A bit for each bus already queued: a bridge set up wrong - its
       secondary bus one of its own ancestors - cannot send us round */
    uint8_t queued[32] = {0};
    int head = 0, tail = 0;

    queue[tail++] = 0;
    queued[0] = 1;
    while (head < tail) {
        int bus = queue[head++];
        for (int slot = 0; slot < 32; slot++) {
            uint32_t first = pci_config_read(bus, slot, 0, 0x00);
            if ((first & 0xFFFF) == 0xFFFF)
                continue;
            int funcs = multifunction(bus, slot) ? 8 : 1;
            for (int func = 0; func < funcs; func++) {
                uint32_t id = func ? pci_config_read(bus, slot, func, 0x00)
                                   : first;
                if ((id & 0xFFFF) == 0xFFFF)
                    continue;
                if (visit(bus, slot, func, id, ctx))
                    return 1;
                /* A PCI-to-PCI bridge (header type 1) and the bus behind
                   it, its secondary one; zero is one the firmware left
                   unnumbered, with nothing reachable behind it */
                uint32_t hdr = (pci_config_read(bus, slot, func, 0x0C) >> 16)
                               & 0x7F;
                if (hdr != 1)
                    continue;
                int sec = (int)((pci_config_read(bus, slot, func, 0x18) >> 8)
                                & 0xFF);
                if (sec && !(queued[sec >> 3] & (1u << (sec & 7))) &&
                    tail < 256) {
                    queued[sec >> 3] |= (uint8_t)(1u << (sec & 7));
                    queue[tail++] = (uint8_t)sec;
                }
            }
        }
    }
    return 0;
}

struct wanted {
    int by_class;
    uint16_t vendor, device;
    uint8_t base, sub, progif;
    int skip;                      /* matching ones to pass over first */
    struct pci_dev *out;
};

static int is_wanted(int bus, int slot, int func, uint32_t id, void *ctx)
{
    struct wanted *w = ctx;
    if (w->by_class) {
        uint32_t class = pci_config_read(bus, slot, func, 0x08);
        if ((class >> 24) != w->base || ((class >> 16) & 0xFF) != w->sub ||
            ((class >> 8) & 0xFF) != w->progif)
            return 0;
    } else if ((id & 0xFFFF) != w->vendor || (id >> 16) != w->device) {
        return 0;
    }
    if (w->skip-- > 0)
        return 0;
    fill(w->out, bus, slot, func, id);
    return 1;
}

int pci_find_device(uint16_t vendor, uint16_t device, struct pci_dev *out)
{
    struct wanted w = { 0, vendor, device, 0, 0, 0, 0, out };
    return walk(is_wanted, &w);
}

/* Searching by class: this is how we come upon a controller whose
   model we do not know up front - every AHCI controller reports itself
   as 0x01/0x06/0x01, no matter whose it is. */
int pci_find_class(uint8_t base, uint8_t sub, uint8_t progif,
                   struct pci_dev *out)
{
    return pci_find_class_nth(base, sub, progif, 0, out);
}

int pci_find_class_nth(uint8_t base, uint8_t sub, uint8_t progif, int n,
                       struct pci_dev *out)
{
    struct wanted w = { 1, 0, 0, base, sub, progif, n, out };
    return n >= 0 && walk(is_wanted, &w);
}

/* Every device on every bus, one call at a time.
 *
 * The finders above answer "is there one of these"; this answers "what
 * is in this machine", which is the question one actually has in front
 * of unknown hardware. A laptop that will not move its pointer is
 * either a laptop with nothing on the PS/2 port or a laptop whose
 * touchpad is on a bus we do not speak - and the difference is visible
 * here and nowhere else. */
int pci_enumerate(int index, struct pci_dev *out, uint32_t *class_out)
{
    if (index < 0)
        return -1;
    int seen = 0;
    for (int bus = 0; bus < 256; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            uint32_t first_id = pci_config_read(bus, slot, 0, 0x00);
            if ((first_id & 0xFFFF) == 0xFFFF)
                continue;
            int funcs = multifunction(bus, slot) ? 8 : 1;
            for (int func = 0; func < funcs; func++) {
                uint32_t id = func == 0
                                  ? first_id
                                  : pci_config_read(bus, slot, func, 0x00);
                if ((id & 0xFFFF) == 0xFFFF)
                    continue;
                if (seen++ != index)
                    continue;
                fill(out, bus, slot, func, id);
                if (class_out)
                    *class_out = pci_config_read(bus, slot, func, 0x08);
                return 1;
            }
        }
    }
    return 0;
}

void pci_enable_bus_master(const struct pci_dev *dev)
{
    uint32_t cmd = pci_config_read(dev->bus, dev->slot, dev->func, 0x04);
    cmd |= PCI_CMD_IO | PCI_CMD_MEM | PCI_CMD_MASTER;
    pci_config_write(dev->bus, dev->slot, dev->func, 0x04, cmd);
}
