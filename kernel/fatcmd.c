/* Looking into the EFI partition from the shell
 *
 * These exist so that what the FAT driver reads can be looked at by a
 * person, on a real machine, before anything writes there. A partition
 * belonging to the system that boots this laptop is not a place to
 * find out afterwards.
 *
 * They live in a file of their own because both shells have them and
 * neither of them should have its own copy.
 */

#include "include/fatcmd.h"
#include "include/fat.h"
#include "include/part.h"
#include "include/blk.h"
#include "include/printk.h"
#include "include/string.h"
#include "include/initrd.h"

/* Which partition to work on: the number given on the line, or - with
   nothing given - the first EFI one there is. */
static int pick(const char *arg, uint32_t *lba)
{
    int which = 0;
    struct partition pt;

    for (const char *p = arg; p && *p >= '0' && *p <= '9'; p++)
        which = which * 10 + (*p - '0');

    int np = part_scan();
    if (np <= 0) {
        printk("fat: the disk has no partition table\n");
        return -1;
    }
    if (which > 0) {
        if (!part_get(which - 1, &pt)) {
            printk("fat: there is no partition %u\n", (uint32_t)which);
            return -1;
        }
        *lba = (uint32_t)pt.start;
        return which;
    }
    for (int i = 0; i < np; i++) {
        if (!part_get(i, &pt))
            break;
        if (!strcmp(pt.kind, "EFI (ESP)")) {
            *lba = (uint32_t)pt.start;
            return i + 1;
        }
    }
    printk("fat: I found no EFI partition - give a number, e.g. 'fat 1'\n");
    return -1;
}

static int need(void)
{
    if (fat_mounted())
        return 1;
    printk("nothing is mounted - 'fat' first\n");
    return 0;
}

/* "fat <partition> [disk]" - and the second number is what makes a
   stick usable: the partition table and the filesystem are read from
   THAT disk, while everything else in the system goes on using the one
   it was using. Without it, looking at a pendrive would mean pointing
   the whole machine at it. */
void fatcmd_mount(const char *arg)
{
    uint32_t lba = 0;
    int disk = -1;
    int was = blk_current();

    if (arg) {
        const char *p = arg;
        while (*p == ' ')
            p++;
        while (*p >= '0' && *p <= '9')
            p++;
        while (*p == ' ')
            p++;
        if (*p >= '0' && *p <= '9') {
            int n = 0;
            while (*p >= '0' && *p <= '9')
                n = n * 10 + (*p++ - '0');
            disk = n - 1;
            if (disk < 0 || disk >= blk_count()) {
                printk("fat: there is no disk %u ('disks' shows how many "
                       "there are)\n", (uint32_t)n);
                return;
            }
            /* The table has to be read from the disk being asked about */
            blk_use(disk);
        }
    }

    int which = pick(arg ? arg : "", &lba);
    if (disk >= 0)
        blk_use(was);
    if (which < 0)
        return;
    if (!fat_mount_on(disk >= 0 ? disk : was, lba)) {
        printk("fat: partition %u (sector %u) is not a FAT\n",
               (uint32_t)which, lba);
        return;
    }
    printk("disk %u, partition %u, sector %u: %s%s%s\n",
           (uint32_t)((disk >= 0 ? disk : was) + 1), (uint32_t)which, lba,
           fat_kind(), fat_label()[0] ? ", label " : "", fat_label());
    printk("cluster %u B, %u KiB in all, %u KiB free\n",
           fat_cluster_bytes(), fat_total_kib(), fat_free_kib());
}

void fatcmd_ls(const char *path)
{
    uint32_t dir = 0;
    struct fat_entry e;
    int n = 0;

    if (!need())
        return;
    if (!path || !path[0])
        path = "/";
    if (!fat_dir(path, &dir)) {
        printk("fatls: %s - there is no such directory\n", path);
        return;
    }
    for (int i = 0; fat_readdir(dir, i, &e); i++) {
        if (e.is_dir)
            printk("  <dir>  %s\n", e.name);
        else
            printk("  %u B  %s\n", e.size, e.name);
        n++;
    }
    printk("%s: %u entries\n", path, (uint32_t)n);
}

void fatcmd_cat(const char *path)
{
    static char buf[1024];
    if (!need())
        return;
    if (!path || !path[0]) {
        printk("usage: fatcat <path>\n");
        return;
    }
    int n = fat_read(path, 0, buf, sizeof(buf) - 1);
    if (n < 0) {
        printk("fatcat: %s - there is no such file\n", path);
        return;
    }
    buf[n] = '\0';
    printk("%s", buf);
    if (n && buf[n - 1] != '\n')
        printk("\n");
}

void fatcmd_put(const char *arg)
{
    static char path[128];
    int n = 0;

    if (!need())
        return;
    if (!arg)
        arg = "";
    while (arg[n] && arg[n] != ' ' && n < (int)sizeof(path) - 1)
        n++;
    if (!arg[n]) {
        printk("usage: fatput <path> <text>\n");
        return;
    }
    memcpy(path, arg, (size_t)n);
    path[n] = '\0';

    const char *text = arg + n + 1;
    uint32_t len = 0;
    while (text[len])
        len++;
    if (fat_write_file(path, text, len) < 0)
        printk("fatput: could not write %s\n", path);
    else
        printk("fatput: %s, %u B\n", path, len);
}

void fatcmd_mkdir(const char *path)
{
    if (!need())
        return;
    if (!path || !path[0]) {
        printk("usage: fatmkdir <path>\n");
        return;
    }
    if (fat_mkdir(path) < 0)
        printk("fatmkdir: could not make %s\n", path);
    else
        printk("fatmkdir: %s\n", path);
}

void fatcmd_rm(const char *path)
{
    if (!need())
        return;
    if (!path || !path[0]) {
        printk("usage: fatrm <path>\n");
        return;
    }
    if (fat_unlink(path) < 0)
        printk("fatrm: could not remove %s\n", path);
    else
        printk("fatrm: %s\n", path);
}

/* The whole file through the driver, reduced to one number.
 *
 * A file bigger than a cluster is read by following a chain, and a
 * chain followed wrongly gives back bytes that look perfectly
 * plausible. One number over all of them, compared with the same
 * number computed by whoever wrote the file, is what says the chain
 * was followed and not merely walked.
 */
void fatcmd_sum(const char *path)
{
    static uint8_t buf[512];
    uint32_t sum = 0, total = 0, off = 0;

    if (!need())
        return;
    if (!path || !path[0]) {
        printk("usage: fatsum <path>\n");
        return;
    }
    for (;;) {
        int n = fat_read(path, off, buf, sizeof(buf));
        if (n < 0) {
            printk("fatsum: %s - there is no such file\n", path);
            return;
        }
        if (!n)
            break;
        for (int i = 0; i < n; i++)
            sum = ((sum << 1) | (sum >> 31)) + buf[i];
        total += (uint32_t)n;
        off += (uint32_t)n;
    }
    printk("fatsum: %s, %u B, sum %x\n", path, total, sum);
}

/* A file out of the initrd onto the FAT partition.
 *
 * This is the whole of what installing amounts to: the loader, the
 * kernel and the initrd are files the running system already has, and
 * putting them on the EFI partition is copying them there. The command
 * exists before the installer does because it is the part that can be
 * checked on its own - byte for byte, by a program that is not ours.
 */
void fatcmd_copy(const char *arg)
{
    static char from[64];
    int n = 0;

    if (!need())
        return;
    if (!arg)
        arg = "";
    while (arg[n] && arg[n] != ' ' && n < (int)sizeof(from) - 1)
        n++;
    if (!arg[n]) {
        printk("usage: fatcopy <name in the initrd> <path on the FAT>\n");
        return;
    }
    memcpy(from, arg, (size_t)n);
    from[n] = '\0';

    const char *to = arg + n + 1;
    while (*to == ' ')
        to++;
    if (!*to) {
        printk("usage: fatcopy <name in the initrd> <path on the FAT>\n");
        return;
    }

    uint32_t size = 0;
    const void *data = initrd_find(from, &size);
    if (!data) {
        printk("fatcopy: the initrd has no '%s'\n", from);
        return;
    }
    if (fat_write_file(to, data, size) < 0)
        printk("fatcopy: could not write %s\n", to);
    else
        printk("fatcopy: %s -> %s, %u B\n", from, to, size);
}
