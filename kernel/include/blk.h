#ifndef PROTONOS_BLK_H
#define PROTONOS_BLK_H

#include <stdint.h>

/* The block device layer
 *
 * The filesystem and the swapper do not need to know whether the disk
 * sits behind an AHCI controller or behind old ATA in PIO mode. The
 * choice is made once, at startup: AHCI has priority (it is faster and
 * it is the only thing visible on newer boards), ATA remains as a
 * fallback.
 */

/* Detects a disk and picks a driver. 1 = there is a disk. */
int blk_init(void);
int blk_present(void);

/* Name of the chosen driver (for the startup message) */
const char *blk_name(void);

/* Capacity in 512 B sectors (0 = unknown) */
uint32_t blk_sectors(void);

/* Read/write 'count' consecutive sectors. The buffer must lie in
   physically contiguous memory - with AHCI the controller reaches for
   it itself. 0 = OK, -1 = error. */
int blk_read(uint32_t lba, uint32_t count, void *buf);
int blk_write(uint32_t lba, uint32_t count, const void *buf);

#endif
