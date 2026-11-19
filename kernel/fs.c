/* The file system's front
 *
 * Everything in the kernel reaches files through the fs_* calls of
 * fs.h, and they arrive here. This picks the format on the disk at
 * mount and hands each call to it (struct fs_backend): TFS3 today
 * (kernel/tfs3.c); TFS4, the ext2 format, takes its place next
 * (docs/tfs4-plan.md), with TFS3 kept for the disks it is on.
 *
 * What does not depend on the format lives here: whether there is a
 * file system at all, the API's own limits (a path fits FS_PATH_MAX),
 * a listing by index that does not start from the top on every call,
 * and the kernel shell's conveniences in the root.
 */

#include "include/fs.h"
#include "include/fsbackend.h"
#include "include/tfs3.h"
#include "include/string.h"
#include "include/printk.h"

static const struct fs_backend *be;

/* A path the API takes: not empty, and short enough */
static int path_fits(const char *path)
{
    if (!path || !path[0])
        return 0;
    for (uint32_t i = 0; i < FS_PATH_MAX; i++)
        if (!path[i])
            return 1;
    return 0;
}

int fs_init(void)
{
    if (tfs3_mount()) {
        be = &tfs3_backend;
        return 1;
    }
    be = 0;
    return 0;
}

int fs_ready(void)
{
    return be != 0;
}

int fs_format_now(int partition)
{
    if (!tfs3_format(partition))
        return 0;
    be = &tfs3_backend;
    return 1;
}

/* --- paths --- */

/* Where the last listing by index stopped. Programs and the desktop
   ask for entry 0, 1, 2 ... of one directory in turn; for each the
   cursor of the one before is enough to go on from, where starting
   from the top every time costs the square of the directory's size. */
static struct {
    int valid, dir, index;
    uint64_t cursor;
} last;


/* A change to what a directory holds: a listing in progress starts
   again from the top rather than skip or repeat an entry */
static void changed(void)
{
    last.valid = 0;
}

int fs_open(int cwd, const char *path, int create)
{
    if (create)
        changed();
    return be && path_fits(path) ? be->open(cwd, path, create) : -1;
}

int fs_mkdir(int cwd, const char *path)
{
    changed();
    return be && path_fits(path) ? be->mkdir(cwd, path) : -1;
}

int fs_chdir(int cwd, const char *path, int *newcwd)
{
    return be && path_fits(path) ? be->chdir(cwd, path, newcwd) : -1;
}

int fs_unlink(int cwd, const char *path)
{
    changed();
    return be && path_fits(path) ? be->unlink(cwd, path) : -1;
}

int fs_rename(int cwd, const char *from, const char *to)
{
    changed();
    return be && path_fits(from) && path_fits(to)
               ? be->rename(cwd, from, to) : -1;
}

int fs_getpath(int slot, char *buf, uint32_t cap)
{
    if (!buf || cap < 2)
        return -1;
    if (!be || slot == FS_ROOT || slot < 0) {
        buf[0] = '/';
        buf[1] = '\0';
        return 1;
    }
    return be->getpath(slot, buf, cap);
}

int fs_stat_path(int cwd, const char *path, struct fs_stat *st)
{
    return be && path_fits(path) && st ? be->stat_path(cwd, path, st) : -1;
}

int fs_stat_slot(int slot, struct fs_stat *st)
{
    return be && st ? be->stat_slot(slot, st) : -1;
}

/* --- listings --- */

int fs_readdir_at(int dir, uint64_t *cursor, struct fs_dirent *e)
{
    return be && cursor && e ? be->readdir_at(dir, cursor, e) : -1;
}

int fs_readdir(int dir, int index, char *name, uint64_t *size, int *is_d)
{
    if (!be || index < 0)
        return 0;
    static struct fs_dirent e;
    uint64_t cursor = 0;
    int at = 0;
    if (last.valid && last.dir == dir && last.index <= index) {
        cursor = last.cursor;
        at = last.index;
    }
    for (;;) {
        int r = be->readdir_at(dir, &cursor, &e);
        if (r != 1) {
            last.valid = 0;
            return 0;
        }
        if (at == index)
            break;
        at++;
    }
    last.valid = 1;
    last.dir = dir;
    last.index = index + 1;
    last.cursor = cursor;
    if (name)
        memcpy(name, e.name, strlen(e.name) + 1);
    if (size)
        *size = e.size;
    if (is_d)
        *is_d = e.is_dir;
    return 1;
}

/* --- by slot --- */

int64_t fs_size(int slot)
{
    return be ? be->size(slot) : -1;
}

int fs_owner(int slot)
{
    return be ? be->owner(slot) : -1;
}

int fs_can_write(int slot)
{
    return be ? be->can_write(slot) : 0;
}

int fs_set_owner(int slot, int uid)
{
    return be ? be->set_owner(slot, uid) : -1;
}

int fs_truncate(int slot)
{
    return fs_truncate_to(slot, 0);
}

int fs_truncate_to(int slot, uint64_t size)
{
    return be ? be->truncate_to(slot, size) : -1;
}

void fs_slot_closed(int slot)
{
    if (be)
        be->slot_closed(slot);
}

int fs_read_at(int slot, uint64_t off, void *buf, uint32_t len)
{
    return be ? be->read_at(slot, off, buf, len) : -1;
}

int fs_write_at(int slot, uint64_t off, const void *buf, uint32_t len)
{
    return be ? be->write_at(slot, off, buf, len) : -1;
}

/* --- raw blocks --- */

uint32_t fs_block_size(void)
{
    return be ? be->block_size() : 0;
}

uint32_t fs_file_block(int slot, uint32_t index, int alloc)
{
    return be ? be->file_block(slot, index, alloc) : 0;
}

uint32_t fs_block_lba(uint32_t block)
{
    return be ? be->block_lba(block) : 0;
}

void fs_usage(uint64_t *used, uint64_t *total)
{
    if (used)
        *used = 0;
    if (total)
        *total = 0;
    if (be)
        be->usage(used, total);
}

/* --- the kernel shell's conveniences, in the root --- */

int fs_write(const char *name, const void *data, uint32_t len)
{
    int idx = fs_open(FS_ROOT, name, 1);
    if (idx < 0)
        return -1;
    fs_truncate(idx);
    return fs_write_at(idx, 0, data, len) < 0 ? -1 : 0;
}

int fs_read(const char *name, void *buf, uint32_t max)
{
    int idx = fs_open(FS_ROOT, name, 0);
    if (idx < 0)
        return -1;
    return fs_read_at(idx, 0, buf, max);
}

void fs_list(void)
{
    if (!be) {
        printk("FS: there is no disk\n");
        return;
    }
    static char name[FS_NAME_LEN + 1];
    uint64_t size;
    int is_d;
    int n = 0;
    printk("Root directory:\n");
    for (int i = 0; fs_readdir(FS_ROOT, i, name, &size, &is_d); i++) {
        if (is_d)
            printk("  %s/\n", name);
        else
            printk("  %s  (%u B)\n", name, (uint32_t)size);
        n++;
    }
    if (!n)
        printk("  (empty)\n");
}
