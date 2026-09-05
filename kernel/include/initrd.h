#ifndef PROTONOS_INITRD_H
#define PROTONOS_INITRD_H

#include <stdint.h>
#include "multiboot.h"

/* An initrd in the PIRD format (scripts/mkinitrd.py):
   "PIRD" | u32 count | entries {name[16], u32 offset, u32 size} | data */

void initrd_init(const struct multiboot_info *mbi);
uint32_t initrd_file_count(void);
const void *initrd_find(const char *name, uint32_t *size);
void initrd_ls(void);   /* the 'ls' shell command */

/* The whole image, as it arrived - not a file inside it. The installer
   writes this onto the EFI partition byte for byte, because that is
   what the loader will read there. NULL = there is none. */
const void *initrd_image(uint32_t *size);

#endif
