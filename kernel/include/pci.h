#ifndef PROTONOS_PCI_H
#define PROTONOS_PCI_H

#include <stdint.h>

/* Znajduje liniowy framebuffer karty graficznej (BAR0 urzadzenia klasy
   display) przez przestrzen konfiguracyjna PCI. Zwraca adres fizyczny
   albo 0, gdy nie znaleziono. */
uint32_t pci_find_vga_lfb(void);

#endif
