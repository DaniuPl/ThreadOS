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

/* The images programs are found in: the initrd, and - when the loader
   brought one - the applications' image beside it (apps64.img: the
   web engine, too large to be part of every boot's initrd for those
   who never start it, and kept apart so the initrd is the size it
   was). A name is looked for in the initrd first. */
struct image {
    const uint8_t *base;
    uint32_t size, count;
    const struct pird_entry *entries;
};
static struct image initrd, apps;

const void *initrd_image(uint32_t *size)
{
    if (size)
        *size = initrd.size;
    return initrd.base;
}

const void *initrd_apps_image(uint32_t *size)
{
    if (size)
        *size = apps.size;
    return apps.base;
}

static int take(struct image *img, const void *image, uint32_t size)
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
    img->base = p;
    img->size = size;
    img->count = count;
    img->entries = e;
    return 0;
}

int initrd_use(const void *image, uint32_t size)
{
    return take(&initrd, image, size);
}

int initrd_use_apps(const void *image, uint32_t size)
{
    return take(&apps, image, size);
}

uint32_t initrd_file_count(void)
{
    return initrd.count;
}

static const void *find_in(const struct image *img, const char *name,
                           uint32_t *size)
{
    for (uint32_t i = 0; i < img->count; i++) {
        if (!strcmp(img->entries[i].name, name)) {
            if (size)
                *size = img->entries[i].size;
            return img->base + img->entries[i].offset;
        }
    }
    return (void *)0;
}

const void *initrd_find(const char *name, uint32_t *size)
{
    const void *f = find_in(&initrd, name, size);
    return f ? f : find_in(&apps, name, size);
}

static void list(const char *what, const struct image *img)
{
    printk("%s (%u B):\n", what, img->size);
    for (uint32_t i = 0; i < img->count; i++)
        printk("  %s  (%u B)\n", img->entries[i].name, img->entries[i].size);
}

void initrd_ls(void)
{
    if (!initrd.count) {
        printk("initrd: no files\n");
        return;
    }
    list("initrd", &initrd);
    if (apps.count)
        list("apps64.img", &apps);
}
