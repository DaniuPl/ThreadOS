#ifndef THREADOS_PCI_INFO_H
#define THREADOS_PCI_INFO_H

#include <stdint.h>

struct pci_info {
    uint32_t bus;
    uint32_t slot;
    uint32_t function;
    uint32_t vendor;
    uint32_t device;
    uint32_t class_code;
};

#endif
