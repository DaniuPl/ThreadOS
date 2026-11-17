#ifndef THREADOS_INITRD_H
#define THREADOS_INITRD_H

#include <stdint.h>
#include "multiboot.h"

/* An initrd in the TIRD format (scripts/mkinitrd.py):
   "TIRD" | u32 count | entries {name[16], u32 offset, u32 size} | data */

/* The Multiboot module (kernel64/initrd64.c) */
void initrd_init(const struct multiboot_info *mbi);
/* An image already in memory, however it came: checked - its magic,
   and every entry inside it - and then used. 0 = taken. */
int initrd_use(const void *image, uint32_t size);
/* The same for the applications' image (apps64.img), a second module
   in the same format whose programs initrd_find finds too */
int initrd_use_apps(const void *image, uint32_t size);
uint32_t initrd_file_count(void);
const void *initrd_find(const char *name, uint32_t *size);
void initrd_ls(void);   /* the 'ls' shell command */

/* The whole image, as it arrived - not a file inside it. The installer
   writes this onto the EFI partition byte for byte, because that is
   what the loader will read there. NULL = there is none. */
const void *initrd_image(uint32_t *size);
const void *initrd_apps_image(uint32_t *size);

#endif
