#ifndef PROTONOS_ATA_H
#define PROTONOS_ATA_H

#include <stdint.h>

#define ATA_SECTOR_SIZE 512

/* Czy na pierwszej magistrali (master) jest dysk */
int ata_present(void);

/* Odczyt/zapis pojedynczego sektora 512 B (LBA28, tryb PIO, polling).
   Zwraca 0 przy sukcesie, -1 przy bledzie. */
int ata_read_sector(uint32_t lba, void *buf);
int ata_write_sector(uint32_t lba, const void *buf);

#endif
