/* The initrd - an image of user programs loaded by the bootloader
 *
 * The TIRD format is deliberately trivial: a header, a file listing,
 * the data. How the image arrives differs by machine - on a PC GRUB or
 * threadboot hands it over as a Multiboot module (kernel64/initrd64.c),
 * on ARM64 QEMU's fw_cfg does - and this file is what is the same: an
 * image in memory, taken apart. Its listing is the image's word, so
 * every entry is checked to lie inside it before anything is read.
 */

#include "include/initrd.h"
#include "include/string.h"
#include "include/printk.h"

struct pird_entry {
    char name[16];
    uint32_t offset;
    uint32_t size;
} __attribute__((packed));

static const uint8_t *base;
static uint32_t total_size;
static uint32_t file_count;
static const struct pird_entry *entries;

const void *initrd_image(uint32_t *size)
{
    if (size)
        *size = total_size;
    return base;
}

int initrd_use(const void *image, uint32_t size)
{
    const uint8_t *p = image;
    if (!p || size < 8 || p[0] != 'T' || p[1] != 'I' || p[2] != 'R' ||
        p[3] != 'D')
        return -1;
    uint32_t count = *(const uint32_t *)(p + 4);
    if (count > (size - 8) / sizeof(struct pird_entry))
        return -1;
    const struct pird_entry *e = (const struct pird_entry *)(p + 8);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t n = 0;
        while (n < sizeof(e[i].name) && e[i].name[n])
            n++;
        if (n == sizeof(e[i].name) || e[i].offset > size ||
            e[i].size > size - e[i].offset)
            return -1;                 /* a name with no end, or outside */
    }
    base = p;
    total_size = size;
    file_count = count;
    entries = e;
    return 0;
}

uint32_t initrd_file_count(void)
{
    return file_count;
}

const void *initrd_find(const char *name, uint32_t *size)
{
    for (uint32_t i = 0; i < file_count; i++) {
        if (!strcmp(entries[i].name, name)) {
            if (size)
                *size = entries[i].size;
            return base + entries[i].offset;
        }
    }
    return (void *)0;
}

void initrd_ls(void)
{
    if (!file_count) {
        printk("initrd: no files\n");
        return;
    }
    printk("initrd (%u B):\n", total_size);
    for (uint32_t i = 0; i < file_count; i++)
        printk("  %s  (%u B)\n", entries[i].name, entries[i].size);
}
