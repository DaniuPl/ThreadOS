/* Installing ThreadOS onto the machine's own disk
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
 * Two paths are written. \EFI\ThreadOS is ours and is always written.
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
#include "include/desktop.h"
#include "include/gpt.h"
#include "include/fs.h"

#ifdef __x86_64__
#include "../kernel64/paging64.h"
#define IDENTITY_TOP() paging64_identity_top()
#else
#include "include/paging.h"
#define IDENTITY_TOP() paging_identity_limit()
#endif

#define DIR_OURS     "/EFI/ThreadOS"
#define PATH_LOADER  "/EFI/ThreadOS/BOOTX64.EFI"
#define PATH_KERNEL  "/EFI/ThreadOS/threados64.bin"
#define PATH_INITRD  "/EFI/ThreadOS/initrd64.img"
#define DIR_FALLBACK "/EFI/BOOT"
#define PATH_FALLBACK "/EFI/BOOT/BOOTX64.EFI"

/* The EFI partition we make when the disk becomes ours. A hundred and
   twenty-eight megabytes is far more than three files need and is the
   smallest size at which FAT32 is comfortably FAT32 - the format is
   decided by having more than 65524 clusters, and a smaller partition
   has to use clusters so small that the table itself gets silly. */
#define ESP_MIB 128

static struct {
    int ready;
    int was;                     /* the disk in use before we looked */
    struct install_info info;
    const uint8_t *loader;
    const uint8_t *kernel;
    const uint8_t *initrd;
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

static void say(char *dst, const char *src, int max)
{
    int i = 0;
    while (src[i] && i < max - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

/* Looking is over: the FAT the looking may have mounted is let go and
   the machine is pointed back at the disk it was using. Both matter -
   a mounted volume on a disk nobody is looking at any more is a
   driver holding a sector cache from somewhere else. */
static void done_looking(void)
{
    fat_umount();
    if (plan.was >= 0 && blk_current() != plan.was)
        blk_use(plan.was);
}

static int no(struct install_info *out, uint32_t why)
{
    plan.ready = 0;
    plan.info.problem = why;
    plan.info.ok = 0;
    done_looking();
    if (out)
        *out = plan.info;
    return 0;
}

int install_look(int mode, int disk, struct install_info *out)
{
    struct partition pt;
    struct fat_entry e;
    struct install_info *in = &plan.info;

    memset(&plan, 0, sizeof(plan));
    in->problem = INST_OK;
    in->mode = (uint32_t)(mode == INST_WHOLE ? INST_WHOLE : INST_ALONGSIDE);
    in->steps = in->mode == INST_WHOLE ? INST_STEPS_WHOLE : INST_STEPS;

    if (!blk_present())
        return no(out, INST_NO_DISK);

    /* Which disk this is about. Reading another one's table means
       pointing the block layer at it, because everything below -
       part.c, fat.c - reads the disk in use. But LOOKING MUST CHANGE
       NOTHING, and leaving the machine pointed at a disk somebody
       merely asked about would leave its filesystem reading a disk it
       was never mounted from. So the disk in use is put back before
       this returns, and install_step() points at the chosen one again
       when the time comes to write. */
    int was = blk_current();
    if (disk >= 0 && blk_use(disk) < 0)
        return no(out, INST_NO_DISK);
    in->disk = (uint32_t)blk_current();
    in->disks = (uint32_t)blk_count();
    plan.was = was;

    /* What we would write. If the machine was not started by a loader
       that hands these over, there is nothing to install and no
       reason to look at the disk at all. */
    plan.loader = module_at("loader", &in->loader_size);
    plan.kernel = module_at("the kernel", &in->kernel_size);
    plan.initrd = initrd_image(&in->initrd_size);
    if (!plan.loader || !plan.kernel || !plan.initrd || !in->initrd_size)
        return no(out, INST_NO_FILES);

    say(in->disk_name, blk_name(), INST_NAME);
    in->disk_mib = blk_sectors() / 2048;
    /* Whether taking the whole disk is even possible is worth saying
       on every screen, including the ones that are about the other
       way: a person told "there is no EFI partition here" wants to
       know in the same breath whether there is another way at all. */
    in->can_wipe = in->disk_mib >= GPT_MIN_MIB;

    int np = part_scan();
    in->parts = np > 0 ? (uint32_t)np : 0;

    if (in->mode == INST_WHOLE) {
        /* Nothing to look at on the disk: what matters is whether
           what we are about to make FITS. Everything that is there
           now is going away, so its shape is not a question. */
        if (!in->can_wipe)
            return no(out, INST_TOO_SMALL);
        in->esp_index = 1;
        in->esp_lba = 2048;
        say(in->kind, "FAT32", 8);
        say(in->label, "EFI", INST_NAME);
        in->free_kib = ESP_MIB * 1024;
        in->fallback_free = 1;         /* a fresh partition has nobody in it */
        in->need_kib = (in->loader_size + in->kernel_size + in->initrd_size
                        + in->loader_size) / 1024 + 4;
        in->ok = 1;
        plan.ready = 1;
        done_looking();
        if (out)
            *out = *in;
        return 1;
    }

    if (np <= 0)
        return no(out, INST_NO_TABLE);

    for (int i = 0; i < np; i++) {
        if (!part_get(i, &pt))
            break;
        if (!strcmp(pt.kind, "EFI (ESP)")) {
            in->esp_index = (uint32_t)(i + 1);
            in->esp_lba = (uint32_t)pt.start;
            break;
        }
    }
    if (!in->esp_index)
        return no(out, INST_NO_ESP);
    if (!fat_mount(in->esp_lba))
        return no(out, INST_NOT_FAT);

    say(in->kind, fat_kind(), 8);
    say(in->label, fat_label(), INST_NAME);
    in->free_kib = fat_free_kib();
    in->fallback_free = !fat_stat(PATH_FALLBACK, &e);
    in->need_kib = (in->loader_size + in->kernel_size + in->initrd_size
                    + (in->fallback_free ? in->loader_size : 0)) / 1024 + 4;
    if (in->free_kib < in->need_kib)
        return no(out, INST_NO_ROOM);

    in->ok = 1;
    plan.ready = 1;
    done_looking();
    if (out)
        *out = *in;
    return 1;
}

/* The steps, in the order they happen. Taking the whole disk adds
   three in front - a table, a filesystem on the EFI partition and one
   on ours - and after those it is the same work. The last step is the
   only one that may decline to do anything. */
const char *install_step_name(int n)
{
    if (plan.info.mode == INST_WHOLE) {
        switch (n) {
        case 0: return "partition table (GPT)";
        case 1: return "EFI partition (FAT32)";
        case 2: return "ThreadOS partition (TFS)";
        default: n -= 3; break;
        }
    }
    switch (n) {
    case 0: return DIR_OURS;
    case 1: return PATH_LOADER;
    case 2: return PATH_KERNEL;
    case 3: return PATH_INITRD;
    case 4: return PATH_FALLBACK;
    default: return "?";
    }
}

/* The three that only the whole-disk way takes. Each of them is
   final, and each of them has to leave the disk in a state the next
   one can read - so the table is re-read after it is written rather
   than assumed. */
static int step_wipe(int n)
{
    struct partition pt;

    switch (n) {
    case 0:
        return gpt_make(ESP_MIB) < 0 ? -1 : 0;
    case 1: {
        int np = part_scan();
        if (np < 2 || !part_get(0, &pt))
            return -1;
        if (fat_mkfs((uint32_t)pt.start, (uint32_t)pt.sectors, "EFI") < 0)
            return -1;
        return fat_mount((uint32_t)pt.start) ? 0 : -1;
    }
    case 2:
        /* The system's own partition. Not needed to boot - the loader
           reads everything it needs off the EFI partition - but a
           machine that installs itself and then has nowhere to keep a
           file is only half installed. */
        return fs_format_now(2) ? 0 : -1;
    default:
        return -1;
    }
}

int install_step(int n)
{
    if (!plan.ready)
        return -1;
    /* Back onto the disk the plan is about. Looking put the machine
       back where it was; writing is the moment that is undone. */
    if (blk_current() != (int)plan.info.disk &&
        blk_use((int)plan.info.disk) < 0)
        return -1;

    if (plan.info.mode == INST_WHOLE) {
        if (n < 3)
            return step_wipe(n);
        n -= 3;
    } else if (!fat_mounted() && !fat_mount(plan.info.esp_lba)) {
        /* The EFI partition, mounted again: looking let it go so that
           nothing would be left holding a volume on a disk nobody
           chose. */
        return -1;
    }
    switch (n) {
    case 0:
        return fat_mkdir(DIR_OURS) < 0 ? -1 : 0;
    case 1:
        return fat_write_file(PATH_LOADER, plan.loader,
                              plan.info.loader_size) < 0 ? -1 : 0;
    case 2:
        return fat_write_file(PATH_KERNEL, plan.kernel,
                              plan.info.kernel_size) < 0 ? -1 : 0;
    case 3:
        return fat_write_file(PATH_INITRD, plan.initrd,
                              plan.info.initrd_size) < 0 ? -1 : 0;
    case 4:
        /* The name the firmware falls back to. On a machine that has
           one already it belongs to whoever boots that machine today,
           and 1 means "there was nothing here for us to do" - which is
           not a failure and must not be reported as one. */
        if (!plan.info.fallback_free)
            return 1;
        if (fat_mkdir(DIR_FALLBACK) < 0)
            return -1;
        return fat_write_file(PATH_FALLBACK, plan.loader,
                              plan.info.loader_size) < 0 ? -1 : 0;
    default:
        return -1;
    }
}

/* --- and the same thing said out loud, for the shells -------------- */

static void complain(uint32_t why, const struct install_info *in)
{
    switch (why) {
    case INST_NO_DISK:
        printk("install: there is no disk\n");
        break;
    case INST_NO_FILES:
        printk("install: this system has nothing to install itself from.\n");
        printk("         The boot loader has to hand over the kernel image and "
               "itself\n");
        printk("         as modules - threadboot does that (booting through "
               "UEFI off a stick).\n");
        break;
    case INST_NO_TABLE:
        printk("install: the disk has no partition table, so it has no "
               "EFI partition either.\n");
        break;
    case INST_NO_ESP:
        printk("install: I found no EFI partition (%u partitions on the "
               "disk).\n", in->parts);
        break;
    case INST_NOT_FAT:
        printk("install: partition %u is not a FAT - and an EFI partition "
               "has to be one.\n", in->esp_index);
        break;
    case INST_NO_ROOM:
        printk("install: the EFI partition has %u KiB free, and it needs "
               "about %u KiB.\n", in->free_kib, in->need_kib);
        break;
    case INST_TOO_SMALL:
        printk("install: this disk has %u MiB - too little for a partition "
               "table of our own.\n", in->disk_mib);
        break;
    default:
        printk("install: I do not know what is wrong\n");
        break;
    }
}

int install_plan(int mode, int disk)
{
    struct install_info in;

    if (!install_look(mode, disk, &in)) {
        complain(in.problem, &in);
        return 0;
    }

    printk("Disk %u of %u: %s, %u MiB\n", in.disk + 1, in.disks,
           in.disk_name, in.disk_mib);
    if (in.mode == INST_WHOLE) {
        printk("\nTHIS WHOLE DISK WILL BE ThreadOS. It will lose "
               "EVERYTHING:\n");
        printk("  partition table, %u partitions and everything that is on "
               "them.\n", in.parts);
        printk("I will make: a GPT, an EFI partition of %u MiB (FAT32) and "
               "a ThreadOS\n        partition (TFS) on the rest of the disk.\n",
               (uint32_t)ESP_MIB);
    }
    else
        printk("EFI partition: number %u, sector %u, %s%s%s, free %u KiB\n",
               in.esp_index, in.esp_lba, in.kind, in.label[0] ? ", " : "",
               in.label, in.free_kib);
    printk("\nI will write:\n");
    printk("  %s  %u B\n", PATH_LOADER, in.loader_size);
    printk("  %s  %u B\n", PATH_KERNEL, in.kernel_size);
    printk("  %s  %u B\n", PATH_INITRD, in.initrd_size);
    if (in.fallback_free)
        printk("  %s  %u B  (not there - the machine will boot on its own)\n",
               PATH_FALLBACK, in.loader_size);
    else
        printk("  %s IS TAKEN ALREADY - I am not touching it.\n", PATH_FALLBACK);

    if (in.mode != INST_WHOLE)
        printk("\nI do NOT touch: the partition table, other partitions or "
               "any file\nthat is on the EFI partition already.\n");
    if (!in.fallback_free)
        printk("To start ThreadOS you will have to add an entry to the "
               "firmware's boot menu\nand point it at %s.\n", PATH_LOADER);
    return 1;
}

int install_do(void)
{
    for (int n = 0; n < (int)plan.info.steps; n++) {
        int rc = install_step(n);
        if (rc < 0) {
            printk("install: I cannot write %s\n", install_step_name(n));
            return -1;
        }
        if (rc == 0)
            printk("  %s - done\n", install_step_name(n));
    }
    if (plan.info.fallback_free)
        printk("\nDone. This machine will boot ThreadOS off the disk.\n");
    else
        printk("\nDone. The files are on the EFI partition; %s stayed somebody else's,\n"
               "so add an entry in the firmware pointing at %s.\n",
               PATH_FALLBACK, PATH_LOADER);
    return 0;
}

/* --- and the way it is meant to be done ------------------------------
 *
 * A window, on a screen with nothing else on it. Installing is not one
 * task among others: while a disk is being written to, a taskbar and
 * somebody's other windows within reach are not a convenience, they
 * are a way to click something else by mistake. So the compositor is
 * started in the mode that has none of it, with the installer as the
 * only program - and the only thing the environment offers besides the
 * installer is restarting the machine.
 */
int install_graphical(void)
{
    return desktop_start_kiosk("ginstal");
}
