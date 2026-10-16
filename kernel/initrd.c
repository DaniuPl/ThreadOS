/* The initrd - an image of user programs loaded by the bootloader
 *
 * QEMU (-initrd) and GRUB (module) pass the file as a Multiboot
 * module. The TIRD format is deliberately trivial: a header, a file
 * listing, the data. The PMM reserves the module's frames, so nobody
 * overwrites it for us.
 */

#include "include/initrd.h"
#include "include/string.h"
#include "include/printk.h"

/* How far the identity-described memory reaches is known to paging -
   and this is the only file in this module that depends on the
   processor mode, because in long mode the page tables are built by
   completely different code. */
#ifdef __x86_64__
#include "../kernel64/paging64.h"
#define IDENTITY_TOP() paging64_identity_top()
#else
#include "include/paging.h"
#define IDENTITY_TOP() paging_identity_limit()
#endif

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

void initrd_init(const struct multiboot_info *mbi)
{
    if (!(mbi->flags & MULTIBOOT_FLAG_MODS) || mbi->mods_count == 0) {
        printk("[ !! ] initrd: the boot loader handed over no module\n");
        return;
    }

    const struct multiboot_mod *mod =
        (const struct multiboot_mod *)(uintptr_t)mbi->mods_addr;

    if (mod->end > IDENTITY_TOP()) {
        printk("[ !! ] initrd: the module is outside managed memory (%x)\n",
               mod->start);
        return;
    }

    const uint8_t *p = (const uint8_t *)(uintptr_t)mod->start;
    if (p[0] != 'T' || p[1] != 'I' || p[2] != 'R' || p[3] != 'D') {
        printk("[ !! ] initrd: the module's magic is wrong\n");
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
        printk("initrd: no files\n");
        return;
    }
    printk("initrd (%u B):\n", total_size);
    for (uint32_t i = 0; i < file_count; i++)
        printk("  %s  (%u B)\n", entries[i].name, entries[i].size);
}
