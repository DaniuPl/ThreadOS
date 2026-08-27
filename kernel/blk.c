/* Warstwa urzadzenia blokowego - wybor sterownika dysku
 *
 * AHCI ma pierwszenstwo: przenosi dane sam, przez DMA, i na nowszych
 * plytach jest jedynym sposobem zobaczenia dysku. Stary ATA w trybie
 * PIO zostaje jako droga zapasowa - dziala wszedzie, ale kazde slowo
 * przepisuje procesor.
 */

#include "include/blk.h"
#include "include/ahci.h"
#include "include/ata.h"

enum backend { BLK_NONE, BLK_AHCI, BLK_ATA };
static enum backend backend;

int blk_init(void)
{
    if (ahci_init()) {
        backend = BLK_AHCI;
        return 1;
    }
    if (ata_present()) {
        backend = BLK_ATA;
        return 1;
    }
    backend = BLK_NONE;
    return 0;
}

int blk_present(void)
{
    return backend != BLK_NONE;
}

const char *blk_name(void)
{
    switch (backend) {
    case BLK_AHCI: return "AHCI (SATA, DMA)";
    case BLK_ATA:  return "ATA PIO";
    default:       return "brak";
    }
}

uint32_t blk_sectors(void)
{
    switch (backend) {
    case BLK_AHCI: return ahci_sectors();
    case BLK_ATA:  return ata_sectors();
    default:       return 0;
    }
}

int blk_read(uint32_t lba, uint32_t count, void *buf)
{
    if (backend == BLK_AHCI)
        return ahci_read(lba, count, buf);
    if (backend != BLK_ATA)
        return -1;
    uint8_t *p = buf;
    for (uint32_t i = 0; i < count; i++)
        if (ata_read_sector(lba + i, p + i * ATA_SECTOR_SIZE) < 0)
            return -1;
    return 0;
}

int blk_write(uint32_t lba, uint32_t count, const void *buf)
{
    if (backend == BLK_AHCI)
        return ahci_write(lba, count, buf);
    if (backend != BLK_ATA)
        return -1;
    const uint8_t *p = buf;
    for (uint32_t i = 0; i < count; i++)
        if (ata_write_sector(lba + i, p + i * ATA_SECTOR_SIZE) < 0)
            return -1;
    return 0;
}
