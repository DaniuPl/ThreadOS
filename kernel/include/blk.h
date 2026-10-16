#ifndef THREADOS_BLK_H
#define THREADOS_BLK_H

#include <stdint.h>

/* The block device layer
 *
 * The filesystem and the swapper do not need to know whether the disk
 * sits behind an AHCI controller or behind old ATA in PIO mode. What
 * they do need is that there is ONE disk they are reading and writing
 * - so this layer keeps a list of the disks the machine has and one of
 * them in use, and everything above it works on that one.
 *
 * The list is in order from newer to older: NVMe, then every AHCI
 * port with a disk on it, then the two drives of the old ATA bus. The
 * first one is in use at startup, which makes a machine with one disk
 * behave exactly as it did when this layer knew of only one.
 */

#define BLK_MAX 8

/* Finds the disks and takes the first one. 1 = there is at least one. */
int blk_init(void);
int blk_present(void);

/* How many there are, which one is in use, and how to change that.
   Changing it changes what the filesystem, the partition table and
   FAT are looking at - so whoever changes it has to mean it. */
int blk_count(void);
int blk_current(void);
int blk_use(int i);                  /* 0 = done, -1 = there is no such one */

/* Name of the driver behind the disk in use, and behind any of them */
const char *blk_name(void);
const char *blk_name_of(int i);

/* Capacity in 512 B sectors (0 = unknown) */
uint32_t blk_sectors(void);
uint32_t blk_sectors_of(int i);

/* A disk plugged in while the machine runs, and one pulled out. The
   numbers on the list never move: a disk that leaves keeps its place
   and is marked gone, because somebody is reading that list and about
   to type a number from it. */
int blk_usb_added(int unit);
void blk_usb_gone(int unit);

/* Read/write 'count' consecutive sectors. The buffer must lie in
   physically contiguous memory - with AHCI the controller reaches for
   it itself. 0 = OK, -1 = error. */
int blk_read(uint32_t lba, uint32_t count, void *buf);
int blk_write(uint32_t lba, uint32_t count, const void *buf);

#endif
