/* Volumes other than the system's, for programs
 *
 * A stick is the one place a backup can live that the machine's own
 * disk going wrong does not take with it - so Mend (user64/gmend.c)
 * has to reach one. The kernel could read and write a stick's FAT
 * already (kernel/fat.c), but only its shell asked it to (fat, fatls,
 * fatput). This is the same FAT for a program: which FAT volumes there
 * are on disks other than the system's, one of them mounted, and on it
 * what a backup needs - a name looked up, a folder listed, a file read
 * a piece at a time, a file written whole, a folder made, a file taken
 * away.
 *
 * One volume at a time, as fat.c has one: mounting another puts the
 * first away, and so does the kernel shell's 'fat'. The system's disk
 * stays the one the file system and the swapper use throughout - FAT
 * points the block layer at its own disk for each read and write and
 * puts it back (fat_mount_on).
 */

#include "include/vol.h"
#include "include/blk.h"
#include "include/fat.h"
#include "include/part.h"
#include "include/string.h"

static void copy_name(char *dst, const char *src, uint32_t cap)
{
    uint32_t n = 0;
    while (src && src[n] && n + 1 < cap) {
        dst[n] = src[n];
        n++;
    }
    dst[n] = 0;
}

/* One FAT volume, as found: mounted for a moment to read its label and
   its sizes */
static int describe(int disk, uint32_t lba, struct vol_info *v)
{
    if (!fat_mount_on(disk, lba))
        return 0;
    memset(v, 0, sizeof(*v));
    v->disk = disk;
    v->lba = lba;
    v->kib_total = fat_total_kib();
    v->kib_free = fat_free_kib();
    copy_name(v->label, fat_label(), sizeof(v->label));
    copy_name(v->kind, fat_kind(), sizeof(v->kind));
    copy_name(v->where, blk_name_of(disk), sizeof(v->where));
    fat_umount();
    return 1;
}

int vol_list(struct vol_info *out, int max)
{
    int n = 0, sys = blk_current();
    if (sys < 0 || max <= 0)
        return 0;
    /* the table is read with the block layer pointed at each disk in
       turn, and pointed back at once - as the shell's 'fat' does; system
       calls come one at a time under the big lock. A stick's transfers
       want interrupts, so they stay on. */
    for (int d = 0; d < blk_count() && n < max; d++) {
        if (d == sys || !blk_sectors_of(d) || blk_use(d) < 0)
            continue;
        struct partition pt;
        uint32_t lbas[16];
        int nl = 0, np = part_scan();
        for (int i = 0; i < np && nl < 16; i++)
            if (part_get(i, &pt) && pt.start < 0xFFFFFFFFull)
                lbas[nl++] = (uint32_t)pt.start;
        blk_use(sys);
        /* a stick formatted whole, with no table: FAT from its first
           sector */
        if (!nl)
            lbas[nl++] = 0;
        for (int i = 0; i < nl && n < max; i++)
            n += describe(d, lbas[i], &out[n]);
    }
    blk_use(sys);
    return n;
}

int vol_mount(int disk, uint32_t lba)
{
    if (disk < 0 || disk >= blk_count() || disk == blk_current())
        return -1;               /* the system's own disk is not a volume */
    return fat_mount_on(disk, lba) ? 0 : -1;
}

void vol_umount(void)
{
    fat_umount();
}

static void entry_of(const struct fat_entry *e, struct vol_entry *out)
{
    memset(out, 0, sizeof(*out));
    out->size = e->size;
    out->is_dir = e->is_dir;
    copy_name(out->name, e->name, sizeof(out->name));
}

int vol_stat(const char *path, struct vol_entry *out)
{
    struct fat_entry e;
    if (!fat_mounted())
        return -1;
    if (!strcmp(path, "/")) {
        memset(out, 0, sizeof(*out));
        out->is_dir = 1;
        return 1;
    }
    if (!fat_stat(path, &e))
        return 0;
    entry_of(&e, out);
    return 1;
}

int vol_dir(const char *path, int index, struct vol_entry *out)
{
    uint32_t clus;
    struct fat_entry e;
    if (!fat_mounted() || index < 0 || !fat_dir(path, &clus))
        return -1;
    if (!fat_readdir(clus, index, &e))
        return 0;
    entry_of(&e, out);
    return 1;
}

int vol_read(const char *path, uint32_t off, void *buf, uint32_t len)
{
    return fat_mounted() ? fat_read(path, off, buf, len) : -1;
}

int vol_write(const char *path, const void *data, uint32_t len)
{
    return fat_mounted() ? fat_write_file(path, data, len) : -1;
}

int vol_mkdir(const char *path)
{
    return fat_mounted() ? fat_mkdir(path) : -1;
}

int vol_unlink(const char *path)
{
    return fat_mounted() ? fat_unlink(path) : -1;
}
