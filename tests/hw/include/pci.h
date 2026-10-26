#ifndef HW_PCI_H
#define HW_PCI_H
#include <stdint.h>
struct pci_dev {
    int bus, slot, func;
    uint16_t vendor, device;
    uint32_t bar0;
    uint32_t bar[6];
    uint8_t irq;
};
int pci_find_device(uint16_t vendor, uint16_t device, struct pci_dev *out);
void pci_enable_bus_master(const struct pci_dev *dev);
#endif
