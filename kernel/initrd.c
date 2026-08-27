/* Initrd - obraz z programami użytkownika ładowany przez bootloader
 *
 * QEMU (-initrd) i GRUB (module) przekazują plik jako moduł Multiboot.
 * Format PIRD jest celowo trywialny: nagłówek, spis plików, dane.
 * PMM rezerwuje ramki modułu, więc nikt nam go nie nadpisze.
 */

#include "include/initrd.h"
#include "include/string.h"
#include "include/printk.h"
#include "include/paging.h"

struct pird_entry {
    char name[16];
    uint32_t offset;
    uint32_t size;
} __attribute__((packed));

static const uint8_t *base;
static uint32_t total_size;
static uint32_t file_count;
static const struct pird_entry *entries;

void initrd_init(const struct multiboot_info *mbi)
{
    if (!(mbi->flags & MULTIBOOT_FLAG_MODS) || mbi->mods_count == 0) {
        printk("[ !! ] initrd: bootloader nie przekazal modulu\n");
        return;
    }

    const struct multiboot_mod *mod =
        (const struct multiboot_mod *)mbi->mods_addr;

    if (mod->end > paging_identity_limit()) {
        printk("[ !! ] initrd: modul poza zarzadzana pamiecia (%x)\n",
               mod->start);
        return;
    }

    const uint8_t *p = (const uint8_t *)mod->start;
    if (p[0] != 'P' || p[1] != 'I' || p[2] != 'R' || p[3] != 'D') {
        printk("[ !! ] initrd: zly magic modulu\n");
        return;
    }

    base = p;
    total_size = mod->end - mod->start;
    file_count = *(const uint32_t *)(p + 4);
    entries = (const struct pird_entry *)(p + 8);
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
        printk("initrd: brak plikow\n");
        return;
    }
    printk("initrd (%u B):\n", total_size);
    for (uint32_t i = 0; i < file_count; i++)
        printk("  %s  (%u B)\n", entries[i].name, entries[i].size);
}
