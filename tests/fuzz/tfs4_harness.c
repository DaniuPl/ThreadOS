/* kernel/tfs4.c against damaged disks
 *
 * A disk is input like any other: somebody else's machine wrote it, or
 * nobody did and it is just broken. The whole image is the case: it is
 * the disk behind blk_read, TFS4 mounts it as the system does, and
 * everything it says is there is walked - every directory listed and
 * each one's path asked, every name looked up again by its path and
 * through ".." and its symbolic links, every file read at its start, in
 * its middle and at its end, its blocks asked the way a mapping asks.
 * Then it is written: a file made, filled past its direct blocks,
 * cut back, moved into a new directory, the first file found renamed
 * and removed - on a damaged disk the bitmaps, counts and directories
 * the writing trusts are damaged too. The sanitizers watch every read
 * and write; a mount that fails is a fine answer.
 *
 * Built by scripts/fuzz_parsers.py with stand-ins for the block layer,
 * the partition table and the task's rights (none of which the image
 * can reach). Usage: tfs4_harness <image>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "include/tfs4.h"
#include "include/rtc.h"

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

int blk_write(uint32_t lba, uint32_t count, const void *buf)
{
    if ((uint64_t)(lba + (uint64_t)count) * 512 > disk_len)
        return -1;
    memcpy(disk + (size_t)lba * 512, buf, (size_t)count * 512);
    return 0;
}

void rtc_read(struct rtc_time *t)
{
    t->year = 2026;
    t->month = 11;
    t->day = 19;
    t->hour = t->min = t->sec = 0;
}

void random_bytes(void *out, uint32_t n) { memset(out, 0x5A, n); }
static int held = 12;                      /* a file "still open" */
int task64_slot_open(int slot) { return slot == held; }
int task64_umask(void) { return 022; }
static int last_err;                       /* kernel/fs.c's, for tfs4.c */
void fs_set_errno(int e) { last_err = e; }
int fs_errno(void) { return last_err; }
int user_is_system_file(const char *n) { return !strcmp(n, "system.cfg"); }

int part_scan(void) { return 0; }
int part_get(int i, struct partition *p) { (void)i; (void)p; return 0; }
int task64_level(void) { return 2; }      /* the administrator: sees all */
int task64_uid(void) { return 0; }
int printk(const char *fmt, ...) { (void)fmt; return 0; }

static const struct fs_backend *b = &tfs4_backend;
static long budget = 4000;                /* entries walked, at most */
static long files_read;
static char path[FS_PATH_MAX];
static char first_file[FS_PATH_MAX];

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
        if (!first_file[0])
            memcpy(first_file, path, strlen(path) + 1);
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

/* --format <MiB>: the kernel's mkfs on a blank disk of that size, then
   the same writing as for a case, the disk left in TFS4_OUT for e2fsck */
static int format_case(long mib)
{
    disk_len = (size_t)mib << 20;
    disk = calloc(1, disk_len);
    if (!disk || !tfs4_format(0, (uint32_t)(disk_len / 512)) ||
        !tfs4_mount())
        return 1;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return 2;
    if (!strcmp(argv[1], "--format") && argc > 2) {
        if (format_case(atol(argv[2])))
            return 1;
        goto mounted;
    }
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
mounted:
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

        static unsigned char data[20000];
        for (size_t i = 0; i < sizeof(data); i++)
            data[i] = (unsigned char)(i * 7);
        int f = b->open(FS_ROOT, "/fuzz-new", 1);
        if (f >= 0) {
            b->write_at(f, 0, data, sizeof(data));
            b->write_at(f, 5u << 20, data, 4096);
            b->truncate_to(f, 3000);
            b->read_at(f, 0, data, 4096);
            b->file_block(f, 40, 1);
        }
        if (f >= 0) {
            b->chmod(f, 0640);
            b->utimes(f, 1000000000, FS_TIME_NOW);
            b->chown(f, 1000, 100);
        }
        b->link(FS_ROOT, "/fuzz-new", "/fuzz-second");
        b->symlink(FS_ROOT, "fuzz-new", "/fuzz-short");
        b->symlink(FS_ROOT, "/a/target/long/enough/to/need/a/block/of/its/"
                   "own/past/sixty/bytes/x", "/fuzz-long");
        static char target[FS_PATH_MAX];
        b->readlink(FS_ROOT, "/fuzz-short", target, sizeof(target));
        b->readlink(FS_ROOT, "/fuzz-long", target, sizeof(target));
        b->lstat_path(FS_ROOT, "/fuzz-long", &st);
        b->access(FS_ROOT, "/fuzz-short", 4);
        b->unlink(FS_ROOT, "/fuzz-short");
        b->unlink(FS_ROOT, "/fuzz-second");
        b->mkdir(FS_ROOT, "/fuzz-dir");
        b->rename(FS_ROOT, "/fuzz-new", "/fuzz-dir/moved");
        b->unlink(FS_ROOT, "/fuzz-dir/moved");
        b->unlink(FS_ROOT, "/fuzz-dir");
        if (first_file[0]) {
            b->rename(FS_ROOT, first_file, "/renamed-first");
            b->unlink(FS_ROOT, "/renamed-first");
        }
        b->unlink(FS_ROOT, "/lost+found");
        held = 0;                          /* and now closed */
        b->slot_closed(12);
    }
    /* the disk as the writing left it, for e2fsck to judge (a clean
       image in must come out clean) */
    const char *out = getenv("TFS4_OUT");
    if (out) {
        FILE *o = fopen(out, "wb");
        if (o) {
            fwrite(disk, 1, disk_len, o);
            fclose(o);
        }
    }
    free(disk);
    return 0;
}
