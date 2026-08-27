/* Minimalny dostep do PCI (mechanizm 1, porty 0xCF8/0xCFC)
 *
 * Uzywany dwojako:
 *  - znalezienie adresu liniowego framebuffera karty graficznej
 *    (klasa 0x03, BAR0 -> fizyczny adres pamieci obrazu),
 *  - znalezienie karty sieciowej RTL8139 po vendor/device oraz
 *    wlaczenie jej bus masteringu (DMA), bez ktorego nie ruszy.
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
            continue;                       /* brak urzadzenia */

        uint32_t class = pci_config_read(0, dev, 0, 0x08);
        if ((class >> 24) != 0x03)          /* klasa bazowa 0x03 = display */
            continue;

        uint32_t bar0 = pci_config_read(0, dev, 0, 0x10);
        if (bar0 & 0x01)                    /* BAR I/O, nie pamieciowy */
            continue;
        return bar0 & 0xFFFFFFF0u;          /* adres = BAR bez bitow flag */
    }
    return 0;
}

static void fill(struct pci_dev *out, int slot, uint32_t id)
{
    out->bus = 0;
    out->slot = slot;
    out->vendor = (uint16_t)(id & 0xFFFF);
    out->device = (uint16_t)(id >> 16);
    for (int i = 0; i < 6; i++)
        out->bar[i] = pci_config_read(0, slot, 0, 0x10 + i * 4);
    out->bar0 = out->bar[0];
    out->irq = (uint8_t)(pci_config_read(0, slot, 0, 0x3C) & 0xFF);
}

int pci_find_device(uint16_t vendor, uint16_t device, struct pci_dev *out)
{
    for (int slot = 0; slot < 32; slot++) {
        uint32_t id = pci_config_read(0, slot, 0, 0x00);
        if ((id & 0xFFFF) != vendor || (id >> 16) != device)
            continue;

        fill(out, slot, id);
        return 1;
    }
    return 0;
}

/* Szuka po klasie: tak trafiamy na kontroler, ktorego modelu nie znamy
   z gory - kazdy kontroler AHCI zglasza sie jako 0x01/0x06/0x01,
   niezaleznie od tego, czyj jest. */
int pci_find_class(uint8_t base, uint8_t sub, uint8_t progif,
                   struct pci_dev *out)
{
    for (int slot = 0; slot < 32; slot++) {
        uint32_t id = pci_config_read(0, slot, 0, 0x00);
        if ((id & 0xFFFF) == 0xFFFF)
            continue;

        uint32_t class = pci_config_read(0, slot, 0, 0x08);
        if ((class >> 24) != base || ((class >> 16) & 0xFF) != sub ||
            ((class >> 8) & 0xFF) != progif)
            continue;

        fill(out, slot, id);
        return 1;
    }
    return 0;
}

void pci_enable_bus_master(const struct pci_dev *dev)
{
    uint32_t cmd = pci_config_read(dev->bus, dev->slot, 0, 0x04);
    cmd |= PCI_CMD_IO | PCI_CMD_MEM | PCI_CMD_MASTER;
    pci_config_write(dev->bus, dev->slot, 0, 0x04, cmd);
}
