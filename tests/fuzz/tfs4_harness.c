/* kernel/tfs4.c against damaged disks
 *
 * A disk is input like any other: somebody else's machine wrote it, or
 * nobody did and it is just broken. The whole image is the case: it is
 * the disk behind blk_read, TFS4 mounts it as the system does, and
 * everything it says is there is walked - every directory listed and
 * each one's path asked, every name looked up again by its path and
 * through ".." and its symbolic links, every file read at its start, in
 * its middle and at its end, its blocks asked the way a mapping asks.
 * The sanitizers watch every read; a mount that fails is a fine answer.
 *
 * Built by scripts/fuzz_parsers.py with stand-ins for the block layer,
 * the partition table and the task's rights (none of which the image
 * can reach). Usage: tfs4_harness <image>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "include/tfs4.h"

static unsigned char *disk;
static size_t disk_len;

int blk_present(void) { return 1; }
uint32_t blk_sectors(void) { return (uint32_t)(disk_len / 512); }

int blk_read(uint32_t lba, uint32_t count, void *buf)
{
    if ((uint64_t)(lba + (uint64_t)count) * 512 > disk_len)
        return -1;
    memcpy(buf, disk + (size_t)lba * 512, (size_t)count * 512);
    return 0;
}

int part_scan(void) { return 0; }
int part_get(int i, struct partition *p) { (void)i; (void)p; return 0; }
int task64_level(void) { return 2; }      /* the administrator: sees all */
int task64_uid(void) { return 0; }
int printk(const char *fmt, ...) { (void)fmt; return 0; }

static const struct fs_backend *b = &tfs4_backend;
static long budget = 4000;                /* entries walked, at most */
static long files_read;
static char path[FS_PATH_MAX];

static void walk(int dir, int depth)
{
    if (depth > 24 || budget <= 0)
        return;
    static struct fs_dirent e;
    char *mine = malloc(FS_PATH_MAX);
    b->getpath(dir, mine, FS_PATH_MAX);
    uint64_t cursor = 0;
    int r;
    while (budget-- > 0 && (r = b->readdir_at(dir, &cursor, &e)) == 1) {
        size_t n = strlen(mine);
        if (n + 1 + strlen(e.name) + 1 > FS_PATH_MAX)
            continue;
        snprintf(path, sizeof(path), "%s/%s", n > 1 ? mine : "", e.name);
        struct fs_stat st;
        if (b->stat_path(FS_ROOT, path, &st) == 0 && st.type == FS_T_DIR) {
            int sub;
            if (b->chdir(FS_ROOT, path, &sub) == 0 && sub != dir) {
                int back;
                b->chdir(sub, "..", &back);
                walk(sub, depth + 1);
            }
            continue;
        }
        int f = b->open(FS_ROOT, path, 0);
        if (f < 0)
            continue;
        files_read++;
        int64_t size = b->size(f);
        static unsigned char buf[8192];
        b->read_at(f, 0, buf, sizeof(buf));
        if (size > 0) {
            b->read_at(f, (uint64_t)size / 2, buf, 4096);
            b->read_at(f, (uint64_t)size - 1, buf, 1);
            b->file_block(f, (uint32_t)(((uint64_t)size - 1) /
                                        b->block_size()), 0);
        }
        b->read_at(f, (uint64_t)1 << 40, buf, 16);
    }
    free(mine);
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return 2;
    FILE *fh = fopen(argv[1], "rb");
    if (!fh)
        return 2;
    fseek(fh, 0, SEEK_END);
    long n = ftell(fh);
    fseek(fh, 0, SEEK_SET);
    if (n <= 0 || n > (64 << 20)) {
        fclose(fh);
        return 0;
    }
    /* exactly its length: a read past the image is a read past an
       allocation, which the sanitizer sees */
    disk = malloc((size_t)n);
    disk_len = fread(disk, 1, (size_t)n, fh);
    fclose(fh);
    if (tfs4_mount()) {
        walk(FS_ROOT, 0);
        /* for whoever wants to see that the cases reach the files */
        if (getenv("TFS4_SAY"))
            printf("mounted, %ld files read\n", files_read);
        uint64_t used, total;
        b->usage(&used, &total);
        struct fs_stat st;
        b->stat_path(FS_ROOT, "/../../.", &st);
        b->stat_path(FS_ROOT, "link/link/link/link/link/link/link/link/x",
                     &st);
    }
    free(disk);
    return 0;
}
