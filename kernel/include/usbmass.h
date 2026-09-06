#ifndef PROTONOS_USBMASS_H
#define PROTONOS_USBMASS_H

#include <stdint.h>

/* A USB disk - a pendrive, a card reader, an external drive
 *
 * One class, one transport: Bulk-Only Transport, which every stick
 * made in the last twenty years speaks. Inside it travels SCSI - the
 * same commands a real disk answers - so the whole driver is: wrap a
 * command, send it, read the data, read the verdict.
 *
 * It registers itself in blk.c like any other disk, so the
 * filesystem, the partition table, FAT and the installer see nothing
 * new. That is the point: a pendrive is a disk.
 */

/* Looks for a USB disk on the bus and prepares it. 1 = there is one. */
int usbmass_init(void);

/* How many were found, and their capacity in 512-byte sectors */
int usbmass_count(void);
uint32_t usbmass_sectors(int unit);

/* The name to print: the maker and the model from INQUIRY */
const char *usbmass_name(int unit);

/* Is it still in the socket? A disk that has been pulled out keeps its
   number and its name - and answers no to this. */
int usbmass_present(int unit);

/* Reading and writing whole sectors. 0 = done, -1 = error. */
int usbmass_read(int unit, uint32_t lba, uint32_t count, void *buf);
int usbmass_write(int unit, uint32_t lba, uint32_t count, const void *buf);

#endif
