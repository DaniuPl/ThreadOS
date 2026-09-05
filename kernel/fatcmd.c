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
        printk("fat: na dysku nie ma tablicy partycji\n");
        return -1;
    }
    if (which > 0) {
        if (!part_get(which - 1, &pt)) {
            printk("fat: nie ma partycji %u\n", (uint32_t)which);
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
    printk("fat: nie znalazlem partycji EFI - podaj numer, np. 'fat 1'\n");
    return -1;
}

static int need(void)
{
    if (fat_mounted())
        return 1;
    printk("nic nie jest podmontowane - najpierw 'fat'\n");
    return 0;
}

void fatcmd_mount(const char *arg)
{
    uint32_t lba = 0;
    int which = pick(arg ? arg : "", &lba);
    if (which < 0)
        return;
    if (!fat_mount(lba)) {
        printk("fat: partycja %u (sektor %u) to nie jest FAT\n",
               (uint32_t)which, lba);
        return;
    }
    printk("partycja %u, sektor %u: %s%s%s\n", (uint32_t)which, lba,
           fat_kind(), fat_label()[0] ? ", etykieta " : "", fat_label());
    printk("klaster %u B, razem %u KiB, wolnych %u KiB\n",
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
        printk("fatls: %s - nie ma takiego katalogu\n", path);
        return;
    }
    for (int i = 0; fat_readdir(dir, i, &e); i++) {
        if (e.is_dir)
            printk("  <kat>  %s\n", e.name);
        else
            printk("  %u B  %s\n", e.size, e.name);
        n++;
    }
    printk("%s: %u pozycji\n", path, (uint32_t)n);
}

void fatcmd_cat(const char *path)
{
    static char buf[1024];
    if (!need())
        return;
    if (!path || !path[0]) {
        printk("uzycie: fatcat <sciezka>\n");
        return;
    }
    int n = fat_read(path, 0, buf, sizeof(buf) - 1);
    if (n < 0) {
        printk("fatcat: %s - nie ma takiego pliku\n", path);
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
        printk("uzycie: fatput <sciezka> <tresc>\n");
        return;
    }
    memcpy(path, arg, (size_t)n);
    path[n] = '\0';

    const char *text = arg + n + 1;
    uint32_t len = 0;
    while (text[len])
        len++;
    if (fat_write_file(path, text, len) < 0)
        printk("fatput: nie udalo sie zapisac %s\n", path);
    else
        printk("fatput: %s, %u B\n", path, len);
}

void fatcmd_mkdir(const char *path)
{
    if (!need())
        return;
    if (!path || !path[0]) {
        printk("uzycie: fatmkdir <sciezka>\n");
        return;
    }
    if (fat_mkdir(path) < 0)
        printk("fatmkdir: nie udalo sie zrobic %s\n", path);
    else
        printk("fatmkdir: %s\n", path);
}

void fatcmd_rm(const char *path)
{
    if (!need())
        return;
    if (!path || !path[0]) {
        printk("uzycie: fatrm <sciezka>\n");
        return;
    }
    if (fat_unlink(path) < 0)
        printk("fatrm: nie udalo sie usunac %s\n", path);
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
        printk("uzycie: fatsum <sciezka>\n");
        return;
    }
    for (;;) {
        int n = fat_read(path, off, buf, sizeof(buf));
        if (n < 0) {
            printk("fatsum: %s - nie ma takiego pliku\n", path);
            return;
        }
        if (!n)
            break;
        for (int i = 0; i < n; i++)
            sum = ((sum << 1) | (sum >> 31)) + buf[i];
        total += (uint32_t)n;
        off += (uint32_t)n;
    }
    printk("fatsum: %s, %u B, suma %x\n", path, total, sum);
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
        printk("uzycie: fatcopy <nazwa w initrd> <sciezka na FAT>\n");
        return;
    }
    memcpy(from, arg, (size_t)n);
    from[n] = '\0';

    const char *to = arg + n + 1;
    while (*to == ' ')
        to++;
    if (!*to) {
        printk("uzycie: fatcopy <nazwa w initrd> <sciezka na FAT>\n");
        return;
    }

    uint32_t size = 0;
    const void *data = initrd_find(from, &size);
    if (!data) {
        printk("fatcopy: w initrd nie ma '%s'\n", from);
        return;
    }
    if (fat_write_file(to, data, size) < 0)
        printk("fatcopy: nie udalo sie zapisac %s\n", to);
    else
        printk("fatcopy: %s -> %s, %u B\n", from, to, size);
}
