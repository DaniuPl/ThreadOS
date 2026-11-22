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
#include "include/usbmass.h"
#include "include/fat.h"
#include "include/string.h"
#include "../kernel64/paging64.h"

enum backend { BLK_NONE, BLK_NVME, BLK_AHCI, BLK_ATA, BLK_USB };

struct dev {
    enum backend kind;
    int unit;                    /* which one, within that driver */
    uint32_t sectors;
    int gone;                    /* pulled out of the socket */
    /* Written since start, and in how many writes: every write costs
       a disk a fixed price before the first byte moves, so the two
       together say whether a filesystem talks to it sensibly */
    uint32_t written, writes;
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
    /* USB disks need no pointing: the unit number travels with every
       command, because the device is addressed by its slot on the bus
       rather than by a register somebody set earlier. */
}

static void add(enum backend kind, int unit, uint32_t sectors)
{
    if (ndevs >= BLK_MAX || !sectors)
        return;
    devs[ndevs].kind = kind;
    devs[ndevs].unit = unit;
    devs[ndevs].sectors = sectors;
    devs[ndevs].gone = 0;
    devs[ndevs].written = 0;
    devs[ndevs].writes = 0;
    ndevs++;
}

/* --- disks that arrive and leave while the machine runs -------------
 *
 * A stick pushed into a socket is a new disk, and the numbers already
 * on the list must not move: somebody is looking at 'disks' and about
 * to type 'install text whole 2'. So a disk that leaves keeps its
 * place and its number, and is marked gone; a disk that arrives is
 * added at the end.
 */
int blk_usb_added(int unit)
{
    for (int i = 0; i < ndevs; i++) {
        if (devs[i].kind == BLK_USB && devs[i].unit == unit) {
            devs[i].gone = 0;             /* the same one came back */
            devs[i].sectors = usbmass_sectors(unit);
            return i;
        }
    }
    add(BLK_USB, unit, usbmass_sectors(unit));
    return ndevs - 1;
}

void blk_usb_gone(int unit)
{
    for (int i = 0; i < ndevs; i++) {
        if (devs[i].kind != BLK_USB || devs[i].unit != unit)
            continue;
        devs[i].gone = 1;
        /* Anything mounted from it stops being mounted, and nothing is
           written back: there is nowhere to write it to. */
        fat_forget(i);
        /* If the machine was pointed at it, point it back at the first
           disk - otherwise the next write goes to a socket with
           nothing in it. */
        if (cur == i) {
            cur = 0;
            point();
        }
    }
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

    /* And the disk somebody plugged in. Last on the list on purpose:
       a machine that has a disk of its own is to keep booting from it,
       whatever is in the socket. */
    if (usbmass_init()) {
        for (int i = 0; i < usbmass_count(); i++)
            add(BLK_USB, i, usbmass_sectors(i));
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

int blk_has_ata(void)
{
    for (int i = 0; i < ndevs; i++)
        if (devs[i].kind == BLK_ATA)
            return 1;
    return 0;
}

int blk_current(void)
{
    return cur;
}

static const char *kind_name(enum backend k)
{
    switch (k) {
    case BLK_NVME: return "NVMe (PCIe, queues)";
    case BLK_AHCI: return "AHCI (SATA, DMA)";
    case BLK_ATA:  return "ATA PIO";
    case BLK_USB:  return "USB (mass storage)";
    default:       return "none";
    }
}

const char *blk_name_of(int i)
{
    if (i < 0 || i >= ndevs)
        return "none";
    if (devs[i].gone)
        return "USB (pulled out of the socket)";
    return kind_name(devs[i].kind);
}

uint32_t blk_sectors_of(int i)
{
    return (i < 0 || i >= ndevs) ? 0 : devs[i].sectors;
}

void blk_write_stats(int i, uint32_t *sectors, uint32_t *writes)
{
    *sectors = (i < 0 || i >= ndevs) ? 0 : devs[i].written;
    *writes = (i < 0 || i >= ndevs) ? 0 : devs[i].writes;
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

/* A buffer a device cannot be given: memory the kernel reaches through
   the direct map rather than at its own address (kernel64/paging64.h) -
   a program's page on its way to or from the swap file. A controller
   that moves data itself (AHCI, USB) is told an address, and that
   address would be wrong; so such a buffer goes through one of ours,
   a page at a time. Disk calls come one at a time already (a
   controller has one command in flight), so one buffer is enough. */
static uint8_t bounce[4096] __attribute__((aligned(4096)));

static int far(const void *buf, uint32_t count)
{
    return (uint64_t)(uintptr_t)buf + (uint64_t)count * ATA_SECTOR_SIZE >
           paging64_identity_top();
}

#define BOUNCE_SECTORS (sizeof(bounce) / ATA_SECTOR_SIZE)

static int read_near(uint32_t lba, uint32_t count, void *buf);
static int write_near(uint32_t lba, uint32_t count, const void *buf);

int blk_read(uint32_t lba, uint32_t count, void *buf)
{
    if (!far(buf, count))
        return read_near(lba, count, buf);
    uint8_t *p = buf;
    for (uint32_t done = 0; done < count; ) {
        uint32_t n = count - done < BOUNCE_SECTORS ? count - done
                                                   : (uint32_t)BOUNCE_SECTORS;
        if (read_near(lba + done, n, bounce) < 0)
            return -1;
        memcpy(p + done * ATA_SECTOR_SIZE, bounce, n * ATA_SECTOR_SIZE);
        done += n;
    }
    return 0;
}

int blk_write(uint32_t lba, uint32_t count, const void *buf)
{
    if (!far(buf, count))
        return write_near(lba, count, buf);
    const uint8_t *p = buf;
    for (uint32_t done = 0; done < count; ) {
        uint32_t n = count - done < BOUNCE_SECTORS ? count - done
                                                   : (uint32_t)BOUNCE_SECTORS;
        memcpy(bounce, p + done * ATA_SECTOR_SIZE, n * ATA_SECTOR_SIZE);
        if (write_near(lba + done, n, bounce) < 0)
            return -1;
        done += n;
    }
    return 0;
}

/* Nothing past the device's end, whoever asks: what lies past it is
   not this disk's, and a controller that does not check (an old ATA
   one, a cheap USB bridge) would wrap or write somewhere else */
static int in_device(uint32_t lba, uint32_t count)
{
    uint32_t n = devs[cur].sectors;
    return n && lba < n && count <= n - lba;
}

/* An ATA sector that failed is asked for twice more: a drive still
   busy with the last flush, or a bus that hiccupped, answers the second
   time - and a sector written again is the same sector */
static int ata_sector(uint32_t lba, void *buf, int write)
{
    for (int tries = 0; tries < 3; tries++)
        if ((write ? ata_write_sector(lba, buf) : ata_read_sector(lba, buf))
            == 0)
            return 0;
    return -1;
}

static int read_near(uint32_t lba, uint32_t count, void *buf)
{
    if (cur < 0 || devs[cur].gone || !in_device(lba, count))
        return -1;
    point();
    if (devs[cur].kind == BLK_NVME)
        return nvme_read(lba, count, buf);
    if (devs[cur].kind == BLK_AHCI)
        return ahci_read(lba, count, buf);
    if (devs[cur].kind == BLK_USB)
        return usbmass_read(devs[cur].unit, lba, count, buf);
    uint8_t *p = buf;
    for (uint32_t i = 0; i < count; i++)
        if (ata_sector(lba + i, p + i * ATA_SECTOR_SIZE, 0) < 0)
            return -1;
    return 0;
}

static int write_near(uint32_t lba, uint32_t count, const void *buf)
{
    if (cur < 0 || devs[cur].gone || !in_device(lba, count))
        return -1;
    point();
    devs[cur].written += count;
    devs[cur].writes++;
    if (devs[cur].kind == BLK_NVME)
        return nvme_write(lba, count, buf);
    if (devs[cur].kind == BLK_AHCI)
        return ahci_write(lba, count, buf);
    if (devs[cur].kind == BLK_USB)
        return usbmass_write(devs[cur].unit, lba, count, buf);
    const uint8_t *p = buf;
    for (uint32_t i = 0; i < count; i++)
        if (ata_sector(lba + i, (void *)(p + i * ATA_SECTOR_SIZE), 1) < 0)
            return -1;
    return 0;
}
