#ifndef PROTONOS_PCI_H
#define PROTONOS_PCI_H

#include <stdint.h>

/* Finds the linear framebuffer of a graphics card (BAR0 of a display
   class device) through PCI configuration space. Returns a physical
   address or 0 when nothing was found. */
uint32_t pci_find_vga_lfb(void);

/* A PCI device that was found (bus 0, function 0) */
struct pci_dev {
    int bus, slot, func;   /* one slot may have several functions */
    uint16_t vendor, device;
    uint32_t bar0;        /* raw contents of BAR0 (bit 0: 1 = I/O port) */
    uint32_t bar[6];      /* all base registers, raw */
    uint8_t irq;          /* the IRQ line from the Interrupt Line field */
};

/* Looks for a device with the given vendor/device. 1 = found. */
int pci_find_device(uint16_t vendor, uint16_t device, struct pci_dev *out);

/* Looks for a device by class (class/subclass/programming interface) -
   this is how one finds a controller whose model number is not known
   up front, e.g. any AHCI controller (0x01/0x06/0x01). 1 = found. */
int pci_find_class(uint8_t base, uint8_t sub, uint8_t progif,
                   struct pci_dev *out);

/* Walks every device on the bus: index 0, 1, 2... until it returns 0.
   'class_out' takes the raw class register (class, subclass, prog-if
   and revision), because on unknown hardware the class is the useful
   half of the answer. */
int pci_enumerate(int index, struct pci_dev *out, uint32_t *class_out);

/* Enables I/O port access, memory access and bus mastering (DMA) */
void pci_enable_bus_master(const struct pci_dev *dev);

uint32_t pci_config_read(int bus, int slot, int func, int off);
void pci_config_write(int bus, int slot, int func, int off, uint32_t value);

#endif
