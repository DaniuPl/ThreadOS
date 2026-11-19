/* TFS4 - the ext2 format, implemented from its specification
 *
 * TFS4 is ThreadOS's file system and its bytes are ext2's (revision 1),
 * unchanged, so that e2fsprogs on any Linux can check every disk this
 * writes and Linux can mount it (docs/tfs4-plan.md). Written from the
 * format's documentation - the kernel.org "ext4 Data Structures and
 * Algorithms" pages and Dave Poirier's "The Second Extended File
 * System" - not from Linux's code.
 *
 * This is stage 2 of the plan: reading. The disk is split into block
 * groups, each with a block bitmap, an inode bitmap and a slice of the
 * inode table, described by the group descriptors after the
 * superblock. A file's blocks are listed in its inode: twelve direct,
 * then a block of pointers, a block of blocks of pointers and one more
 * level. A directory is a file of entries (inode, record length, name).
 *
 * The handle the rest of the kernel holds - a slot - is the inode
 * number, as stable as TFS3's slots were; the root, inode 2, is
 * FS_ROOT outside this file. Every number read from the disk is checked
 * against the file system's bounds before it is used: a disk is input
 * like any other, and t_fsbounds and the fuzz harness hold this to it.
 */

#include "include/tfs4.h"
#include "include/blk.h"
#include "include/part.h"
#include "include/heap.h"
#include "include/string.h"
#include "include/printk.h"
#include "include/io.h"
#include "include/user.h"
#include "../kernel64/task64.h"

#define EXT2_MAGIC       0xEF53
#define ROOT_INO         2

/* feature bits this reader understands */
#define INCOMPAT_FILETYPE  0x0002
#define INCOMPAT_KNOWN     INCOMPAT_FILETYPE
#define COMPAT_EXT_ATTR    0x0008

#define S_IFMT   0xF000
#define S_IFDIR  0x4000
#define S_IFREG  0x8000
#define S_IFLNK  0xA000

#define FT_DIR   2

#define SYMLINK_DEPTH 8

static struct {
    int mounted;
    uint32_t base;               /* the first sector of the file system */
    uint32_t bs;                 /* block size */
    uint32_t spb;                /* sectors per block */
    uint32_t blocks, inodes;
    uint32_t free_blocks;
    uint32_t first_data;
    uint32_t bpg, ipg;
    uint32_t isize;
    uint32_t groups;
    uint32_t incompat, ro_compat, compat;
    int filetype;                /* entries carry their file's type */
} fs;

struct gdesc {
    uint32_t block_bitmap, inode_bitmap, inode_table;
};
static struct gdesc *gd;

/* An inode as this file uses it */
struct inode {
    uint32_t num;
    uint16_t mode, links;
    uint32_t uid, gid;
    uint64_t size;
    uint32_t atime, ctime, mtime;
    uint32_t flags;
    uint32_t block[15];
    uint32_t file_acl;
    uint32_t blocks512;
};

static uint8_t bbuf[4096];       /* one block, for whoever reads one */

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}

/* --- blocks ---------------------------------------------------------- */

/* Only blocks of the file system: a pointer past its end on a damaged
   or hostile disk would read whatever lies behind it */
static int read_block(uint32_t b, void *buf)
{
    if (!b || b >= fs.blocks)
        return -1;
    return blk_read(fs.base + b * fs.spb, fs.spb, buf);
}

/* Pointer blocks are read over and over while a file is read in order;
   the last few are kept */
#define PCACHE 4
static struct {
    uint32_t block;
    uint32_t stamp;
    uint32_t ptr[1024];
} pcache[PCACHE];
static uint32_t pstamp;

static const uint32_t *pointers(uint32_t b)
{
    int victim = 0;
    for (int i = 0; i < PCACHE; i++) {
        if (pcache[i].block == b && b) {
            pcache[i].stamp = ++pstamp;
            return pcache[i].ptr;
        }
        if (pcache[i].stamp < pcache[victim].stamp)
            victim = i;
    }
    if (read_block(b, pcache[victim].ptr) < 0)
        return 0;
    pcache[victim].block = b;
    pcache[victim].stamp = ++pstamp;
    return pcache[victim].ptr;
}

static void pcache_drop(void)
{
    for (int i = 0; i < PCACHE; i++)
        pcache[i].block = pcache[i].stamp = 0;
}

/* --- inodes ---------------------------------------------------------- */

static int ino_valid(uint32_t n)
{
    return n >= 1 && n <= fs.inodes;
}

#define ICACHE 32
static struct {
    struct inode in;
    uint32_t stamp;
} icache[ICACHE];
static uint32_t istamp;

static int load_inode(uint32_t n, struct inode *out)
{
    if (!fs.mounted || !ino_valid(n))
        return -1;
    int victim = 0;
    for (int i = 0; i < ICACHE; i++) {
        if (icache[i].in.num == n) {
            icache[i].stamp = ++istamp;
            *out = icache[i].in;
            return 0;
        }
        if (icache[i].stamp < icache[victim].stamp)
            victim = i;
    }
    uint32_t g = (n - 1) / fs.ipg, idx = (n - 1) % fs.ipg;
    if (g >= fs.groups)
        return -1;
    uint64_t at = (uint64_t)idx * fs.isize;
    uint32_t b = gd[g].inode_table + (uint32_t)(at / fs.bs);
    if (b < gd[g].inode_table || read_block(b, bbuf) < 0)
        return -1;
    const uint8_t *p = bbuf + at % fs.bs;
    struct inode *in = &icache[victim].in;
    memset(in, 0, sizeof(*in));
    in->num = n;
    in->mode = rd16(p);
    in->uid = rd16(p + 2) | (uint32_t)rd16(p + 120) << 16;
    in->size = rd32(p + 4);
    in->atime = rd32(p + 8);
    in->ctime = rd32(p + 12);
    in->mtime = rd32(p + 16);
    in->gid = rd16(p + 24) | (uint32_t)rd16(p + 122) << 16;
    in->links = rd16(p + 26);
    in->blocks512 = rd32(p + 28);
    in->flags = rd32(p + 32);
    for (int i = 0; i < 15; i++)
        in->block[i] = rd32(p + 40 + 4 * i);
    in->file_acl = rd32(p + 104);
    /* the size's high half: large_file on a regular file */
    if ((in->mode & S_IFMT) == S_IFREG)
        in->size |= (uint64_t)rd32(p + 108) << 32;
    icache[victim].stamp = ++istamp;
    *out = *in;
    return 0;
}

static int is_dir_mode(const struct inode *in)
{
    return (in->mode & S_IFMT) == S_IFDIR;
}

/* The disk block under a file's n-th block: 0 for a hole, or for a
   pointer that is not the file system's */
static uint32_t bmap(const struct inode *in, uint64_t n)
{
    uint32_t per = fs.bs / 4;
    if (n < 12)
        return in->block[n] < fs.blocks ? in->block[n] : 0;
    n -= 12;
    const uint32_t *p;
    if (n < per) {
        p = pointers(in->block[12]);
        return p && p[n] < fs.blocks ? p[n] : 0;
    }
    n -= per;
    if (n < (uint64_t)per * per) {
        p = pointers(in->block[13]);
        if (!p)
            return 0;
        p = pointers(p[n / per]);
        return p && p[n % per] < fs.blocks ? p[n % per] : 0;
    }
    n -= (uint64_t)per * per;
    if (n < (uint64_t)per * per * per) {
        p = pointers(in->block[14]);
        if (!p)
            return 0;
        p = pointers(p[n / per / per]);
        if (!p)
            return 0;
        p = pointers(p[(n / per) % per]);
        return p && p[n % per] < fs.blocks ? p[n % per] : 0;
    }
    return 0;
}

/* A file's bytes: holes read as zeros */
static int read_bytes(const struct inode *in, uint64_t off, void *buf,
                      uint32_t len)
{
    if (off >= in->size)
        return 0;
    if (len > in->size - off)
        len = (uint32_t)(in->size - off);
    uint8_t *dst = buf;
    uint32_t done = 0;
    while (done < len) {
        uint64_t pos = off + done;
        uint32_t boff = (uint32_t)(pos % fs.bs);
        uint32_t chunk = fs.bs - boff;
        if (chunk > len - done)
            chunk = len - done;
        uint32_t b = bmap(in, pos / fs.bs);
        if (!b) {
            memset(dst + done, 0, chunk);
        } else {
            if (read_block(b, bbuf) < 0)
                return -1;
            memcpy(dst + done, bbuf + boff, chunk);
        }
        done += chunk;
    }
    return (int)done;
}

/* --- rights -------------------------------------------------------------
 * Until stage 4 brings the whole of rwx, a file is readable by its
 * owner, an administrator, or anybody when its "others" bit says so -
 * which is what a disk made by mke2fs -d says for its ordinary files. */

static int may_read(const struct inode *in)
{
    if (task64_level() == USER_ADMIN)
        return 1;
    if (in->uid == (uint32_t)task64_uid())
        return 1;
    return (in->mode & 0004) != 0;
}

static int may_enter(const struct inode *in)
{
    if (task64_level() == USER_ADMIN ||
        in->uid == (uint32_t)task64_uid())
        return 1;
    return (in->mode & 0001) != 0;
}

/* --- directories -------------------------------------------------------- */

struct dentry {
    uint32_t ino;
    uint8_t type;
    uint32_t name_len;
    char name[FS_NAME_LEN + 1];
};

/* The entry at byte *off of a directory, skipping empty ones: 1 and
   *off moved past it, 0 at the end, -1 for an entry that does not
   hold together (a record length of 0, running past its block, a name
   longer than its record) - the rest of a damaged directory is not
   read rather than read wrongly */
static uint8_t dblk[4096];       /* the directory block last read */
static uint32_t dblk_no, dblk_of;

static int dir_next(const struct inode *dir, uint64_t *off, struct dentry *e)
{
    uint8_t *blk = dblk;
    while (*off < dir->size) {
        uint64_t pos = *off;
        uint32_t boff = (uint32_t)(pos % fs.bs);
        uint32_t b = bmap(dir, pos / fs.bs);
        if (!b)
            return -1;                 /* a directory has no holes */
        if (dblk_no != b || dblk_of != dir->num) {
            dblk_no = 0;
            if (read_block(b, blk) < 0)
                return -1;
            dblk_no = b;
            dblk_of = dir->num;
        }
        if (boff + 8 > fs.bs)
            return -1;
        const uint8_t *p = blk + boff;
        uint32_t ino = rd32(p);
        uint32_t rec = rd16(p + 4);
        uint32_t nl = fs.filetype ? p[6] : rd16(p + 6);
        if (rec < 8 || rec % 4 || boff + rec > fs.bs || nl + 8 > rec ||
            nl > FS_NAME_LEN)
            return -1;
        *off = pos + rec;
        if (!ino || !nl)
            continue;                  /* an empty slot (or an htree node) */
        if (!ino_valid(ino))
            return -1;
        e->ino = ino;
        e->type = fs.filetype ? p[7] : 0;
        e->name_len = nl;
        memcpy(e->name, p + 8, nl);
        e->name[nl] = '\0';
        return 1;
    }
    return 0;
}

/* The inode a directory names 'name' (len bytes): 0 when none */
static uint32_t dir_lookup(const struct inode *dir, const char *name,
                           uint32_t len)
{
    static struct dentry e;
    uint64_t off = 0;
    while (dir_next(dir, &off, &e) == 1)
        if (e.name_len == len && !memcmp(e.name, name, len))
            return e.ino;
    return 0;
}

/* --- paths --------------------------------------------------------------- */

static uint32_t ino_of_slot(int slot)
{
    return slot == FS_ROOT ? ROOT_INO : (uint32_t)slot;
}

static int slot_of_ino(uint32_t ino)
{
    return ino == ROOT_INO ? FS_ROOT : (int)ino;
}

/* Where a symbolic link points: its target into out (cap bytes) */
static int link_target(const struct inode *in, char *out, uint32_t cap)
{
    if (in->size >= cap)
        return -1;
    uint32_t n = (uint32_t)in->size;
    /* a short one lives in the inode's block list itself; the attribute
       block, if any, is counted in i_blocks without being the link's */
    uint32_t own = in->blocks512 -
                   (in->file_acl ? fs.bs / 512 : 0);
    if (n < 60 && own == 0) {
        memcpy(out, in->block, n);
    } else if (read_bytes(in, 0, out, n) != (int)n) {
        return -1;
    }
    out[n] = '\0';
    return (int)n;
}

static uint32_t resolve_in(uint32_t start, const char *path, uint32_t n,
                           int follow_last, int depth);

/* Resolves the first n bytes of a path from directory 'start' (an inode)
   to an inode, or 0. Symbolic links on the way are followed (no more
   than SYMLINK_DEPTH deep); the last one only when follow_last. */
static uint32_t resolve_in(uint32_t start, const char *path, uint32_t n,
                           int follow_last, int depth)
{
    uint32_t cur = (n && path[0] == '/') ? ROOT_INO : start;
    const char *p = path, *end = path + n;
    static struct inode in;
    while (p < end) {
        while (p < end && *p == '/')
            p++;
        if (p == end)
            break;
        const char *s = p;
        while (p < end && *p != '/')
            p++;
        uint32_t len = (uint32_t)(p - s);
        if (len == 1 && s[0] == '.')
            continue;
        if (load_inode(cur, &in) < 0 || !is_dir_mode(&in) || !may_enter(&in))
            return 0;
        if (len > FS_NAME_LEN)
            return 0;
        uint32_t next = len == 2 && s[0] == '.' && s[1] == '.'
                            ? (cur == ROOT_INO ? ROOT_INO
                                               : dir_lookup(&in, "..", 2))
                            : dir_lookup(&in, s, len);
        if (!next || load_inode(next, &in) < 0)
            return 0;
        int last = p == end || (p < end && p + 1 == end && *p == '/');
        if ((in.mode & S_IFMT) == S_IFLNK && (!last || follow_last)) {
            if (depth >= SYMLINK_DEPTH)
                return 0;
            char *target = kmalloc(FS_PATH_MAX);
            if (!target)
                return 0;
            int tl = link_target(&in, target, FS_PATH_MAX);
            uint32_t to = tl > 0 ? resolve_in(cur, target, (uint32_t)tl, 1,
                                              depth + 1) : 0;
            kfree(target);
            if (!to)
                return 0;
            next = to;
        }
        cur = next;
    }
    return cur;
}

static uint32_t resolve(int cwd, const char *path)
{
    return resolve_in(ino_of_slot(cwd), path, (uint32_t)strlen(path), 1, 0);
}

/* --- the backend's calls --------------------------------------------------- */

static int t4_open(int cwd, const char *path, int create)
{
    (void)create;                      /* read-only: nothing is made */
    irqflags_t flags = irq_save();
    static struct inode in;
    uint32_t ino = resolve(cwd, path);
    int r = -1;
    if (ino && load_inode(ino, &in) == 0 &&
        (in.mode & S_IFMT) == S_IFREG && may_read(&in))
        r = slot_of_ino(ino);
    irq_restore(flags);
    return r;
}

static int t4_refuse_path(int cwd, const char *path)
{
    (void)cwd;
    (void)path;
    return -1;                         /* read-only until stage 3 */
}

static int t4_rename(int cwd, const char *from, const char *to)
{
    (void)cwd;
    (void)from;
    (void)to;
    return -1;
}

static int t4_chdir(int cwd, const char *path, int *newcwd)
{
    irqflags_t flags = irq_save();
    static struct inode in;
    uint32_t ino = resolve(cwd, path);
    int ok = ino && load_inode(ino, &in) == 0 && is_dir_mode(&in) &&
             may_enter(&in);
    if (ok)
        *newcwd = slot_of_ino(ino);
    irq_restore(flags);
    return ok ? 0 : -1;
}

/* The path of a directory: up through "..", finding each one's name in
   its parent */
static int t4_getpath(int slot, char *buf, uint32_t cap)
{
    irqflags_t flags = irq_save();
    static struct inode in, parent;
    static struct dentry e;
    uint32_t ino = ino_of_slot(slot);
    uint32_t end = cap;                 /* the path is built from the end */
    if (cap < 2)
        goto fail;
    buf[--end] = '\0';
    for (int depth = 0; ino != ROOT_INO; depth++) {
        if (depth > 4096 || load_inode(ino, &in) < 0 || !is_dir_mode(&in))
            goto fail;
        uint32_t up = dir_lookup(&in, "..", 2);
        if (!up || load_inode(up, &parent) < 0)
            goto fail;
        uint64_t off = 0;
        int found = 0;
        while (dir_next(&parent, &off, &e) == 1) {
            int dots = (e.name_len == 1 && e.name[0] == '.') ||
                       (e.name_len == 2 && e.name[0] == '.' &&
                        e.name[1] == '.');
            if (e.ino == ino && !dots) {
                found = 1;
                break;
            }
        }
        if (!found || e.name_len + 1 > end)
            goto fail;
        end -= e.name_len;
        memcpy(buf + end, e.name, e.name_len);
        buf[--end] = '/';
        ino = up;
    }
    if (end == cap - 1)
        buf[--end] = '/';
    uint32_t len = cap - 1 - end;
    for (uint32_t i = 0; i <= len; i++)  /* forward: the path moves left */
        buf[i] = buf[end + i];
    irq_restore(flags);
    return (int)len;
fail:
    buf[0] = '/';
    buf[1] = '\0';
    irq_restore(flags);
    return -1;
}

static void fill(const struct inode *in, struct fs_stat *st)
{
    memset(st, 0, sizeof(*st));
    st->slot = slot_of_ino(in->num);
    st->ino = in->num;
    st->type = is_dir_mode(in) ? FS_T_DIR : FS_T_FILE;
    st->size = is_dir_mode(in) ? 0 : in->size;
    st->owner = (int)in->uid;
}

static int t4_stat_path(int cwd, const char *path, struct fs_stat *st)
{
    irqflags_t flags = irq_save();
    static struct inode in;
    uint32_t ino = resolve(cwd, path);
    int ok = ino && load_inode(ino, &in) == 0 &&
             (is_dir_mode(&in) || may_read(&in));
    if (ok)
        fill(&in, st);
    irq_restore(flags);
    return ok ? 0 : -1;
}

static int t4_stat_slot(int slot, struct fs_stat *st)
{
    irqflags_t flags = irq_save();
    static struct inode in;
    int ok = load_inode(ino_of_slot(slot), &in) == 0;
    if (ok)
        fill(&in, st);
    irq_restore(flags);
    return ok ? 0 : -1;
}

/* The cursor is the byte offset in the directory of the next entry */
static int t4_readdir_at(int dir, uint64_t *cursor, struct fs_dirent *out)
{
    irqflags_t flags = irq_save();
    static struct inode in, child;
    static struct dentry e;
    int r = -1;
    if (load_inode(ino_of_slot(dir), &in) < 0 || !is_dir_mode(&in) ||
        !may_read(&in))
        goto done;
    for (;;) {
        r = dir_next(&in, cursor, &e);
        if (r != 1)
            break;
        if ((e.name_len == 1 && e.name[0] == '.') ||
            (e.name_len == 2 && e.name[0] == '.' && e.name[1] == '.'))
            continue;
        memset(out, 0, sizeof(*out));
        memcpy(out->name, e.name, e.name_len + 1);
        out->ino = e.ino;
        out->slot = slot_of_ino(e.ino);
        if (load_inode(e.ino, &child) == 0) {
            out->is_dir = is_dir_mode(&child);
            out->size = out->is_dir ? 0 : child.size;
        } else {
            out->is_dir = e.type == FT_DIR;
        }
        break;
    }
    if (r < 0)
        r = 0;                         /* what could be read was */
done:
    irq_restore(flags);
    return r;
}

static int64_t t4_size(int slot)
{
    static struct inode in;
    irqflags_t flags = irq_save();
    int64_t r = load_inode(ino_of_slot(slot), &in) == 0 && may_read(&in)
                    ? (int64_t)in.size : -1;
    irq_restore(flags);
    return r;
}

static int t4_owner(int slot)
{
    static struct inode in;
    irqflags_t flags = irq_save();
    int r = load_inode(ino_of_slot(slot), &in) == 0 ? (int)in.uid : -1;
    irq_restore(flags);
    return r;
}

static int t4_no(int slot)
{
    (void)slot;
    return 0;                          /* nobody writes yet */
}

static int t4_set_owner(int slot, int uid)
{
    (void)slot;
    (void)uid;
    return -1;
}

static int t4_truncate_to(int slot, uint64_t size)
{
    (void)slot;
    (void)size;
    return -1;
}

static void t4_slot_closed(int slot)
{
    (void)slot;
}

static int t4_read_at(int slot, uint64_t off, void *buf, uint32_t len)
{
    static struct inode in;
    irqflags_t flags = irq_save();
    int r = -1;
    if (load_inode(ino_of_slot(slot), &in) == 0 &&
        (in.mode & S_IFMT) == S_IFREG && may_read(&in))
        r = read_bytes(&in, off, buf, len);
    irq_restore(flags);
    return r;
}

static int t4_write_at(int slot, uint64_t off, const void *buf, uint32_t len)
{
    (void)slot;
    (void)off;
    (void)buf;
    (void)len;
    return -1;
}

static uint32_t t4_file_block(int slot, uint32_t index, int alloc)
{
    static struct inode in;
    if (alloc)
        return 0;
    irqflags_t flags = irq_save();
    uint32_t b = load_inode(ino_of_slot(slot), &in) == 0 && may_read(&in)
                     ? bmap(&in, index) : 0;
    irq_restore(flags);
    return b;
}

static uint32_t t4_block_lba(uint32_t block)
{
    return fs.base + block * fs.spb;
}

static uint32_t t4_block_size(void)
{
    return fs.bs;
}

static void t4_usage(uint64_t *used, uint64_t *total)
{
    if (used)
        *used = fs.blocks - fs.free_blocks;
    if (total)
        *total = fs.blocks;
}

const struct fs_backend tfs4_backend = {
    .name = "TFS4",
    .open = t4_open,
    .mkdir = t4_refuse_path,
    .chdir = t4_chdir,
    .unlink = t4_refuse_path,
    .rename = t4_rename,
    .getpath = t4_getpath,
    .stat_path = t4_stat_path,
    .stat_slot = t4_stat_slot,
    .readdir_at = t4_readdir_at,
    .size = t4_size,
    .owner = t4_owner,
    .can_write = t4_no,
    .set_owner = t4_set_owner,
    .truncate_to = t4_truncate_to,
    .slot_closed = t4_slot_closed,
    .read_at = t4_read_at,
    .write_at = t4_write_at,
    .file_block = t4_file_block,
    .block_lba = t4_block_lba,
    .block_size = t4_block_size,
    .usage = t4_usage,
};

/* --- mounting ---------------------------------------------------------------- */

/* The superblock of an ext2 file system starting at sector 'base', of
   at most 'sectors' sectors: 1 when it is one this reader takes, with
   fs filled in; 0 when it is not ext2 at all or not ours to read
   (*why says which) */
static int take(uint32_t base, uint32_t sectors, const char **why)
{
    static uint8_t sb[1024];
    *why = "";
    if (blk_read(base + 2, 2, sb) < 0)
        return 0;
    if (rd16(sb + 56) != EXT2_MAGIC) {
        *why = "no ext2 superblock";
        return 0;
    }
    uint32_t log = rd32(sb + 24);
    uint32_t rev = rd32(sb + 76);
    memset(&fs, 0, sizeof(fs));
    fs.base = base;
    fs.inodes = rd32(sb);
    fs.blocks = rd32(sb + 4);
    fs.free_blocks = rd32(sb + 12);
    fs.first_data = rd32(sb + 20);
    fs.bpg = rd32(sb + 32);
    fs.ipg = rd32(sb + 40);
    fs.isize = rev >= 1 ? rd16(sb + 88) : 128;
    fs.compat = rev >= 1 ? rd32(sb + 92) : 0;
    fs.incompat = rev >= 1 ? rd32(sb + 96) : 0;
    fs.ro_compat = rev >= 1 ? rd32(sb + 100) : 0;
    if (log > 2) {
        *why = "blocks larger than 4 KiB";
        return 0;
    }
    fs.bs = 1024u << log;
    fs.spb = fs.bs / 512;
    if (fs.incompat & ~INCOMPAT_KNOWN) {
        *why = "a feature this reader does not know (ext3's journal or "
               "ext4's extents?)";
        return 0;
    }
    if (!fs.blocks || !fs.inodes || !fs.bpg || !fs.ipg ||
        fs.bpg > fs.bs * 8 || fs.ipg > fs.bs * 8 ||
        fs.isize < 128 || fs.isize > fs.bs || (fs.isize & (fs.isize - 1)) ||
        fs.first_data != (fs.bs == 1024 ? 1u : 0u) ||
        fs.free_blocks > fs.blocks ||
        (uint64_t)fs.blocks * fs.spb > sectors) {
        *why = "a superblock that does not hold together";
        return 0;
    }
    fs.groups = (fs.blocks - fs.first_data + fs.bpg - 1) / fs.bpg;
    if ((uint64_t)fs.groups * fs.ipg < fs.inodes) {
        *why = "more inodes than its groups hold";
        return 0;
    }
    fs.filetype = (fs.incompat & INCOMPAT_FILETYPE) != 0;

    /* the group descriptors, in the blocks after the superblock's */
    uint32_t gdt = fs.first_data + 1;
    uint32_t per = fs.bs / 32;
    kfree(gd);
    gd = kmalloc(fs.groups * sizeof(*gd));
    if (!gd) {
        *why = "no memory for its group descriptors";
        return 0;
    }
    for (uint32_t g = 0; g < fs.groups; g++) {
        if (g % per == 0 && read_block(gdt + g / per, bbuf) < 0) {
            *why = "its group descriptors cannot be read";
            return 0;
        }
        const uint8_t *d = bbuf + (g % per) * 32;
        gd[g].block_bitmap = rd32(d);
        gd[g].inode_bitmap = rd32(d + 4);
        gd[g].inode_table = rd32(d + 8);
        uint32_t table_blocks = (fs.ipg * fs.isize + fs.bs - 1) / fs.bs;
        if (gd[g].block_bitmap >= fs.blocks ||
            gd[g].inode_bitmap >= fs.blocks || !gd[g].inode_table ||
            gd[g].inode_table >= fs.blocks ||
            fs.blocks - gd[g].inode_table < table_blocks) {
            *why = "a group descriptor pointing outside the file system";
            return 0;
        }
    }
    memset(icache, 0, sizeof(icache));
    pcache_drop();
    dblk_no = 0;
    fs.mounted = 1;
    static struct inode root;
    if (load_inode(ROOT_INO, &root) < 0 || !is_dir_mode(&root)) {
        fs.mounted = 0;
        *why = "its root is not a directory";
        return 0;
    }
    return 1;
}

/* A file system on the whole disk is ours when its label says
   ThreadOS: plain ext2 on a disk is somebody's Linux */
static int labelled_ours(uint32_t base)
{
    static uint8_t sb[1024];
    if (blk_read(base + 2, 2, sb) < 0 || rd16(sb + 56) != EXT2_MAGIC)
        return 0;
    return !memcmp(sb + 120, "ThreadOS", 9);
}

/* ThreadOS's partition type: the bytes spell THREADOS-TFS (kernel/gpt.c) */
static int our_type(const uint8_t *type)
{
    return !memcmp(type, "THREADOS-TFS", 12);
}

int tfs4_mount(void)
{
    const char *why;
    fs.mounted = 0;
    if (!blk_present())
        return 0;
    int n = part_scan();
    if (n <= 0) {
        if (!labelled_ours(0))
            return 0;
        if (!take(0, blk_sectors(), &why)) {
            printk("[ !! ] TFS4: the disk is labelled ThreadOS, but %s\n", why);
            return 0;
        }
    } else {
        int found = 0;
        for (int i = 0; i < n && !found; i++) {
            struct partition pt;
            if (!part_get(i, &pt) || !our_type(pt.type) ||
                pt.start > 0xFFFFFFFFu || pt.sectors > 0xFFFFFFFFu)
                continue;
            if (take((uint32_t)pt.start, (uint32_t)pt.sectors, &why))
                found = 1;
            else if (why[0] && strcmp(why, "no ext2 superblock"))
                printk("[ !! ] TFS4: partition %u is ThreadOS's, but %s\n",
                       (uint32_t)(i + 1), why);
        }
        if (!found)
            return 0;
    }
    printk("[ OK ] TFS4: %u blocks of %u B, %u inodes, from sector %u "
           "(read-only)\n", fs.blocks, fs.bs, fs.inodes, fs.base);
    return 1;
}
