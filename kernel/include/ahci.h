#ifndef THREADOS_AHCI_H
#define THREADOS_AHCI_H

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

/* Looks for an AHCI controller on the PCI bus and prepares EVERY port
   with a disk on it. Returns how many were found (0 = none), and
   leaves the first of them chosen. */
int ahci_init(void);

/* How many disks the controller has, and which of them the calls
   below work on. A machine can have several, and which one a person
   means is a question only they can answer. */
int ahci_count(void);
int ahci_select(int i);              /* 0 = done, -1 = there is no such one */

/* Capacity of the CHOSEN disk in 512 B sectors (0 = unknown) */
uint32_t ahci_sectors(void);

/* Read/write 'count' consecutive sectors. The buffer must lie in
   physically contiguous memory - the controller reads it itself,
   without any help from paging. 0 = OK, -1 = error. */
int ahci_read(uint32_t lba, uint32_t count, void *buf);
int ahci_write(uint32_t lba, uint32_t count, const void *buf);

#endif
