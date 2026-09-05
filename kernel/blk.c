/* The block device layer - the disks the machine has, and the one in use
 *
 * There used to be one disk here. The layer looked at NVMe, then at
 * AHCI, then at old ATA, took the first thing it found and that was
 * the disk - which is right for a machine with one, and wrong the
 * moment somebody is asked which disk to install onto.
 *
 * So now it is a list. The order is still from newer to older - NVMe
 * sits straight on PCI Express, AHCI moves data itself through DMA,
 * ATA PIO copies every word through a port and remains as the thing
 * that works everywhere - and the first one on the list is the one in
 * use, which makes a machine with one disk behave exactly as it did.
 *
 * Everything above this - the filesystem, the partition table, FAT -
 * reads and writes THE DISK IN USE. That is deliberate: threading a
 * device number through every one of them would be a change to every
 * one of them, for a question that is asked once, by a person, in an
 * installer.
 */

#include "include/blk.h"
#include "include/nvme.h"
#include "include/ahci.h"
#include "include/ata.h"

enum backend { BLK_NONE, BLK_NVME, BLK_AHCI, BLK_ATA };

struct dev {
    enum backend kind;
    int unit;                    /* which one, within that driver */
    uint32_t sectors;
};

static struct dev devs[BLK_MAX];
static int ndevs;
static int cur = -1;

/* Points the chosen driver at the chosen unit. Called before every
   read and write rather than only when the choice changes: two
   drivers keep a selection of their own, and something else may have
   moved it in between. */
static void point(void)
{
    if (cur < 0 || cur >= ndevs)
        return;
    if (devs[cur].kind == BLK_AHCI)
        ahci_select(devs[cur].unit);
    else if (devs[cur].kind == BLK_ATA)
        ata_select(devs[cur].unit);
}

static void add(enum backend kind, int unit, uint32_t sectors)
{
    if (ndevs >= BLK_MAX || !sectors)
        return;
    devs[ndevs].kind = kind;
    devs[ndevs].unit = unit;
    devs[ndevs].sectors = sectors;
    ndevs++;
}

int blk_init(void)
{
    ndevs = 0;
    cur = -1;

    if (nvme_init())
        add(BLK_NVME, 0, nvme_sectors());

    int sata = ahci_init();
    for (int i = 0; i < sata; i++) {
        ahci_select(i);
        add(BLK_AHCI, i, ahci_sectors());
    }

    /* The old bus, and only drives that answer IDENTIFY with a size.
       An empty bus reads back as ones, and a bus that is there with
       nothing on it answers zero - either way there is no disk, and
       listing one would be listing something nobody can read. */
    if (ata_present()) {
        for (int u = 0; u < 2; u++) {
            ata_select(u);
            add(BLK_ATA, u, ata_sectors());
        }
        ata_select(0);
    }

    if (ndevs) {
        cur = 0;
        point();
    }
    return ndevs > 0;
}

int blk_present(void)
{
    return cur >= 0;
}

int blk_count(void)
{
    return ndevs;
}

int blk_current(void)
{
    return cur;
}

static const char *kind_name(enum backend k)
{
    switch (k) {
    case BLK_NVME: return "NVMe (PCIe, kolejki)";
    case BLK_AHCI: return "AHCI (SATA, DMA)";
    case BLK_ATA:  return "ATA PIO";
    default:       return "brak";
    }
}

const char *blk_name_of(int i)
{
    if (i < 0 || i >= ndevs)
        return "brak";
    return kind_name(devs[i].kind);
}

uint32_t blk_sectors_of(int i)
{
    return (i < 0 || i >= ndevs) ? 0 : devs[i].sectors;
}

const char *blk_name(void)
{
    return blk_name_of(cur);
}

uint32_t blk_sectors(void)
{
    return blk_sectors_of(cur);
}

int blk_use(int i)
{
    if (i < 0 || i >= ndevs)
        return -1;
    cur = i;
    point();
    return 0;
}

int blk_read(uint32_t lba, uint32_t count, void *buf)
{
    if (cur < 0)
        return -1;
    point();
    if (devs[cur].kind == BLK_NVME)
        return nvme_read(lba, count, buf);
    if (devs[cur].kind == BLK_AHCI)
        return ahci_read(lba, count, buf);
    uint8_t *p = buf;
    for (uint32_t i = 0; i < count; i++)
        if (ata_read_sector(lba + i, p + i * ATA_SECTOR_SIZE) < 0)
            return -1;
    return 0;
}

int blk_write(uint32_t lba, uint32_t count, const void *buf)
{
    if (cur < 0)
        return -1;
    point();
    if (devs[cur].kind == BLK_NVME)
        return nvme_write(lba, count, buf);
    if (devs[cur].kind == BLK_AHCI)
        return ahci_write(lba, count, buf);
    const uint8_t *p = buf;
    for (uint32_t i = 0; i < count; i++)
        if (ata_write_sector(lba + i, p + i * ATA_SECTOR_SIZE) < 0)
            return -1;
    return 0;
}
