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

/* The images programs are found in: the initrd, and the application
   packages the loader brought beside it (.tapp, scripts/mktapp.py -
   Weft, the web engine's browser, is one: too large to be part of every
   boot's initrd for those who never start it). A name is looked for in
   the initrd first, so a package cannot stand in for a program of the
   system's own. */
struct image {
    const uint8_t *base;
    uint32_t size, count;
    const struct pird_entry *entries;
};
static struct image initrd;
static struct image pkg_img[PKG_MAX];
static struct package pkgs[PKG_MAX];
static int npkgs;

#if defined(__aarch64__)
#define THIS_ARCH "aarch64"
#else
#define THIS_ARCH "x86_64"
#endif

const void *initrd_image(uint32_t *size)
{
    if (size)
        *size = initrd.size;
    return initrd.base;
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

static const void *find_in(const struct image *img, const char *name,
                           uint32_t *size);

/* One "key value" line of a manifest into a field: the value cut at
   the field's size, and a key that is not this one leaving it as it
   was */
static int field(const char *line, int len, const char *key, char *out,
                 int room)
{
    int k = 0;
    while (key[k] && k < len && line[k] == key[k])
        k++;
    if (key[k] || k >= len || line[k] != ' ')
        return 0;
    int n = 0;
    for (k++; k < len && n < room - 1; k++)
        out[n++] = line[k];
    out[n] = 0;
    return 1;
}

static uint32_t number(const char *s, int *ok)
{
    uint32_t v = 0;
    *ok = *s != 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || v > 100000000u) {
            *ok = 0;
            return 0;
        }
        v = v * 10 + (uint32_t)(*s - '0');
    }
    return v;
}

int package_use(const void *image, uint32_t size)
{
    if (npkgs == PKG_MAX)
        return PKG_FULL;
    struct image *img = &pkg_img[npkgs];
    if (take(img, image, size) < 0)
        return PKG_BAD;
    /* The manifest: the first entry, text, its first line the magic */
    if (!img->count || strcmp(img->entries[0].name, "manifest"))
        return PKG_NO_MANIFEST;
    const char *m = (const char *)img->base + img->entries[0].offset;
    uint32_t mlen = img->entries[0].size;
    static const char magic[] = "ThreadOS app\n";
    if (mlen < sizeof(magic) - 1 || memcmp(m, magic, sizeof(magic) - 1))
        return PKG_NO_MANIFEST;

    struct package p;
    memset(&p, 0, sizeof(p));
    p.art = -1;
    char arch[16] = "", version[12] = "", art[8] = "", dock[8] = "";
    for (uint32_t at = sizeof(magic) - 1; at < mlen; ) {
        uint32_t end = at;
        while (end < mlen && m[end] != '\n')
            end++;
        const char *line = m + at;
        int len = (int)(end - at);
        field(line, len, "id", p.id, sizeof(p.id));
        field(line, len, "name", p.name, sizeof(p.name));
        field(line, len, "name.pl", p.name_pl, sizeof(p.name_pl));
        field(line, len, "version", version, sizeof(version));
        field(line, len, "arch", arch, sizeof(arch));
        field(line, len, "exec", p.exec, sizeof(p.exec));
        field(line, len, "art", art, sizeof(art));
        field(line, len, "dock", dock, sizeof(dock));
        at = end + 1;
    }
    int ok = p.id[0] != 0;
    for (const char *c = p.id; *c; c++)
        ok &= (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') ||
              *c == '-';
    int vok, aok = 1;
    p.version = number(version, &vok);
    if (art[0])
        p.art = (int)number(art, &aok);
    if (!ok || !vok || !aok || !p.name[0] || !p.exec[0])
        return PKG_NO_MANIFEST;
    /* Programs built for another processor are not a package for this
       machine: refused here rather than as an exec that cannot work */
    if (strcmp(arch, THIS_ARCH))
        return PKG_OTHER_ARCH;
    if (!find_in(img, p.exec, (uint32_t *)0))
        return PKG_NO_EXEC;
    for (int i = 0; i < npkgs; i++)
        if (!strcmp(pkgs[i].id, p.id))
            return PKG_TWICE;
    p.dock = !strcmp(dock, "yes");
    p.base = image;
    p.size = size;
    pkgs[npkgs++] = p;
    return 0;
}

const char *package_why(int code)
{
    switch (code) {
    case PKG_BAD:         return "not an image, or a listing outside it";
    case PKG_NO_MANIFEST: return "no manifest that says what it is";
    case PKG_OTHER_ARCH:  return "its programs are for another processor";
    case PKG_NO_EXEC:     return "the program it names is not in it";
    case PKG_TWICE:       return "a package of that name is there already";
    case PKG_FULL:        return "there are as many packages as there can be";
    default:              return "?";
    }
}

int package_count(void)
{
    return npkgs;
}

const struct package *package_at(int i)
{
    return i >= 0 && i < npkgs ? &pkgs[i] : (const struct package *)0;
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
    for (int i = 0; !f && i < npkgs; i++)
        f = find_in(&pkg_img[i], name, size);
    return f;
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
    for (int i = 0; i < npkgs; i++) {
        printk("package %s, version %u - ", pkgs[i].id, pkgs[i].version);
        list(pkgs[i].name, &pkg_img[i]);
    }
}
