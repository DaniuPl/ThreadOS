#ifndef THREADOS_ATA_H
#define THREADOS_ATA_H

#include <stdint.h>

#define ATA_SECTOR_SIZE 512

/* Is there anything on the first bus at all (the lines are not
   floating high) */
int ata_present(void);

/* Two drives share the bus: the master and the slave, and which of
   them the commands go to is one bit in the drive register. Everything
   below works on the one chosen here; the choice is remembered, so
   nothing that reads or writes has to carry it. */
void ata_select(int unit);           /* 0 = master, 1 = slave */
int ata_selected(void);

/* Disk capacity in sectors of the CHOSEN drive (IDENTIFY DEVICE,
   LBA28). 0 = there is no such drive, or it could not be read. */
uint32_t ata_sectors(void);

/* Read/write of a single 512 B sector (LBA28, PIO mode, polling).
   Returns 0 on success, -1 on error. */
int ata_read_sector(uint32_t lba, void *buf);
int ata_write_sector(uint32_t lba, const void *buf);

#endif
