/* Moving an installed system from TFS3 to TFS4
 *
 * A disk that still holds TFS3 - the 64-file file system ThreadOS had
 * before TFS4 (docs/tfs4-plan.md, stage 5) - is moved over at the first
 * start of a kernel that has TFS4, in place, on the same partition:
 *
 *   1. TFS3's whole area (its bitmap reaches 32 MiB at most) is read
 *      into memory;
 *   2. a copy of it goes onto the EFI partition, as
 *      \EFI\ThreadOS\tfs3-backup.img, and beside it a note,
 *      tfs3-moving, saying where on the disk it came from;
 *   3. a partition whose GPT type is not ThreadOS's yet becomes ours
 *      (TFS4 is found by its type; TFS3 was found by its superblock);
 *   4. the partition is formatted with TFS4;
 *   5. every directory and file of the copy is made again, with its
 *      owner (swap.sys is not: the next start makes it anew);
 *   6. the note goes. The copy stays, for whoever wants the old disk
 *      back.
 *
 * A start that finds the note finishes what the last one began, from
 * the copy, since the partition may by then be half TFS4: steps 3 to 6
 * again. A disk with no EFI partition (a test's image) is moved from
 * memory alone, and says so.
 */

#include "include/tfs3to4.h"
#include "include/tfs3.h"
#include "include/tfs4.h"
#include "include/fat.h"
#include "include/part.h"
#include "include/gpt.h"
#include "include/blk.h"
#include "include/pmm.h"
#include "include/string.h"
#include "include/printk.h"

#define DIR_OURS    "/EFI/ThreadOS"
#define PATH_COPY   "/EFI/ThreadOS/tfs3-backup.img"
#define PATH_NOTE   "/EFI/ThreadOS/tfs3-moving"

static struct {
    uint8_t *mem;                 /* the copy, contiguous */
    uint32_t frames;
    uint32_t bytes;
    int files, dirs, lost;
} mv;

static int esp_was_mounted;

/* The EFI partition, mounted: 1 when there is one */
static int esp_mount(void)
{
    esp_was_mounted = fat_mounted();
    if (esp_was_mounted)
        return 1;
    int n = part_scan();
    for (int i = 0; i < n; i++) {
        struct partition pt;
        if (part_get(i, &pt) && !strcmp(pt.kind, "EFI (ESP)") &&
            pt.start <= 0xFFFFFFFFu && fat_mount((uint32_t)pt.start))
            return 1;
    }
    return 0;
}

static void esp_done(void)
{
    if (!esp_was_mounted)
        fat_umount();
}

static int mem_take(uint32_t bytes)
{
    mv.frames = (bytes + 4095) / 4096;
    uint32_t at = pmm_alloc_contiguous(mv.frames);
    if (!at)
        return 0;
    mv.mem = (uint8_t *)(uintptr_t)at;
    mv.bytes = bytes;
    return 1;
}

static void mem_give_back(void)
{
    for (uint32_t i = 0; i < mv.frames; i++)
        pmm_free_frame((uint32_t)(uintptr_t)mv.mem + i * 4096);
    mv.mem = 0;
    mv.frames = 0;
}

static void say_number(char *out, uint32_t v)
{
    char tmp[12];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    int k = 0;
    while (n)
        out[k++] = tmp[--n];
    out[k] = '\0';
}

static uint32_t read_number(const char **p)
{
    uint32_t v = 0;
    while (**p == ' ')
        (*p)++;
    while (**p >= '0' && **p <= '9')
        v = v * 10 + (uint32_t)(*(*p)++ - '0');
    return v;
}

/* --- making the tree again ------------------------------------------- */

static void copy_tree(int dir, char *path, uint32_t len, int depth)
{
    static struct fs_dirent e;
    static uint8_t chunk[4096];
    if (depth > 64)
        return;
    uint64_t cursor = 0;
    for (;;) {
        /* the listing is TFS3's, from the copy; one entry at a time,
           since what is under it is listed in between */
        uint64_t at = cursor;
        if (tfs3_backend.readdir_at(dir, &at, &e) != 1)
            break;
        cursor = at;
        uint32_t nl = (uint32_t)strlen(e.name);
        if (len + 1 + nl + 1 > FS_PATH_MAX)
            continue;
        if (depth == 0 && !strcmp(e.name, "swap.sys"))
            continue;
        path[len] = '/';
        memcpy(path + len + 1, e.name, nl + 1);
        int owner = tfs3_backend.owner(e.slot);
        int child = e.slot;
        if (e.is_dir) {
            int made;
            if (tfs4_backend.mkdir(FS_ROOT, path) < 0 ||
                tfs4_backend.chdir(FS_ROOT, path, &made) < 0) {
                mv.lost++;
                printk("[ !! ] TFS4: %s could not be made again\n", path);
            } else {
                mv.dirs++;
                if (owner >= 0)
                    tfs4_backend.set_owner(made, owner);
                /* a home is its owner's alone, as kernel/user.c makes
                   them now */
                if (depth == 1 && !memcmp(path, "/home/", 6))
                    tfs4_backend.chmod(made, 0700);
                copy_tree(child, path, len + 1 + nl, depth + 1);
            }
        } else {
            int to = tfs4_backend.open(FS_ROOT, path, 1);
            int64_t size = tfs3_backend.size(child);
            int ok = to >= 0 && size >= 0;
            for (uint64_t off = 0; ok && off < (uint64_t)size;) {
                int n = tfs3_backend.read_at(child, off, chunk, sizeof(chunk));
                if (n <= 0 || tfs4_backend.write_at(to, off, chunk,
                                                    (uint32_t)n) != n)
                    ok = 0;
                else
                    off += (uint64_t)n;
            }
            /* what could be read stays, its owner's: a damaged block
               loses the rest of that file, not the file */
            if (to >= 0 && owner >= 0)
                tfs4_backend.set_owner(to, owner);
            if (ok) {
                mv.files++;
            } else {
                mv.lost++;
                printk("[ !! ] TFS4: %s could not be moved whole\n", path);
            }
        }
        path[len] = '\0';
    }
}

/* Steps 3 to 6, with the copy in memory: the partition made ours,
   formatted, filled. 1 done; 0 the partition not touched yet; -1 it
   was, and the move did not finish */
static int finish(uint32_t base, uint32_t sectors, uint32_t slot, int esp)
{
    /* the copy is read before anything is written: one that does not
       read is no reason to format */
    if (!tfs3_open_image(mv.mem, mv.bytes)) {
        printk("[ !! ] TFS4: the copy of TFS3 does not read\n");
        return 0;
    }
    tfs3_close();
    if (slot) {
        int n = part_scan();
        for (int i = 0; i < n; i++) {
            struct partition pt;
            if (part_get(i, &pt) && pt.slot == slot &&
                memcmp(pt.type, "THREADOS-TFS", 12) &&
                gpt_set_type(slot, gpt_type_threados()) < 0) {
                printk("[ !! ] TFS4: partition %u could not be made "
                       "ThreadOS's\n", slot);
                return 0;
            }
        }
    }
    printk("[ .. ] TFS4: formatting where TFS3 was\n");
    if (!tfs4_format(base, sectors) || !tfs4_mount()) {
        printk("[ !! ] TFS4: the partition could not be formatted\n");
        return -1;
    }
    tfs3_open_image(mv.mem, mv.bytes);
    static char path[FS_PATH_MAX];
    path[0] = '\0';
    mv.files = mv.dirs = mv.lost = 0;
    copy_tree(FS_ROOT, path, 0, 0);
    tfs3_close();
    if (esp)
        fat_unlink(PATH_NOTE);
    printk("[ OK ] TFS4: %u files and %u directories moved over from TFS3"
           "%s\n", (uint32_t)mv.files, (uint32_t)mv.dirs,
           esp ? " (the old disk: \\EFI\\ThreadOS\\tfs3-backup.img)" : "");
    if (mv.lost)
        printk("[ !! ] TFS4: %u could not be moved\n", (uint32_t)mv.lost);
    return 1;
}

int tfs3to4_migrate(const struct tfs3_where *w)
{
    printk("[ .. ] TFS4: this disk holds TFS3 (%s %u) - moving it to TFS4\n",
           w->partition ? "partition" : "the whole disk",
           (uint32_t)w->partition);
    uint32_t slot = 0;
    if (w->partition) {
        struct partition pt;
        if (!part_is_gpt() || !part_get(w->partition - 1, &pt)) {
            printk("[ !! ] TFS4 lives in a GPT partition: this TFS3 stays\n");
            return 0;
        }
        slot = pt.slot;
    }
    if (!mem_take(w->bytes)) {
        printk("[ !! ] TFS4: no memory for a copy of TFS3: it stays\n");
        return 0;
    }
    for (uint32_t done = 0; done < w->bytes;) {
        uint32_t n = w->bytes - done > 65536 ? 65536 : w->bytes - done;
        if (blk_read(w->base + done / 512, n / 512, mv.mem + done) < 0) {
            mem_give_back();
            printk("[ !! ] TFS4: TFS3 could not be read whole: it stays\n");
            return 0;
        }
        done += n;
    }
    int esp = esp_mount();
    if (esp) {
        char note[64], num[12];
        int at = 0;
        const uint32_t v[4] = { w->base, w->sectors, slot, w->bytes };
        for (int i = 0; i < 4; i++) {
            say_number(num, v[i]);
            for (int k = 0; num[k]; k++)
                note[at++] = num[k];
            note[at++] = i < 3 ? ' ' : '\n';
        }
        /* the copy before the note that points at it: a note is never
           found without its copy whole */
        if (fat_mkdir(DIR_OURS) < 0 ||
            fat_write_file(PATH_COPY, mv.mem, w->bytes) < 0 ||
            fat_write_file(PATH_NOTE, note, (uint32_t)at) < 0) {
            esp_done();
            mem_give_back();
            printk("[ !! ] TFS4: no room for a copy of TFS3 on the EFI "
                   "partition: it stays\n");
            return 0;
        }
        printk("[ OK ] TFS4: a copy of TFS3 kept on the EFI partition\n");
    } else {
        printk("[ !! ] TFS4: no EFI partition to keep a copy on - moving it "
               "from memory alone\n");
    }
    int r = finish(w->base, w->sectors, slot, esp);
    if (esp) {
        /* not begun: the TFS3 stays as it was, and no start after this
           one is to take the copy for a move cut short */
        if (r == 0)
            fat_unlink(PATH_NOTE);
        esp_done();
    }
    mem_give_back();
    return r;
}

int tfs3to4_resume(void)
{
    if (!blk_present() || !esp_mount())
        return 0;
    static char note[64];
    struct fat_entry fe;
    int n = fat_stat(PATH_NOTE, &fe) ? fat_read(PATH_NOTE, 0, note,
                                                sizeof(note) - 1) : -1;
    if (n <= 0) {
        esp_done();
        return 0;
    }
    note[n] = '\0';
    const char *p = note;
    uint32_t base = read_number(&p), sectors = read_number(&p);
    uint32_t slot = read_number(&p), bytes = read_number(&p);
    printk("[ .. ] TFS4: a move from TFS3 was cut short - finishing it from "
           "the copy\n");
    if (!bytes || bytes > (32u << 20) + 65536 || !mem_take(bytes) ||
        fat_read(PATH_COPY, 0, mv.mem, bytes) != (int)bytes) {
        if (mv.mem)
            mem_give_back();
        esp_done();
        printk("[ !! ] TFS4: the copy of TFS3 cannot be read\n");
        return 0;
    }
    int r = finish(base, sectors, slot, 1);
    esp_done();
    mem_give_back();
    return r;
}
