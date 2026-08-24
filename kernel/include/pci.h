#ifndef PROTONOS_PCI_H
#define PROTONOS_PCI_H

#include <stdint.h>

/* Znajduje liniowy framebuffer karty graficznej (BAR0 urzadzenia klasy
   display) przez przestrzen konfiguracyjna PCI. Zwraca adres fizyczny
   albo 0, gdy nie znaleziono. */
uint32_t pci_find_vga_lfb(void);

/* Znalezione urzadzenie PCI (magistrala 0, funkcja 0) */
struct pci_dev {
    int bus, slot;
    uint16_t vendor, device;
    uint32_t bar0;        /* surowa zawartosc BAR0 (bit 0: 1 = port I/O) */
    uint8_t irq;          /* linia IRQ z pola Interrupt Line */
};

/* Szuka urzadzenia o danym vendor/device. 1 = znaleziono. */
int pci_find_device(uint16_t vendor, uint16_t device, struct pci_dev *out);

/* Wlacza dostep do portow I/O, pamieci i bus mastering (DMA) */
void pci_enable_bus_master(const struct pci_dev *dev);

uint32_t pci_config_read(int bus, int slot, int func, int off);
void pci_config_write(int bus, int slot, int func, int off, uint32_t value);

#endif
