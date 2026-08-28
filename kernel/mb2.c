/* Multiboot 2 - przepisanie informacji od loadera
 *
 * Nowsza postac to ciag znacznikow: kazdy ma typ, dlugosc i tresc,
 * a kolejny lezy pod adresem wyrownanym do osmiu bajtow. Bierzemy
 * z nich to samo, co dawala starsza postac - linie polecen, moduly
 * i mape pamieci - i przepisujemy do struktury, ktorej reszta jadra
 * uzywa od poczatku.
 *
 * Jedna rzecz jest nowa i wazna: wskaznik na tablice ACPI. Pod BIOS-em
 * dalo sie go znalezc samemu, przeszukujac pamiec ponizej megabajta.
 * Pod UEFI tej pamieci nie ma - wskaznik podaje wylacznie loader.
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
    uint8_t type;                /* 1 = kolory wprost, 2 = tekst EGA */
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

const char *mb2_boot_kind(void)
{
    return kind;
}

uint32_t mb2_rsdp(void)
{
    return rsdp_addr;
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

    /* Pierwsze osiem bajtow to calkowity rozmiar i pole zerowe;
       znaczniki zaczynaja sie zaraz za nimi */
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
                /* Starsza postac liczy rozmiar wpisu OD POLA za nim,
                   wiec kolejny lezy pod adresem +size+4 */
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
            /* Bierzemy tryb z kolorami wprost i adresem miesczacym sie
               w 32 bitach. Glebi nie sprawdzamy tutaj - lepiej zapamietac
               to, co loader dal, i powiedziec potem wprost, czego nie
               umiemy obsluzyc, niz milczec, jakby bufora nie bylo wcale.
               Typ 2 znaczy, ze loader zostawil tryb tekstowy. */
            if (f->type == 1 && (f->addr >> 32) == 0) {
                framebuffer.addr = (uint32_t)f->addr;
                framebuffer.pitch = f->pitch;
                framebuffer.width = f->width;
                framebuffer.height = f->height;
                framebuffer.bpp = f->bpp;
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

        pos += (t->size + 7) & ~7u;      /* znaczniki co osiem bajtow */
    }

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
