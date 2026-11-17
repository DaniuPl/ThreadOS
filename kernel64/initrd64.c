/* The initrd on a PC: the Multiboot module GRUB or threadboot handed over
 *
 * The PMM reserves the module's frames, so nobody overwrites it for us.
 * What has to be known first is that the module lies in memory the
 * kernel reaches at its own address - the identity-mapped part, which
 * only long mode's paging can say (kernel64/paging64.c); the image is
 * then kernel/initrd.c's to take apart.
 *
 * The initrd is the first module. The applications' image, when the
 * loader brought it (apps64.img, the web engine), is the module it
 * named "apps" (boot/efi/efiboot.c, boot/grub64.cfg).
 */

#include "../kernel/include/initrd.h"
#include "../kernel/include/mb2.h"
#include "../kernel/include/printk.h"
#include "paging64.h"

static void apps_init(void)
{
    uint32_t start, size;
    if (!mb2_module("apps", &start, &size) || !size)
        return;
    if ((uint64_t)start + size > paging64_identity_top()) {
        printk("[ !! ] apps64.img lies outside managed memory (%x)\n", start);
        return;
    }
    if (initrd_use_apps((const void *)(uintptr_t)start, size) < 0)
        printk("[ !! ] apps64.img is not an image of programs\n");
    else
        printk("[ OK ] Applications' image: %u KiB\n", size / 1024);
}

void initrd_init(const struct multiboot_info *mbi)
{
    if (!(mbi->flags & MULTIBOOT_FLAG_MODS) || mbi->mods_count == 0) {
        printk("[ !! ] initrd: the boot loader handed over no module\n");
        return;
    }

    const struct multiboot_mod *mod =
        (const struct multiboot_mod *)(uintptr_t)mbi->mods_addr;

    if (mod->end > paging64_identity_top()) {
        printk("[ !! ] initrd: the module is outside managed memory (%x)\n",
               mod->start);
        return;
    }
    if (initrd_use((const void *)(uintptr_t)mod->start,
                   mod->end - mod->start) < 0)
        printk("[ !! ] initrd: the module is not an initrd (its magic, or "
               "a listing that reaches outside it)\n");
    apps_init();
}
