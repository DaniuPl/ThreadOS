#ifndef PROTONOS_AHCI_H
#define PROTONOS_AHCI_H

#include <stdint.h>

/* AHCI (SATA) driver
 *
 * The successor to ATA PIO. The difference is fundamental: with PIO
 * the processor copies every word through an I/O port, with AHCI the
 * controller reaches into memory itself (DMA) following a table of
 * descriptors we hand it. Modern boards no longer have a compatibility
 * mode - without this driver a disk in such a machine is simply
 * invisible.
 */

/* Looks for an AHCI controller on the PCI bus and prepares the first
   port with a disk. 1 = there is a disk ready to work. */
int ahci_init(void);

/* Disk capacity in 512 B sectors (0 = unknown) */
uint32_t ahci_sectors(void);

/* Read/write 'count' consecutive sectors. The buffer must lie in
   physically contiguous memory - the controller reads it itself,
   without any help from paging. 0 = OK, -1 = error. */
int ahci_read(uint32_t lba, uint32_t count, void *buf);
int ahci_write(uint32_t lba, uint32_t count, const void *buf);

#endif
