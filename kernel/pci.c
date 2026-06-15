/* Minimalny dostep do PCI (mechanizm 1, porty 0xCF8/0xCFC)
 *
 * Potrzebny tylko po to, by znalezc adres liniowego framebuffera karty
 * graficznej: skanujemy magistrale 0, szukamy urzadzenia klasy "display
 * controller" (0x03) i czytamy jego BAR0 - to fizyczny adres pamieci
 * obrazu (w QEMU std-vga zwykle wysoko, np. 0xFD000000).
 */

#include "include/pci.h"
#include "include/io.h"

#define PCI_ADDR 0xCF8
#define PCI_DATA 0xCFC

static uint32_t pci_read(int bus, int dev, int func, int off)
{
    uint32_t addr = 0x80000000u | (bus << 16) | (dev << 11) |
                    (func << 8) | (off & 0xFC);
    outl(PCI_ADDR, addr);
    return inl(PCI_DATA);
}

uint32_t pci_find_vga_lfb(void)
{
    for (int dev = 0; dev < 32; dev++) {
        uint32_t id = pci_read(0, dev, 0, 0x00);
        if ((id & 0xFFFF) == 0xFFFF)
            continue;                       /* brak urzadzenia */

        uint32_t class = pci_read(0, dev, 0, 0x08);
        if ((class >> 24) != 0x03)          /* klasa bazowa 0x03 = display */
            continue;

        uint32_t bar0 = pci_read(0, dev, 0, 0x10);
        if (bar0 & 0x01)                    /* BAR I/O, nie pamieciowy */
            continue;
        return bar0 & 0xFFFFFFF0u;          /* adres = BAR bez bitow flag */
    }
    return 0;
}
