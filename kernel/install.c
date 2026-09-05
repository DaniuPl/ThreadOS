/* Installing ProtonOS onto the machine's own disk
 *
 * On a UEFI machine installing is smaller than the word suggests:
 * three files written onto the EFI partition that is already there.
 * The firmware looks for a loader on that partition and starts it -
 * that is the whole mechanism. So nothing here creates a partition,
 * nothing touches the table, and nothing removes or moves a single
 * file belonging to the system that already lives on this disk.
 *
 * Where the three files come from is the part worth explaining. The
 * kernel running at this moment is loaded segments, not the file it
 * came from, and the file is what has to be written. So the loader
 * hands all three over as Multiboot 2 modules: the initrd as it always
 * did, and the kernel image and the loader itself by name. A system
 * started some other way - QEMU with -kernel, or GRUB from a disc
 * that was not made for this - has nothing to install and says so
 * rather than writing something approximate.
 *
 * Two paths are written. \EFI\ProtonOS is ours and is always written.
 * \EFI\BOOT\BOOTX64.EFI - the name firmware falls back to when nothing
 * else is arranged - is written ONLY IF IT IS NOT THERE. On a machine
 * that has one, it belongs to whoever boots that machine today, and
 * taking it would mean installing over them rather than beside them.
 * Then the firmware's own boot menu is the way in, and saying that is
 * better than a machine that quietly stops booting what it used to.
 */

#include "include/install.h"
#include "include/fat.h"
#include "include/part.h"
#include "include/blk.h"
#include "include/mb2.h"
#include "include/initrd.h"
#include "include/printk.h"
#include "include/string.h"

#ifdef __x86_64__
#include "../kernel64/paging64.h"
#define IDENTITY_TOP() paging64_identity_top()
#else
#include "include/paging.h"
#define IDENTITY_TOP() paging_identity_limit()
#endif

#define DIR_OURS     "/EFI/ProtonOS"
#define PATH_LOADER  "/EFI/ProtonOS/BOOTX64.EFI"
#define PATH_KERNEL  "/EFI/ProtonOS/protonos64.bin"
#define PATH_INITRD  "/EFI/ProtonOS/initrd64.img"
#define DIR_FALLBACK "/EFI/BOOT"
#define PATH_FALLBACK "/EFI/BOOT/BOOTX64.EFI"

static struct {
    int ready;
    int esp_index;
    uint32_t esp_lba;
    const uint8_t *loader;
    uint32_t loader_size;
    const uint8_t *kernel;
    uint32_t kernel_size;
    const uint8_t *initrd;
    uint32_t initrd_size;
    int fallback_free;
} plan;

/* A module the loader named, checked against the memory this kernel
   can actually reach. A module above that line is not a module we can
   read, and reading it anyway is a page fault in the middle of writing
   to somebody's disk. */
static const uint8_t *module_at(const char *name, uint32_t *size)
{
    uint32_t start = 0, len = 0;
    if (!mb2_module(name, &start, &len) || !len)
        return 0;
    if ((uint64_t)start + len > (uint64_t)IDENTITY_TOP())
        return 0;
    *size = len;
    return (const uint8_t *)(uintptr_t)start;
}

int install_plan(void)
{
    struct partition pt;
    struct fat_entry e;

    memset(&plan, 0, sizeof(plan));

    if (!blk_present()) {
        printk("instaluj: nie ma dysku\n");
        return 0;
    }

    /* What we would write. If the machine was not started by a loader
       that hands these over, there is nothing to install and no
       reason to look at the disk at all. */
    plan.loader = module_at("loader", &plan.loader_size);
    plan.kernel = module_at("jadro", &plan.kernel_size);
    plan.initrd = initrd_image(&plan.initrd_size);
    if (!plan.loader || !plan.kernel || !plan.initrd_size) {
        printk("instaluj: ten system nie ma z czego sie zainstalowac.\n");
        printk("          Program rozruchowy musi podac obraz jadra i "
               "siebie samego\n");
        printk("          jako moduly - robi to protoboot (rozruch przez "
               "UEFI z pendrive'a).\n");
        printk("          Mam: loader %s, jadro %s, initrd %s\n",
               plan.loader ? "tak" : "NIE", plan.kernel ? "tak" : "NIE",
               plan.initrd_size ? "tak" : "NIE");
        return 0;
    }

    int np = part_scan();
    if (np <= 0) {
        printk("instaluj: na dysku nie ma tablicy partycji, wiec nie ma "
               "tez partycji EFI.\n");
        printk("          Ten dysk nie jest dyskiem, z ktorego uruchamia "
               "sie maszyne z UEFI.\n");
        return 0;
    }
    plan.esp_index = 0;
    for (int i = 0; i < np; i++) {
        if (!part_get(i, &pt))
            break;
        if (!strcmp(pt.kind, "EFI (ESP)")) {
            plan.esp_index = i + 1;
            plan.esp_lba = (uint32_t)pt.start;
            break;
        }
    }
    if (!plan.esp_index) {
        printk("instaluj: nie znalazlem partycji EFI (%u partycji na "
               "dysku).\n", (uint32_t)np);
        return 0;
    }
    if (!fat_mount(plan.esp_lba)) {
        printk("instaluj: partycja %u nie jest FAT-em - a partycja EFI "
               "musi nim byc.\n", (uint32_t)plan.esp_index);
        return 0;
    }

    plan.fallback_free = !fat_stat(PATH_FALLBACK, &e);

    printk("Dysk:     %s, %u MiB\n", blk_name(), blk_sectors() / 2048);
    printk("Partycja EFI: numer %u, sektor %u, %s%s%s, wolne %u KiB\n",
           (uint32_t)plan.esp_index, plan.esp_lba, fat_kind(),
           fat_label()[0] ? ", " : "", fat_label(), fat_free_kib());
    printk("\nZapisze:\n");
    printk("  %s  %u B\n", PATH_LOADER, plan.loader_size);
    printk("  %s  %u B\n", PATH_KERNEL, plan.kernel_size);
    printk("  %s  %u B\n", PATH_INITRD, plan.initrd_size);
    if (plan.fallback_free)
        printk("  %s  %u B  (nie ma go tam - maszyna wystartuje sama)\n",
               PATH_FALLBACK, plan.loader_size);
    else
        printk("  %s JEST JUZ ZAJETY - nie ruszam go.\n", PATH_FALLBACK);

    uint32_t need = (plan.loader_size + plan.kernel_size + plan.initrd_size
                     + (plan.fallback_free ? plan.loader_size : 0)) / 1024 + 4;
    if (fat_free_kib() < need) {
        printk("\ninstaluj: na partycji EFI jest %u KiB wolnego, a trzeba "
               "okolo %u KiB.\n", fat_free_kib(), need);
        return 0;
    }

    printk("\nNIE ruszam: tablicy partycji, innych partycji ani zadnego "
           "pliku,\nktory juz jest na partycji EFI.\n");
    if (!plan.fallback_free)
        printk("Zeby uruchomic ProtonOS-a, trzeba bedzie dodac wpis w "
               "menu rozruchowym\nfirmware'u i wskazac %s.\n", PATH_LOADER);

    plan.ready = 1;
    return 1;
}

int install_do(void)
{
    if (!plan.ready) {
        printk("instaluj: nie ma czego zapisac (najpierw plan)\n");
        return -1;
    }
    if (fat_mkdir(DIR_OURS) < 0) {
        printk("instaluj: nie moge zalozyc %s\n", DIR_OURS);
        return -1;
    }
    if (fat_write_file(PATH_LOADER, plan.loader, plan.loader_size) < 0) {
        printk("instaluj: nie moge zapisac %s\n", PATH_LOADER);
        return -1;
    }
    printk("  %s zapisany\n", PATH_LOADER);
    if (fat_write_file(PATH_KERNEL, plan.kernel, plan.kernel_size) < 0) {
        printk("instaluj: nie moge zapisac %s\n", PATH_KERNEL);
        return -1;
    }
    printk("  %s zapisany\n", PATH_KERNEL);
    if (fat_write_file(PATH_INITRD, plan.initrd, plan.initrd_size) < 0) {
        printk("instaluj: nie moge zapisac %s\n", PATH_INITRD);
        return -1;
    }
    printk("  %s zapisany\n", PATH_INITRD);

    if (plan.fallback_free) {
        if (fat_mkdir(DIR_FALLBACK) < 0 ||
            fat_write_file(PATH_FALLBACK, plan.loader, plan.loader_size) < 0) {
            printk("instaluj: pliki sa, ale %s sie nie udal - maszyna "
                   "wystartuje\n          dopiero po dodaniu wpisu w "
                   "firmwarze.\n", PATH_FALLBACK);
            return 0;
        }
        printk("  %s zapisany\n", PATH_FALLBACK);
        printk("\nGotowe. Ta maszyna uruchomi ProtonOS-a z dysku.\n");
    } else {
        printk("\nGotowe. Pliki sa na partycji EFI; %s zostal cudzy,\n"
               "wiec dodaj w firmwarze wpis wskazujacy %s.\n",
               PATH_FALLBACK, PATH_LOADER);
    }
    return 0;
}
