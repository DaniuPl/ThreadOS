#ifndef PROTONOS_ATA_H
#define PROTONOS_ATA_H

#include <stdint.h>

#define ATA_SECTOR_SIZE 512

/* Is there a disk on the first bus (master) */
int ata_present(void);

/* Disk capacity in sectors (IDENTIFY DEVICE, LBA28).
   0 = could not be read. */
uint32_t ata_sectors(void);

/* Read/write of a single 512 B sector (LBA28, PIO mode, polling).
   Returns 0 on success, -1 on error. */
int ata_read_sector(uint32_t lba, void *buf);
int ata_write_sector(uint32_t lba, const void *buf);

#endif
