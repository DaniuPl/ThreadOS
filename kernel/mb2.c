/* Multiboot 2 - copying over the loader's information
 *
 * The newer form is a sequence of tags: each has a type, a length and
 * a body, and the next one lies at an address aligned to eight bytes.
 * We take from them the same things the older form gave - the command
 * line, the modules and the memory map - and copy them into the
 * structure the rest of the kernel has used from the start.
 *
 * One thing is new and important: the pointer to the ACPI tables.
 * Under a BIOS it could be found by ourselves, by searching memory
 * below one megabyte. Under UEFI that memory does not exist - the
 * pointer comes from the loader and nowhere else.
 */

#include <stdint.h>
#include "include/mb2.h"
#include "include/string.h"

#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002

#define TAG_END      0
#define TAG_CMDLINE  1
#define TAG_MODULE   3
#define TAG_MMAP     6
#define TAG_FRAMEBUF 8
#define TAG_RSDP_OLD 14
#define TAG_RSDP_NEW 15
#define TAG_EFI64    12          /* the UEFI system table's address */
#define TAG_EFI_MMAP 17          /* the firmware's own memory map */
/* Our own, from boot/efi/efiboot.c: the panel's size in millimetres,
   out of its EDID - the one thing about the screen a framebuffer tag
   has no room for, and the one the desktop's scale wants */
#define TAG_PANEL    0x4C4E4150u   /* "PANL" */

struct tag {
    uint32_t type;
    uint32_t size;
} __attribute__((packed));

struct tag_mmap {
    struct tag tag;
    uint32_t entry_size;
    uint32_t entry_version;
} __attribute__((packed));

struct mmap_entry2 {
    uint64_t addr;
    uint64_t len;
    uint32_t type;
    uint32_t reserved;
} __attribute__((packed));

struct tag_framebuffer {
    struct tag tag;
    uint64_t addr;
    uint32_t pitch;
    uint32_t width, height;
    uint8_t bpp;
    uint8_t type;                /* 1 = direct colours, 2 = EGA text */
    uint16_t reserved;
} __attribute__((packed));

struct tag_module {
    struct tag tag;
    uint32_t mod_start;
    uint32_t mod_end;
    char string[1];
} __attribute__((packed));

#define MMAP_MAX 32
#define MODS_MAX 4

static struct multiboot_info info;
static struct multiboot_mmap_entry mmap[MMAP_MAX];
static struct multiboot_mod mods[MODS_MAX];
static uint32_t rsdp_addr;
static struct mb2_framebuffer framebuffer;
static const char *kind = "?";

/* What a UEFI loader leaves of the firmware: the system table, and its
   memory map as the firmware wrote it - the regions marked for runtime
   are what has to stay reachable to call it (kernel64/efirt.c). Copied,
   so that nothing depends on the loader's memory staying put. */
static uint64_t efi_systab;
static uint8_t efi_map[16384];
static uint32_t efi_map_size, efi_desc_size;

int mb2_efi(uint64_t *systab, const uint8_t **map, uint32_t *map_size,
            uint32_t *desc_size)
{
    if (!efi_systab || !efi_map_size || !efi_desc_size)
        return 0;
    *systab = efi_systab;
    *map = efi_map;
    *map_size = efi_map_size;
    *desc_size = efi_desc_size;
    return 1;
}

const char *mb2_boot_kind(void)
{
    return kind;
}

uint32_t mb2_rsdp(void)
{
    return rsdp_addr;
}

/* Modules beyond the initrd: the loader's own name for each is the
   only thing that identifies it. */
static uint32_t nmods_kept;

int mb2_module(const char *name, uint32_t *start, uint32_t *size)
{
    if (!name)
        return 0;
    for (uint32_t i = 0; i < nmods_kept; i++) {
        const char *s = (const char *)(uintptr_t)mods[i].string;
        if (!s)
            continue;
        int k = 0;
        while (name[k] && s[k] == name[k])
            k++;
        if (name[k] || s[k])
            continue;
        if (start)
            *start = mods[i].start;
        if (size)
            *size = mods[i].end - mods[i].start;
        return 1;
    }
    return 0;
}

const struct mb2_framebuffer *mb2_framebuffer(void)
{
    return framebuffer.addr ? &framebuffer : (const struct mb2_framebuffer *)0;
}

const struct multiboot_info *mb2_info(uint32_t magic, uint32_t addr)
{
    if (magic == MULTIBOOT_BOOTLOADER_MAGIC) {
        kind = "Multiboot 1";
        return (const struct multiboot_info *)(uintptr_t)addr;
    }
    if (magic != MULTIBOOT2_BOOTLOADER_MAGIC)
        return (const struct multiboot_info *)0;

    kind = "Multiboot 2";
    memset(&info, 0, sizeof(info));

    uint32_t nmods = 0, nmmap = 0;

    /* The first eight bytes are the total size and a zero field;
       the tags begin right after them */
    uint32_t total = *(const uint32_t *)(uintptr_t)addr;
    uint32_t pos = addr + 8;
    uint32_t end = addr + total;

    while (pos + sizeof(struct tag) <= end) {
        const struct tag *t = (const struct tag *)(uintptr_t)pos;
        if (t->type == TAG_END || t->size < sizeof(struct tag))
            break;

        switch (t->type) {
        case TAG_CMDLINE:
            info.cmdline = pos + sizeof(struct tag);
            info.flags |= MULTIBOOT_FLAG_CMDLINE;
            break;

        case TAG_MODULE: {
            const struct tag_module *m = (const struct tag_module *)t;
            if (nmods < MODS_MAX) {
                mods[nmods].start = m->mod_start;
                mods[nmods].end = m->mod_end;
                mods[nmods].string = (uint32_t)(uintptr_t)m->string;
                mods[nmods].reserved = 0;
                nmods++;
            }
            break;
        }

        case TAG_MMAP: {
            const struct tag_mmap *mm = (const struct tag_mmap *)t;
            uint32_t esz = mm->entry_size;
            if (esz < sizeof(struct mmap_entry2))
                break;
            for (uint32_t o = sizeof(*mm); o + esz <= t->size; o += esz) {
                if (nmmap >= MMAP_MAX)
                    break;
                const struct mmap_entry2 *e =
                    (const struct mmap_entry2 *)(uintptr_t)(pos + o);
                /* The older form counts an entry's size FROM THE FIELD after
                   it, so the next one lies at +size+4 */
                mmap[nmmap].size = sizeof(struct multiboot_mmap_entry) - 4;
                mmap[nmmap].addr = e->addr;
                mmap[nmmap].len = e->len;
                mmap[nmmap].type = e->type;
                nmmap++;
            }
            break;
        }

        case TAG_FRAMEBUF: {
            const struct tag_framebuffer *f =
                (const struct tag_framebuffer *)t;
            /* We take a mode with direct colours and an address that fits
               in 32 bits. We do not check the depth here - better to
               remember what the loader gave and say plainly later what
               we cannot handle than to stay silent as if there were no
               buffer at all. Type 2 means the loader left text mode. */
            if (f->type == 1 && (f->addr >> 32) == 0) {
                framebuffer.addr = (uint32_t)f->addr;
                framebuffer.pitch = f->pitch;
                framebuffer.width = f->width;
                framebuffer.height = f->height;
                framebuffer.bpp = f->bpp;
            }
            break;
        }

        case TAG_PANEL: {
            const uint32_t *mm = (const uint32_t *)(pos + sizeof(struct tag));
            framebuffer.mm_w = mm[0];
            framebuffer.mm_h = mm[1];
            break;
        }

        case TAG_EFI64:
            efi_systab = *(const uint64_t *)(uintptr_t)(pos + sizeof(struct tag));
            break;

        case TAG_EFI_MMAP: {
            const uint32_t *h = (const uint32_t *)(uintptr_t)(pos + sizeof(struct tag));
            uint32_t bytes = t->size - sizeof(struct tag) - 8;
            if (h[0] >= 40 && bytes <= sizeof(efi_map)) {
                memcpy(efi_map, h + 2, bytes);
                efi_map_size = bytes;
                efi_desc_size = h[0];
            }
            break;
        }

        case TAG_RSDP_OLD:
        case TAG_RSDP_NEW:
            if (!rsdp_addr)
                rsdp_addr = pos + sizeof(struct tag);
            break;

        default:
            break;
        }

        pos += (t->size + 7) & ~7u;      /* tags every eight bytes */
    }

    nmods_kept = nmods;
    if (nmods) {
        info.mods_count = nmods;
        info.mods_addr = (uint32_t)(uintptr_t)mods;
        info.flags |= MULTIBOOT_FLAG_MODS;
    }
    if (nmmap) {
        info.mmap_addr = (uint32_t)(uintptr_t)mmap;
        info.mmap_length = nmmap * sizeof(struct multiboot_mmap_entry);
        info.flags |= MULTIBOOT_FLAG_MMAP;
    }
    return &info;
}
