/* The initrd on a PC: the Multiboot module GRUB or threadboot handed over
 *
 * The PMM reserves the module's frames, so nobody overwrites it for us.
 * What has to be known first is that the module lies in memory the
 * kernel reaches at its own address - the identity-mapped part, which
 * only long mode's paging can say (kernel64/paging64.c); the image is
 * then kernel/initrd.c's to take apart.
 */

#include "../kernel/include/initrd.h"
#include "../kernel/include/printk.h"
#include "paging64.h"

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
}
