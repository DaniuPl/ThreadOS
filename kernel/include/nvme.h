#ifndef PROTONOS_NVME_H
#define PROTONOS_NVME_H

#include <stdint.h>

/* Sterownik NVMe (QEMU: -device nvme)
 *
 * Dysk podlaczony wprost do PCI Express, bez calego dziedzictwa ATA.
 * Nie ma tu rejestrow "wybierz dysk" ani ramek FIS - jest para
 * KOLEJEK w pamieci: my wkladamy polecenia do kolejki zgloszen,
 * kontroler odklada odpowiedzi do kolejki ukonczen. O tym, ze cos
 * doszlo, mowi dzwonek (rejestr doorbell), a o tym, ze cos wrocilo -
 * bit fazy w odpowiedzi.
 */

int nvme_init(void);
int nvme_present(void);
uint32_t nvme_sectors(void);
int nvme_read(uint32_t lba, uint32_t count, void *buf);
int nvme_write(uint32_t lba, uint32_t count, const void *buf);

#endif
