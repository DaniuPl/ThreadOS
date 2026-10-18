#ifndef THREADOS_EFIRT_H
#define THREADOS_EFIRT_H

#include <stdint.h>

/* The firmware's runtime services, after the loader has gone
 * (kernel64/efirt.c) - for one thing only: putting an installed system
 * into the machine's boot menu. The 32-bit kernel has none of it.
 */

/* A GPT partition, the way a UEFI device path names it */
struct efirt_partition {
    uint32_t number;             /* its entry in the table, from 1 */
    uint64_t start, sectors;
    uint8_t guid[16];            /* the partition's own GUID */
};

/* 1 = the system was started by a UEFI loader that passed the firmware
   on, and its runtime services can be called */
int efirt_available(void);

/* A boot entry 'desc' for 'path' (\EFI\...) on the partition, made or
   rewritten (an entry with the same description is ours), first in
   BootOrder or last. 0 = done, its number in *number; -1 = not. */
int efirt_add_boot_entry(const char *desc, const struct efirt_partition *p,
                         const char *path, int first, unsigned *number);

#endif
