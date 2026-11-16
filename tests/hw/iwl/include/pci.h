/* PCI on the host: one card, the 3160, in the model's configuration
   space (the same declarations as kernel/include/pci.h) */
#ifndef HW_IWL_PCI_H
#define HW_IWL_PCI_H
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
uint32_t pci_config_read(int bus, int slot, int func, int off);
void pci_config_write(int bus, int slot, int func, int off, uint32_t value);
#endif
