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

/* Application packages (.tapp, scripts/mktapp.py): images in the same
   format whose first entry is a manifest saying what the package is.
   initrd_find finds their programs after the initrd's. */
#define PKG_MAX 16
struct package {
    char id[16];          /* a-z 0-9 '-'; installed as <id>.tapp */
    char name[32];        /* what people see */
    char name_pl[32];     /* the same in Polish, or "" */
    char exec[16];        /* the program that starts it */
    uint32_t version;
    int art;              /* the dock's picture, or -1 */
    int dock;             /* 1 = a tile of its own in the dock */
    const void *base;     /* the whole package, as it arrived */
    uint32_t size;
};
#define PKG_BAD         (-1)
#define PKG_NO_MANIFEST (-2)
#define PKG_OTHER_ARCH  (-3)
#define PKG_NO_EXEC     (-4)
#define PKG_TWICE       (-5)
#define PKG_FULL        (-6)
/* Checked - the image, then its manifest - and taken: 0, or PKG_* */
int package_use(const void *image, uint32_t size);
const char *package_why(int code);
int package_count(void);
const struct package *package_at(int i);

uint32_t initrd_file_count(void);
const void *initrd_find(const char *name, uint32_t *size);
void initrd_ls(void);   /* the 'ls' shell command */

/* The whole image, as it arrived - not a file inside it. The installer
   writes this onto the EFI partition byte for byte, because that is
   what the loader will read there. NULL = there is none. */
const void *initrd_image(uint32_t *size);

#endif
