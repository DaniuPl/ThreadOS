#ifndef PROTONOS_NVME_H
#define PROTONOS_NVME_H

#include <stdint.h>

/* NVMe driver (QEMU: -device nvme)
 *
 * A disk plugged straight into PCI Express, without any of the ATA
 * legacy. There are no "select drive" registers here and no FIS
 * frames - there is a pair of QUEUES in memory: we put commands into
 * the submission queue, the controller puts answers into the
 * completion queue. That something has arrived is announced by a
 * doorbell register, and that something has come back - by a phase bit
 * in the answer.
 */

int nvme_init(void);
int nvme_present(void);
uint32_t nvme_sectors(void);
int nvme_read(uint32_t lba, uint32_t count, void *buf);
int nvme_write(uint32_t lba, uint32_t count, const void *buf);

#endif
