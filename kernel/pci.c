/* Minimal PCI access (mechanism 1, ports 0xCF8/0xCFC)
 *
 * Used in two ways:
 *  - finding the linear framebuffer address of a graphics card
 *    (class 0x03, BAR0 -> the physical address of video memory),
 *  - finding the RTL8139 network card by vendor/device and enabling
 *    its bus mastering (DMA), without which it will not start.
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

int pci_find_device(uint16_t vendor, uint16_t device, struct pci_dev *out)
{
    for (int slot = 0; slot < 32; slot++) {
        int funcs = multifunction(0, slot) ? 8 : 1;
        for (int func = 0; func < funcs; func++) {
            uint32_t id = pci_config_read(0, slot, func, 0x00);
            if ((id & 0xFFFF) != vendor || (id >> 16) != device)
                continue;
            fill(out, 0, slot, func, id);
            return 1;
        }
    }
    return 0;
}

/* Searching by class: this is how we come upon a controller whose
   model we do not know up front - every AHCI controller reports itself
   as 0x01/0x06/0x01, no matter whose it is. */
int pci_find_class(uint8_t base, uint8_t sub, uint8_t progif,
                   struct pci_dev *out)
{
    for (int slot = 0; slot < 32; slot++) {
        int funcs = multifunction(0, slot) ? 8 : 1;
        for (int func = 0; func < funcs; func++) {
            uint32_t id = pci_config_read(0, slot, func, 0x00);
            if ((id & 0xFFFF) == 0xFFFF)
                continue;

            uint32_t class = pci_config_read(0, slot, func, 0x08);
            if ((class >> 24) != base || ((class >> 16) & 0xFF) != sub ||
                ((class >> 8) & 0xFF) != progif)
                continue;

            fill(out, 0, slot, func, id);
            return 1;
        }
    }
    return 0;
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
