/* The initrd on a PC: the Multiboot module GRUB or threadboot handed over
 *
 * The PMM reserves the module's frames, so nobody overwrites it for us.
 * What has to be known first is that the module lies in memory the
 * kernel reaches at its own address - the identity-mapped part, which
 * only long mode's paging can say (kernel64/paging64.c); the image is
 * then kernel/initrd.c's to take apart.
 *
 * The initrd is the first module. The application packages the loader
 * found (.tapp: Weft, the web engine's browser, is one) are the modules
 * it named "tapp" (boot/efi/efiboot.c, boot/grub64.cfg).
 */

#include "../kernel/include/initrd.h"
#include "../kernel/include/mb2.h"
#include "../kernel/include/printk.h"
#include "paging64.h"

/* The application packages: every module the loader named "tapp".
   One that is not a package, or not one for this machine, is said and
   passed over - a system with a broken package is still a system. */
static void packages_init(void)
{
    uint32_t start, size;
    for (int n = 0; mb2_module_nth("tapp", n, &start, &size); n++) {
        if (!size)
            continue;
        if ((uint64_t)start + size > paging64_identity_top()) {
            printk("[ !! ] a package lies outside managed memory (%x)\n",
                   start);
            continue;
        }
        int rc = package_use((const void *)(uintptr_t)start, size);
        if (rc < 0) {
            printk("[ !! ] a package passed over: %s\n", package_why(rc));
            continue;
        }
        const struct package *p = package_at(package_count() - 1);
        printk("[ OK ] Package %s %u: %s, %u KiB\n", p->id, p->version,
               p->name, size / 1024);
    }
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
    packages_init();
}
