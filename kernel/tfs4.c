/* TFS4 - the ext2 format, implemented from its specification
 *
 * TFS4 is ThreadOS's file system and its bytes are ext2's (revision 1),
 * unchanged, so that e2fsprogs on any Linux can check every disk this
 * writes and Linux can mount it (docs/tfs4-plan.md). Written from the
 * format's documentation - the kernel.org "ext4 Data Structures and
 * Algorithms" pages and Dave Poirier's "The Second Extended File
 * System" - not from Linux's code.
 *
 * The disk is split into block groups, each with a block bitmap, an
 * inode bitmap and a slice of the inode table, described by the group
 * descriptors after the superblock. A file's blocks are listed in its
 * inode: twelve direct, then a block of pointers, a block of blocks of
 * pointers and one more level. A directory is a file of entries (inode,
 * record length, name).
 *
 * The handle the rest of the kernel holds - a slot - is the inode
 * number, as stable as TFS3's slots were; the root, inode 2, is
 * FS_ROOT outside this file. Every number read from the disk is checked
 * against the file system's bounds before it is used: a disk is input
 * like any other, and t_fsbounds and the fuzz harness hold this to it.
 *
 * Writing (stage 3) has no journal yet (stage 7), so the order of the
 * writes is what keeps a crash from corrupting: a new file's inode is
 * written before the entry that names it, an entry goes before the
 * inode it named is freed - a crash leaks space rather than leaving a
 * name pointing at nothing. Every change reaches the disk before the
 * call that made it returns (stage 6 brings a cache). Rights, until
 * stage 4 brings rwx, are TFS3's: a file is its owner's and the
 * administrator's, the root directory is everybody's to write in.
 */

#include "include/tfs4.h"
#include "include/blk.h"
#include "include/part.h"
#include "include/heap.h"
#include "include/string.h"
#include "include/printk.h"
#include "include/io.h"
#include "include/user.h"
#include "include/rtc.h"
#include "include/random.h"
#include "../kernel64/task64.h"

#define EXT2_MAGIC       0xEF53
#define ROOT_INO         2
#define LOST_FOUND_INO   11
#define FIRST_INO        11

/* feature bits */
#define COMPAT_EXT_ATTR      0x0008
#define COMPAT_HAS_JOURNAL   0x0004
#define INCOMPAT_FILETYPE    0x0002
#define INCOMPAT_KNOWN       INCOMPAT_FILETYPE
#define RO_SPARSE_SUPER      0x0001
#define RO_LARGE_FILE        0x0002
#define RO_WRITABLE          (RO_SPARSE_SUPER | RO_LARGE_FILE)

#define STATE_VALID      0x0001          /* unmounted cleanly */
#define INDEX_FL         0x00001000      /* an htree directory */

#define S_IFMT   0xF000
#define S_IFDIR  0x4000
#define S_IFREG  0x8000
#define S_IFLNK  0xA000

#define FT_REG   1
#define FT_DIR   2

#define SYMLINK_DEPTH 8

static struct {
    int mounted;
    int writable;
    uint32_t base;               /* the first sector of the file system */
    uint32_t bs;                 /* block size */
    uint32_t spb;                /* sectors per block */
    uint32_t blocks, inodes;
    uint32_t free_blocks, free_inodes;
    uint32_t first_data;
    uint32_t bpg, ipg;
    uint32_t isize;
    uint32_t first_ino;
    uint32_t groups;
    uint32_t incompat, ro_compat, compat;
    int filetype;                /* entries carry their file's type */
    uint32_t last_orphan;
    int sb_dirty;
} fs;

struct gdesc {
    uint32_t block_bitmap, inode_bitmap, inode_table;
    uint32_t free_blocks, free_inodes, used_dirs;
    int dirty;
};
static struct gdesc *gd;

/* An inode as this file uses it */
struct inode {
    uint32_t num;
    uint16_t mode, links;
    uint32_t uid, gid;
    uint64_t size;
    uint32_t atime, ctime, mtime, dtime;
    uint32_t flags;
    uint32_t block[15];
    uint32_t file_acl;
    uint32_t blocks512;
};

static uint8_t bbuf[4096];       /* one block, for whoever reads one */
static uint8_t ibuf[4096];       /* the inode table block being changed */

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}
static void wr16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* Seconds since 1970 from the CMOS clock: the days before this year's
   date (the proleptic Gregorian calendar, March-based so leap days come
   last), then the time of day */
static uint32_t now(void)
{
    struct rtc_time t;
    rtc_read(&t);
    int y = t.year, m = t.month, d = t.day;
    if (y < 1970 || m < 1 || m > 12 || d < 1)
        return 0;
    y -= m <= 2;
    int era = y / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = (int64_t)era * 146097 + doe - 719468;
    return (uint32_t)(days * 86400 + t.hour * 3600 + t.min * 60 + t.sec);
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

static int write_block(uint32_t b, const void *buf)
{
    if (!fs.writable || !b || b >= fs.blocks)
        return -1;
    return blk_write(fs.base + b * fs.spb, fs.spb, buf);
}

static int zero_block(uint32_t b)
{
    static uint8_t zeros[4096];
    return write_block(b, zeros);
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

static void pcache_forget(uint32_t b)
{
    for (int i = 0; i < PCACHE; i++)
        if (pcache[i].block == b)
            pcache[i].block = pcache[i].stamp = 0;
}

static void pcache_drop(void)
{
    for (int i = 0; i < PCACHE; i++)
        pcache[i].block = pcache[i].stamp = 0;
}

/* One pointer of a pointer block set, the block written */
static int ptr_set(uint32_t b, uint32_t i, uint32_t v)
{
    const uint32_t *p = pointers(b);
    if (!p)
        return -1;
    uint32_t *w = (uint32_t *)p;     /* the cached copy is ours to change */
    w[i] = v;
    if (write_block(b, w) < 0) {
        pcache_forget(b);
        return -1;
    }
    return 0;
}

/* --- the bookkeeping: superblock, group descriptors, bitmaps ---------- */

static uint32_t group_blocks(uint32_t g)
{
    return g == fs.groups - 1 ? fs.blocks - fs.first_data - g * fs.bpg
                              : fs.bpg;
}

/* One block bitmap and one inode bitmap are kept; changing groups
   writes back the one that was changed */
static struct {
    uint8_t bits[4096];
    uint32_t group;
    int loaded, dirty;
} bmap_b, bmap_i;

static int bitmap_flush(void)
{
    int r = 0;
    if (bmap_b.dirty &&
        write_block(gd[bmap_b.group].block_bitmap, bmap_b.bits) < 0)
        r = -1;
    if (bmap_i.dirty &&
        write_block(gd[bmap_i.group].inode_bitmap, bmap_i.bits) < 0)
        r = -1;
    bmap_b.dirty = bmap_i.dirty = 0;
    return r;
}

static uint8_t *bitmap_of(int inodes, uint32_t g)
{
    __typeof__(bmap_b) *m = inodes ? &bmap_i : &bmap_b;
    if (m->loaded && m->group == g)
        return m->bits;
    if (m->dirty) {
        uint32_t b = inodes ? gd[m->group].inode_bitmap
                            : gd[m->group].block_bitmap;
        if (write_block(b, m->bits) < 0)
            return 0;
        m->dirty = 0;
    }
    m->loaded = 0;
    if (read_block(inodes ? gd[g].inode_bitmap : gd[g].block_bitmap,
                   m->bits) < 0)
        return 0;
    m->loaded = 1;
    m->group = g;
    return m->bits;
}

static void bitmap_dirty(int inodes)
{
    (inodes ? &bmap_i : &bmap_b)->dirty = 1;
}

/* The counts in the group descriptors and the superblock, after every
   call that changed them */
static int meta_flush(void)
{
    int r = bitmap_flush();
    uint32_t per = fs.bs / 32;
    uint32_t gdt = fs.first_data + 1;
    for (uint32_t g = 0; g < fs.groups; g++) {
        if (!gd[g].dirty)
            continue;
        uint32_t blk = gdt + g / per;
        if (read_block(blk, bbuf) < 0) {
            r = -1;
            continue;
        }
        /* every group this block describes, in one write */
        for (uint32_t h = (g / per) * per; h < fs.groups &&
                                           h < (g / per + 1) * per; h++) {
            uint8_t *d = bbuf + (h % per) * 32;
            wr16(d + 12, gd[h].free_blocks);
            wr16(d + 14, gd[h].free_inodes);
            wr16(d + 16, gd[h].used_dirs);
            gd[h].dirty = 0;
        }
        if (write_block(blk, bbuf) < 0)
            r = -1;
    }
    if (fs.sb_dirty) {
        static uint8_t sb[1024];
        if (blk_read(fs.base + 2, 2, sb) == 0) {
            wr32(sb + 12, fs.free_blocks);
            wr32(sb + 16, fs.free_inodes);
            wr32(sb + 48, now());
            wr32(sb + 232, fs.last_orphan);
            if (blk_write(fs.base + 2, 2, sb) < 0)
                r = -1;
        } else {
            r = -1;
        }
        fs.sb_dirty = 0;
    }
    return r;
}

/* A free block, near 'goal' when it can be: 0 when the disk is full */
static uint32_t alloc_block(uint32_t goal)
{
    if (!fs.free_blocks)
        return 0;
    if (goal < fs.first_data || goal >= fs.blocks)
        goal = fs.first_data;
    uint32_t g0 = (goal - fs.first_data) / fs.bpg;
    for (uint32_t k = 0; k < fs.groups; k++) {
        uint32_t g = (g0 + k) % fs.groups;
        if (!gd[g].free_blocks)
            continue;
        uint8_t *bits = bitmap_of(0, g);
        if (!bits)
            continue;
        uint32_t n = group_blocks(g);
        uint32_t start = k == 0 ? (goal - fs.first_data) % fs.bpg : 0;
        for (uint32_t s = 0; s < n; s++) {
            uint32_t i = (start + s) % n;
            if (bits[i / 8] & (1u << (i % 8)))
                continue;
            bits[i / 8] |= (uint8_t)(1u << (i % 8));
            bitmap_dirty(0);
            gd[g].free_blocks--;
            gd[g].dirty = 1;
            fs.free_blocks--;
            fs.sb_dirty = 1;
            return fs.first_data + g * fs.bpg + i;
        }
    }
    return 0;
}

static void free_block(uint32_t b)
{
    if (b < fs.first_data || b >= fs.blocks)
        return;
    uint32_t g = (b - fs.first_data) / fs.bpg, i = (b - fs.first_data) % fs.bpg;
    uint8_t *bits = bitmap_of(0, g);
    if (!bits || !(bits[i / 8] & (1u << (i % 8))))
        return;                        /* not ours to free twice */
    bits[i / 8] &= (uint8_t)~(1u << (i % 8));
    bitmap_dirty(0);
    gd[g].free_blocks++;
    gd[g].dirty = 1;
    fs.free_blocks++;
    fs.sb_dirty = 1;
    pcache_forget(b);
}

/* A free inode. A directory goes to a group with fewer directories than
   most and inodes to spare, so the tree spreads over the disk; a file
   stays in its directory's group, where its blocks will be. 0 when
   there is none. */
static uint32_t alloc_inode(uint32_t near, int dir)
{
    if (!fs.free_inodes)
        return 0;
    uint32_t g0 = near ? (near - 1) / fs.ipg : 0;
    if (dir) {
        uint32_t avg = fs.free_inodes / fs.groups, best = fs.groups;
        for (uint32_t g = 0; g < fs.groups; g++)
            if (gd[g].free_inodes && gd[g].free_inodes >= avg &&
                (best == fs.groups || gd[g].used_dirs < gd[best].used_dirs))
                best = g;
        if (best < fs.groups)
            g0 = best;
    }
    for (uint32_t k = 0; k < fs.groups; k++) {
        uint32_t g = (g0 + k) % fs.groups;
        if (!gd[g].free_inodes)
            continue;
        uint8_t *bits = bitmap_of(1, g);
        if (!bits)
            continue;
        for (uint32_t i = 0; i < fs.ipg; i++) {
            uint32_t ino = g * fs.ipg + i + 1;
            if (ino < fs.first_ino || (bits[i / 8] & (1u << (i % 8))))
                continue;
            bits[i / 8] |= (uint8_t)(1u << (i % 8));
            bitmap_dirty(1);
            gd[g].free_inodes--;
            if (dir)
                gd[g].used_dirs++;
            gd[g].dirty = 1;
            fs.free_inodes--;
            fs.sb_dirty = 1;
            return ino;
        }
    }
    return 0;
}

static void free_inode_bit(uint32_t ino, int dir)
{
    uint32_t g = (ino - 1) / fs.ipg, i = (ino - 1) % fs.ipg;
    uint8_t *bits = bitmap_of(1, g);
    if (!bits || !(bits[i / 8] & (1u << (i % 8))))
        return;
    bits[i / 8] &= (uint8_t)~(1u << (i % 8));
    bitmap_dirty(1);
    gd[g].free_inodes++;
    if (dir && gd[g].used_dirs)
        gd[g].used_dirs--;
    gd[g].dirty = 1;
    fs.free_inodes++;
    fs.sb_dirty = 1;
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

/* Where an inode lives: its table block and the offset in it */
static int inode_place(uint32_t n, uint32_t *blk, uint32_t *off)
{
    uint32_t g = (n - 1) / fs.ipg, idx = (n - 1) % fs.ipg;
    if (g >= fs.groups)
        return -1;
    uint64_t at = (uint64_t)idx * fs.isize;
    *blk = gd[g].inode_table + (uint32_t)(at / fs.bs);
    *off = (uint32_t)(at % fs.bs);
    return *blk < gd[g].inode_table ? -1 : 0;
}

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
    uint32_t b, off;
    if (inode_place(n, &b, &off) < 0 || read_block(b, bbuf) < 0)
        return -1;
    const uint8_t *p = bbuf + off;
    struct inode *in = &icache[victim].in;
    memset(in, 0, sizeof(*in));
    in->num = n;
    in->mode = rd16(p);
    in->uid = rd16(p + 2) | (uint32_t)rd16(p + 120) << 16;
    in->size = rd32(p + 4);
    in->atime = rd32(p + 8);
    in->ctime = rd32(p + 12);
    in->mtime = rd32(p + 16);
    in->dtime = rd32(p + 20);
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

/* An inode back into its table. 'fresh' clears the whole record first,
   as for an inode just taken: the bytes past the 128 of revision 0 then
   say how many of them are in use, 32, as mke2fs writes them. */
static int write_inode(const struct inode *in, int fresh)
{
    uint32_t b, off;
    if (inode_place(in->num, &b, &off) < 0 || read_block(b, ibuf) < 0)
        return -1;
    uint8_t *p = ibuf + off;
    if (fresh) {
        memset(p, 0, fs.isize);
        if (fs.isize >= 160)
            wr16(p + 128, 32);
    }
    wr16(p, in->mode);
    wr16(p + 2, in->uid & 0xFFFF);
    wr32(p + 4, (uint32_t)in->size);
    wr32(p + 8, in->atime);
    wr32(p + 12, in->ctime);
    wr32(p + 16, in->mtime);
    wr32(p + 20, in->dtime);
    wr16(p + 24, in->gid & 0xFFFF);
    wr16(p + 26, in->links);
    wr32(p + 28, in->blocks512);
    wr32(p + 32, in->flags);
    for (int i = 0; i < 15; i++)
        wr32(p + 40 + 4 * i, in->block[i]);
    wr32(p + 104, in->file_acl);
    wr32(p + 108, (in->mode & S_IFMT) == S_IFREG ? (uint32_t)(in->size >> 32)
                                                  : 0);
    wr16(p + 120, in->uid >> 16);
    wr16(p + 122, in->gid >> 16);
    if (write_block(b, ibuf) < 0)
        return -1;
    for (int i = 0; i < ICACHE; i++)
        if (icache[i].in.num == in->num)
            icache[i].in = *in;
    return 0;
}

static int is_dir_mode(const struct inode *in)
{
    return (in->mode & S_IFMT) == S_IFDIR;
}

static int is_reg_mode(const struct inode *in)
{
    return (in->mode & S_IFMT) == S_IFREG;
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

/* A block for the pointer in slot i of pointer block 'holder' (or the
   inode's own slot, *root): the one there, or a new one - zeroed, a
   pointer block's children must read as holes - counted in the
   inode's blocks */
static uint32_t ensure(struct inode *in, uint32_t *root, uint32_t holder,
                       uint32_t i, uint32_t goal, int zero)
{
    uint32_t cur;
    if (root) {
        cur = *root;
    } else {
        const uint32_t *p = pointers(holder);
        if (!p)
            return 0;
        cur = p[i];
    }
    if (cur && cur < fs.blocks)
        return cur;
    uint32_t b = alloc_block(goal);
    if (!b)
        return 0;
    if (zero && zero_block(b) < 0) {
        free_block(b);
        return 0;
    }
    in->blocks512 += fs.bs / 512;
    if (root) {
        *root = b;
    } else if (ptr_set(holder, i, b) < 0) {
        free_block(b);
        in->blocks512 -= fs.bs / 512;
        return 0;
    }
    return b;
}

/* The block under a file's n-th block, made when missing (and the
   pointer blocks on the way to it). *fresh says whether the data block
   is new. 0 when the disk is full or n is past what three levels
   reach. The inode is changed in memory; the caller writes it. */
static uint32_t bmap_alloc(struct inode *in, uint64_t n, int *fresh)
{
    uint32_t per = fs.bs / 4;
    uint32_t goal = n ? bmap(in, n - 1) + 1 : 0;
    if (goal <= 1)
        goal = fs.first_data + ((in->num - 1) / fs.ipg) * fs.bpg;
    uint32_t had = bmap(in, n);
    *fresh = !had;
    if (had)
        return had;
    if (n < 12)
        return ensure(in, &in->block[n], 0, 0, goal, 0);
    n -= 12;
    if (n < per) {
        uint32_t l1 = ensure(in, &in->block[12], 0, 0, goal, 1);
        return l1 ? ensure(in, 0, l1, (uint32_t)n, goal, 0) : 0;
    }
    n -= per;
    if (n < (uint64_t)per * per) {
        uint32_t l1 = ensure(in, &in->block[13], 0, 0, goal, 1);
        uint32_t l2 = l1 ? ensure(in, 0, l1, (uint32_t)(n / per), goal, 1) : 0;
        return l2 ? ensure(in, 0, l2, (uint32_t)(n % per), goal, 0) : 0;
    }
    n -= (uint64_t)per * per;
    if (n < (uint64_t)per * per * per) {
        uint32_t l1 = ensure(in, &in->block[14], 0, 0, goal, 1);
        uint32_t l2 = l1 ? ensure(in, 0, l1, (uint32_t)(n / per / per), goal, 1)
                         : 0;
        uint32_t l3 = l2 ? ensure(in, 0, l2, (uint32_t)((n / per) % per),
                                  goal, 1) : 0;
        return l3 ? ensure(in, 0, l3, (uint32_t)(n % per), goal, 0) : 0;
    }
    return 0;
}

/* Frees the blocks under *root, a subtree of 'level' levels of pointers
   (0: a data block) covering the file's blocks from 'base', from the
   file block 'keep' on. *root becomes 0 when nothing in it is kept. */
static void cut(struct inode *in, uint32_t *root, int level, uint64_t base,
                uint64_t keep)
{
    uint32_t b = *root;
    if (!b)
        return;
    if (b >= fs.blocks) {
        *root = 0;                       /* not ours: forget it, free nothing */
        return;
    }
    uint64_t span = 1, per = fs.bs / 4;
    for (int l = 0; l < level; l++)
        span *= per;
    if (keep >= base + span)
        return;                          /* all of it stays */
    if (level > 0) {
        uint32_t *kids = kmalloc(fs.bs);
        if (!kids || read_block(b, kids) < 0) {
            kfree(kids);
            return;
        }
        uint64_t child = span / per;
        int changed = 0;
        for (uint32_t i = 0; i < per; i++) {
            uint32_t was = kids[i];
            cut(in, &kids[i], level - 1, base + i * child, keep);
            changed |= kids[i] != was;
        }
        if (keep > base) {
            if (changed)
                write_block(b, kids);
            pcache_forget(b);
            kfree(kids);
            return;                      /* some of it stays: so does b */
        }
        kfree(kids);
    } else if (keep > base) {
        return;
    }
    free_block(b);
    in->blocks512 -= in->blocks512 >= fs.bs / 512 ? fs.bs / 512 : in->blocks512;
    *root = 0;
}

/* Everything from the file block 'keep' on given back */
static void cut_from(struct inode *in, uint64_t keep)
{
    uint64_t per = fs.bs / 4;
    for (int i = 0; i < 12; i++)
        cut(in, &in->block[i], 0, (uint64_t)i, keep);
    cut(in, &in->block[12], 1, 12, keep);
    cut(in, &in->block[13], 2, 12 + per, keep);
    cut(in, &in->block[14], 3, 12 + per + per * per, keep);
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

/* Bytes into a file, blocks made as they are needed; the size grows
   past what is written. The inode is written by the caller. */
static int write_bytes(struct inode *in, uint64_t off, const void *buf,
                       uint32_t len)
{
    const uint8_t *src = buf;
    uint32_t done = 0;
    while (done < len) {
        uint64_t pos = off + done;
        uint32_t boff = (uint32_t)(pos % fs.bs);
        uint32_t chunk = fs.bs - boff;
        if (chunk > len - done)
            chunk = len - done;
        int fresh;
        uint32_t b = bmap_alloc(in, pos / fs.bs, &fresh);
        if (!b)
            break;                       /* the disk is full */
        if (chunk != fs.bs) {
            if (fresh)
                memset(bbuf, 0, fs.bs);
            else if (read_block(b, bbuf) < 0)
                return done ? (int)done : -1;
        }
        memcpy(bbuf + boff, src + done, chunk);
        if (write_block(b, bbuf) < 0)
            return done ? (int)done : -1;
        done += chunk;
    }
    if (off + done > in->size)
        in->size = off + done;
    return (int)done;
}

/* --- rights ---------------------------------------------------------------
 * POSIX's: the owner's, the group's and everybody else's bits, read,
 * write and enter (or run). An administrator's session passes every
 * check, as root does, but for running a file nobody may run. A task's
 * group is "users" (100), an administrator's 0 - ThreadOS has no other
 * groups yet. A directory with the sticky bit (the root, 1777, as /tmp)
 * lets anybody make files in it and only a file's owner take them away.
 * The system's files in the root stay the administrator's alone to
 * make, rename or remove, whatever the bits say. */

#define USERS_GID 100

static int admin(void)
{
    return task64_level() == USER_ADMIN;
}

static uint32_t my_uid(void)
{
    return (uint32_t)task64_uid();
}

static uint32_t my_gid(void)
{
    return admin() ? 0 : USERS_GID;
}

/* want: 4 read, 2 write, 1 enter or run, or a sum */
static int permits(const struct inode *in, int want)
{
    if (admin())
        return !(want & 1) || is_dir_mode(in) || (in->mode & 0111);
    uint32_t bits;
    if (in->uid == my_uid())
        bits = (in->mode >> 6) & 7;
    else if (in->gid == my_gid())
        bits = (in->mode >> 3) & 7;
    else
        bits = in->mode & 7;
    return (bits & (uint32_t)want) == (uint32_t)want;
}

static int may_read(const struct inode *in)
{
    return permits(in, 4);
}

static int may_enter(const struct inode *in)
{
    return permits(in, 1);
}

static int may_write(const struct inode *in)
{
    return permits(in, 2);
}

/* May the current task make or take away names in this directory, and
   (with the sticky bit) take away this one, owned by 'owner' */
static int may_change(const struct inode *dir)
{
    return permits(dir, 3);
}

static int may_remove(const struct inode *dir, const struct inode *victim)
{
    if (!may_change(dir))
        return 0;
    return !(dir->mode & 01000) || admin() || victim->uid == my_uid() ||
           dir->uid == my_uid();
}

static int owner_or_admin(const struct inode *in)
{
    return admin() || in->uid == my_uid();
}

static int fail(int e)
{
    fs_set_errno(e);
    return -1;
}

/* The files in the root only an administrator may make, change or
   remove (kernel/user.c's list and the dock's), and those of them
   everybody may read */
static int system_file(const char *name)
{
    return user_is_system_file(name) || !strcmp(name, "desktop.icons");
}

static int public_file(const char *name)
{
    return !strcmp(name, "system.cfg") || !strcmp(name, "icons.pos") ||
           !strcmp(name, "update.key") || !strcmp(name, "desktop.icons");
}

/* --- directories -------------------------------------------------------- */

struct dentry {
    uint32_t ino;
    uint8_t type;
    uint32_t name_len;
    char name[FS_NAME_LEN + 1];
};

static uint8_t dblk[4096];       /* the directory block last read */
static uint32_t dblk_no, dblk_of;

/* The entry at byte *off of a directory, skipping empty ones: 1 and
   *off moved past it, 0 at the end, -1 for an entry that does not
   hold together (a record length of 0, running past its block, a name
   longer than its record) - the rest of a damaged directory is not
   read rather than read wrongly */
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

static uint32_t rec_need(uint32_t name_len)
{
    return (8 + name_len + 3) & ~3u;
}

static void put_entry(uint8_t *p, uint32_t ino, uint32_t rec,
                      const char *name, uint32_t nl, uint8_t type)
{
    wr32(p, ino);
    wr16(p + 4, rec);
    p[6] = (uint8_t)nl;
    p[7] = fs.filetype ? type : 0;
    memcpy(p + 8, name, nl);
    for (uint32_t i = 8 + nl; i < rec_need(nl) && i < rec; i++)
        p[i] = 0;
}

/* An htree directory changed by a writer that does not keep its index:
   the flag goes, and it is a plain list again, as old Linux did */
static void unindex(struct inode *dir)
{
    dir->flags &= ~INDEX_FL;
}

/* A new entry in a directory: into the slack of an entry with room to
   spare, or a new block at the end */
static int dir_add(struct inode *dir, const char *name, uint32_t nl,
                   uint32_t ino, uint8_t type)
{
    uint32_t need = rec_need(nl);
    uint64_t nblocks = dir->size / fs.bs;
    for (uint64_t k = 0; k < nblocks; k++) {
        uint32_t b = bmap(dir, k);
        if (!b || read_block(b, dblk) < 0)
            return -1;
        dblk_no = 0;
        for (uint32_t off = 0; off + 8 <= fs.bs;) {
            uint8_t *p = dblk + off;
            uint32_t rec = rd16(p + 4);
            if (rec < 8 || rec % 4 || off + rec > fs.bs)
                return -1;
            uint32_t used = rd32(p) ? rec_need(fs.filetype ? p[6] : rd16(p + 6))
                                    : 0;
            if (rec - used >= need) {
                if (used) {
                    wr16(p + 4, used);
                    put_entry(p + used, ino, rec - used, name, nl, type);
                } else {
                    put_entry(p, ino, rec, name, nl, type);
                }
                unindex(dir);
                return write_block(b, dblk);
            }
            off += rec;
        }
    }
    int fresh;
    uint32_t b = bmap_alloc(dir, nblocks, &fresh);
    if (!b)
        return -1;
    memset(dblk, 0, fs.bs);
    dblk_no = 0;
    put_entry(dblk, ino, fs.bs, name, nl, type);
    if (write_block(b, dblk) < 0)
        return -1;
    dir->size += fs.bs;
    unindex(dir);
    return 0;
}

/* The entry naming 'name' taken out: its record joins the one before it
   in its block, or, first in its block, it is marked empty */
static int dir_remove(struct inode *dir, const char *name, uint32_t nl)
{
    uint64_t nblocks = dir->size / fs.bs;
    for (uint64_t k = 0; k < nblocks; k++) {
        uint32_t b = bmap(dir, k);
        if (!b || read_block(b, dblk) < 0)
            return -1;
        dblk_no = 0;
        uint32_t prev = ~0u;
        for (uint32_t off = 0; off + 8 <= fs.bs;) {
            uint8_t *p = dblk + off;
            uint32_t rec = rd16(p + 4);
            if (rec < 8 || rec % 4 || off + rec > fs.bs)
                return -1;
            uint32_t len = fs.filetype ? p[6] : rd16(p + 6);
            if (rd32(p) && len == nl && !memcmp(p + 8, name, nl)) {
                if (prev != ~0u)
                    wr16(dblk + prev + 4, rd16(dblk + prev + 4) + rec);
                else
                    wr32(p, 0);
                unindex(dir);
                return write_block(b, dblk);
            }
            prev = off;
            off += rec;
        }
    }
    return -1;
}

/* A directory's ".." pointed somewhere else (a directory moved) */
static int dir_set_parent(struct inode *dir, uint32_t parent)
{
    uint32_t b = bmap(dir, 0);
    if (!b || read_block(b, dblk) < 0)
        return -1;
    dblk_no = 0;
    uint32_t rec = rd16(dblk + 4);
    if (rec < 12 || rec + 12 > fs.bs)
        return -1;
    uint8_t *p = dblk + rec;             /* ".." is the second entry */
    if ((fs.filetype ? p[6] : rd16(p + 6)) != 2 || p[8] != '.' || p[9] != '.')
        return -1;
    wr32(p, parent);
    return write_block(b, dblk);
}

static int dir_empty(const struct inode *dir)
{
    static struct dentry e;
    uint64_t off = 0;
    int r;
    while ((r = dir_next(dir, &off, &e)) == 1)
        if (!(e.name_len == 1 && e.name[0] == '.') &&
            !(e.name_len == 2 && e.name[0] == '.' && e.name[1] == '.'))
            return 0;
    return r == 0;
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
        if (load_inode(cur, &in) < 0)
            return fail(FS_EIO), 0;
        if (!is_dir_mode(&in))
            return fail(FS_ENOTDIR), 0;
        if (!may_enter(&in))
            return fail(FS_EACCES), 0;
        if (len > FS_NAME_LEN)
            return fail(FS_ENAMETOOLONG), 0;
        uint32_t next = len == 2 && s[0] == '.' && s[1] == '.'
                            ? (cur == ROOT_INO ? ROOT_INO
                                               : dir_lookup(&in, "..", 2))
                            : dir_lookup(&in, s, len);
        if (!next)
            return fail(FS_ENOENT), 0;
        if (load_inode(next, &in) < 0)
            return fail(FS_EIO), 0;
        int last = p == end || (p < end && p + 1 == end && *p == '/');
        if ((in.mode & S_IFMT) == S_IFLNK && (!last || follow_last)) {
            if (depth >= SYMLINK_DEPTH)
                return fail(FS_ELOOP), 0;
            char *target = kmalloc(FS_PATH_MAX);
            if (!target)
                return fail(FS_EIO), 0;
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

/* The directory a path's last name is in, and that name (into base,
   FS_NAME_LEN + 1 bytes): the directory's inode, or 0. "." and ".."
   are not names one can make or take away. */
static uint32_t resolve_parent(int cwd, const char *path, char *base)
{
    uint32_t n = (uint32_t)strlen(path);
    while (n > 1 && path[n - 1] == '/')
        n--;                             /* "dir/" names dir */
    uint32_t s = n;
    while (s > 0 && path[s - 1] != '/')
        s--;
    uint32_t bl = n - s;
    if (bl > FS_NAME_LEN)
        return fail(FS_ENAMETOOLONG), 0;
    if (!bl || (bl == 1 && path[s] == '.') ||
        (bl == 2 && path[s] == '.' && path[s + 1] == '.'))
        return fail(FS_EINVAL), 0;
    memcpy(base, path + s, bl);
    base[bl] = '\0';
    uint32_t dir = s ? resolve_in(ino_of_slot(cwd), path, s, 1, 0)
                     : ino_of_slot(cwd);
    static struct inode in;
    if (!dir)
        return 0;
    if (load_inode(dir, &in) < 0)
        return fail(FS_EIO), 0;
    if (!is_dir_mode(&in))
        return fail(FS_ENOTDIR), 0;
    if (!may_enter(&in))
        return fail(FS_EACCES), 0;
    return dir;
}

/* --- making and taking away -------------------------------------------- */

static int ft_of(const struct inode *in)
{
    switch (in->mode & S_IFMT) {
    case S_IFDIR: return FT_DIR;
    case S_IFREG: return FT_REG;
    case S_IFLNK: return 7;
    default:      return 0;
    }
}

#define MODE_UNSET 0xFFFFFFFFu        /* the caller gave no mode */
#define K_FILE 0
#define K_DIR  1
#define K_LINK 2

/* A new inode named 'base' in directory 'dir': a file, a directory with
   its "." and "..", or a symbolic link holding 'target'. The mode asked
   for loses the task's umask; with none asked for, a file is its
   owner's alone (as TFS3's were - the kernel's own files among them)
   and a directory is everybody's to pass and read. The system's files
   in the root are the system's, 0644 when everybody reads them and
   0600 when nobody else may. 0 and fs_errno when it cannot be made. */
static uint32_t make(uint32_t dir_ino, const char *base, int kind,
                     uint32_t mode, const char *target)
{
    static struct inode parent;
    struct inode in;
    if (!fs.writable)
        return fail(FS_EROFS), 0;
    if (load_inode(dir_ino, &parent) < 0)
        return fail(FS_EIO), 0;
    if (!may_change(&parent) ||
        (dir_ino == ROOT_INO && system_file(base) && !admin()))
        return fail(FS_EACCES), 0;
    uint32_t nl = (uint32_t)strlen(base);
    if (dir_lookup(&parent, base, nl))
        return fail(FS_EEXIST), 0;
    if (kind == K_DIR && parent.links >= 65000)
        return fail(FS_EMLINK), 0;
    uint32_t ino = alloc_inode(kind == K_DIR ? 0 : dir_ino, kind == K_DIR);
    if (!ino)
        return fail(FS_ENOSPC), 0;
    int sys = dir_ino == ROOT_INO && system_file(base);
    uint32_t mask = (uint32_t)task64_umask();
    memset(&in, 0, sizeof(in));
    in.num = ino;
    in.uid = sys ? 0 : my_uid();
    in.gid = sys ? 0 : my_gid();
    uint32_t t = now();
    in.atime = in.ctime = in.mtime = t;
    if (kind == K_DIR) {
        in.mode = S_IFDIR | ((mode == MODE_UNSET ? 0777 : mode & 07777) &
                             ~mask);
        in.links = 2;
        int fresh;
        uint32_t b = bmap_alloc(&in, 0, &fresh);
        if (!b)
            goto full;
        memset(dblk, 0, fs.bs);
        dblk_no = 0;
        put_entry(dblk, ino, 12, ".", 1, FT_DIR);
        put_entry(dblk + 12, dir_ino, fs.bs - 12, "..", 2, FT_DIR);
        if (write_block(b, dblk) < 0)
            goto undo;
        in.size = fs.bs;
    } else if (kind == K_LINK) {
        uint32_t tl = (uint32_t)strlen(target);
        in.mode = S_IFLNK | 0777;
        in.links = 1;
        in.size = tl;
        if (tl < 60) {
            /* a short target lives in the block list, as ext2's "fast"
               links do: no block at all */
            memcpy(in.block, target, tl);
        } else if (write_bytes(&in, 0, target, tl) != (int)tl) {
            goto full;
        }
    } else {
        in.mode = S_IFREG |
                  (sys ? (public_file(base) ? 0644 : 0600)
                       : mode == MODE_UNSET ? 0600 : (mode & 07777) & ~mask);
        in.links = 1;
    }
    /* the inode first, then the name: a crash between leaks an inode
       e2fsck gives back, rather than a name for nothing */
    if (write_inode(&in, 1) < 0)
        goto undo;
    if (dir_add(&parent, base, nl, ino, (uint8_t)ft_of(&in)) < 0)
        goto full;
    if (kind == K_DIR)
        parent.links++;
    parent.mtime = parent.ctime = t;
    write_inode(&parent, 0);
    return ino;
full:
    fs_set_errno(FS_ENOSPC);
undo:
    if (!fs_errno())
        fs_set_errno(FS_EIO);
    if (kind != K_LINK || in.size >= 60)
        cut_from(&in, 0);
    free_inode_bit(ino, kind == K_DIR);
    return 0;
}

/* The superblock's orphan list: inodes with no name, still open, chained
   through their deletion time, as ext3 and e2fsck read it */
static void orphan_add(struct inode *in)
{
    in->dtime = fs.last_orphan;
    fs.last_orphan = in->num;
    fs.sb_dirty = 1;
}

static void orphan_remove(uint32_t ino)
{
    static struct inode o;
    uint32_t prev = 0, cur = fs.last_orphan;
    for (int guard = 0; cur && guard < 65536; guard++) {
        if (load_inode(cur, &o) < 0)
            return;
        if (cur == ino) {
            uint32_t next = o.dtime;
            if (!prev) {
                fs.last_orphan = next;
                fs.sb_dirty = 1;
            } else {
                static struct inode p;
                if (load_inode(prev, &p) == 0) {
                    p.dtime = next;
                    write_inode(&p, 0);
                }
            }
            return;
        }
        prev = cur;
        cur = o.dtime;
    }
}

/* An extended attribute block one inode fewer refers to: freed when
   none does (e2fsck counts them) */
static void drop_xattr(struct inode *in)
{
    if (!in->file_acl || in->file_acl >= fs.blocks)
        return;
    if (read_block(in->file_acl, bbuf) == 0 && rd32(bbuf) == 0xEA020000) {
        uint32_t refs = rd32(bbuf + 4);
        if (refs > 1) {
            wr32(bbuf + 4, refs - 1);
            write_block(in->file_acl, bbuf);
        } else {
            free_block(in->file_acl);
        }
        in->blocks512 -= in->blocks512 >= fs.bs / 512 ? fs.bs / 512 : 0;
    }
    in->file_acl = 0;
}

/* An inode with no names left and nobody holding it: its blocks and
   itself given back, its deletion time set as e2fsck wants of a freed
   inode */
static void release(struct inode *in)
{
    int dir = is_dir_mode(in);
    if (!((in->mode & S_IFMT) == S_IFLNK && in->size < 60 &&
          in->blocks512 == 0))
        cut_from(in, 0);
    else
        memset(in->block, 0, sizeof(in->block));
    drop_xattr(in);
    in->links = 0;
    in->size = 0;
    in->dtime = now();
    write_inode(in, 0);
    free_inode_bit(in->num, dir);
}

/* --- the backend's calls --------------------------------------------------- */

static int t4_open_mode(int cwd, const char *path, int create,
                        uint32_t mode)
{
    irqflags_t flags = irq_save();
    static struct inode in;
    uint32_t ino = resolve(cwd, path);
    int r = -1;
    if (ino) {
        if (load_inode(ino, &in) < 0)
            fail(FS_EIO);
        else if (is_dir_mode(&in))
            fail(FS_EISDIR);
        else if (!is_reg_mode(&in))
            fail(FS_EINVAL);
        else if (!may_read(&in))
            fail(FS_EACCES);
        else
            r = slot_of_ino(ino);
    } else if (create && fs_errno() == FS_ENOENT) {
        static char base[FS_NAME_LEN + 1];
        fs_set_errno(0);
        uint32_t dir = resolve_parent(cwd, path, base);
        uint32_t made = dir ? make(dir, base, K_FILE, mode, 0) : 0;
        if (made)
            r = slot_of_ino(made);
        meta_flush();
    }
    irq_restore(flags);
    return r;
}

static int t4_open(int cwd, const char *path, int create)
{
    return t4_open_mode(cwd, path, create, MODE_UNSET);
}

static int t4_mkdir_mode(int cwd, const char *path, uint32_t mode)
{
    irqflags_t flags = irq_save();
    static char base[FS_NAME_LEN + 1];
    int r = -1;
    if (resolve(cwd, path)) {
        fail(FS_EEXIST);
    } else if (fs_errno() == FS_ENOENT) {
        fs_set_errno(0);
        uint32_t dir = resolve_parent(cwd, path, base);
        if (dir && make(dir, base, K_DIR, mode, 0))
            r = 0;
        meta_flush();
    }
    irq_restore(flags);
    return r;
}

static int t4_mkdir(int cwd, const char *path)
{
    return t4_mkdir_mode(cwd, path, MODE_UNSET);
}

/* A name taken away. A directory has to be empty; a file still open
   keeps its blocks until its last close, on the orphan list. */
static int t4_unlink(int cwd, const char *path)
{
    irqflags_t flags = irq_save();
    static char base[FS_NAME_LEN + 1];
    static struct inode parent, in;
    int r = -1;
    if (!fs.writable) {
        fail(FS_EROFS);
        goto done;
    }
    uint32_t dir = resolve_parent(cwd, path, base);
    if (!dir)
        goto done;
    uint32_t nl = (uint32_t)strlen(base);
    if (load_inode(dir, &parent) < 0) {
        fail(FS_EIO);
        goto done;
    }
    uint32_t ino = dir_lookup(&parent, base, nl);
    if (!ino) {
        fail(FS_ENOENT);
        goto done;
    }
    if (ino == ROOT_INO || ino == LOST_FOUND_INO || load_inode(ino, &in) < 0) {
        fail(FS_EPERM);
        goto done;
    }
    if (!may_change(&parent) ||
        (dir == ROOT_INO && system_file(base) && !admin())) {
        fail(FS_EACCES);
        goto done;
    }
    if (!may_remove(&parent, &in)) {
        fail(FS_EPERM);                  /* the sticky bit: not one's own */
        goto done;
    }
    int isdir = is_dir_mode(&in);
    if (isdir && !dir_empty(&in)) {
        fail(FS_ENOTEMPTY);
        goto done;
    }
    if (dir_remove(&parent, base, nl) < 0) {
        fail(FS_EIO);
        goto done;
    }
    uint32_t t = now();
    parent.mtime = parent.ctime = t;
    if (isdir && parent.links > 2)
        parent.links--;
    write_inode(&parent, 0);
    in.ctime = t;
    if (isdir) {
        release(&in);
    } else if (in.links > 1) {
        in.links--;
        write_inode(&in, 0);
    } else if (task64_slot_open(slot_of_ino(ino))) {
        /* the Unix way, which tmpfile() and databases count on: the
           name goes now, the data at the last close */
        in.links = 0;
        orphan_add(&in);
        write_inode(&in, 0);
    } else {
        release(&in);
    }
    r = 0;
done:
    meta_flush();
    irq_restore(flags);
    return r;
}

static int inode_in_use(uint32_t ino)
{
    uint32_t g = (ino - 1) / fs.ipg, i = (ino - 1) % fs.ipg;
    uint8_t *bits = g < fs.groups ? bitmap_of(1, g) : 0;
    return bits && (bits[i / 8] & (1u << (i % 8)));
}

/* A descriptor closed: a file unlinked while open goes with the last */
static void t4_slot_closed(int slot)
{
    irqflags_t flags = irq_save();
    static struct inode in;
    uint32_t ino = ino_of_slot(slot);
    if (fs.writable && load_inode(ino, &in) == 0 && in.links == 0 &&
        in.mode && inode_in_use(ino) && !task64_slot_open(slot)) {
        orphan_remove(ino);
        release(&in);
        meta_flush();
    }
    irq_restore(flags);
}

/* A new name, perhaps in another directory. The target, if there,
   goes first (a file over a file, an empty directory over a
   directory); a directory may not move into itself or below. */
static int t4_rename(int cwd, const char *from, const char *to)
{
    irqflags_t flags = irq_save();
    static char a[FS_NAME_LEN + 1], b[FS_NAME_LEN + 1];
    static struct inode pa, pb, in, old;
    int r = -1;
    if (!fs.writable) {
        fail(FS_EROFS);
        goto done;
    }
    uint32_t da = resolve_parent(cwd, from, a);
    if (!da)
        goto done;
    uint32_t db = resolve_parent(cwd, to, b);
    if (!db)
        goto done;
    uint32_t al = (uint32_t)strlen(a), bl = (uint32_t)strlen(b);
    if (load_inode(da, &pa) < 0 || load_inode(db, &pb) < 0) {
        fail(FS_EIO);
        goto done;
    }
    uint32_t ino = dir_lookup(&pa, a, al);
    if (!ino) {
        fail(FS_ENOENT);
        goto done;
    }
    if (ino == LOST_FOUND_INO || load_inode(ino, &in) < 0) {
        fail(FS_EPERM);
        goto done;
    }
    if (!may_change(&pa) || !may_change(&pb) ||
        (da == ROOT_INO && system_file(a) && !admin()) ||
        (db == ROOT_INO && system_file(b) && !admin())) {
        fail(FS_EACCES);
        goto done;
    }
    if (!may_remove(&pa, &in)) {
        fail(FS_EPERM);
        goto done;
    }
    int isdir = is_dir_mode(&in);
    if (isdir) {
        /* not into itself: walk up from the target's directory */
        static struct inode up;
        uint32_t cur = db;
        for (int guard = 0; cur != ROOT_INO && guard < 4096; guard++) {
            if (cur == ino || load_inode(cur, &up) < 0) {
                fail(FS_EINVAL);
                goto done;
            }
            cur = dir_lookup(&up, "..", 2);
            if (!cur) {
                fail(FS_EIO);
                goto done;
            }
        }
        if (db == ino) {
            fail(FS_EINVAL);
            goto done;
        }
    }
    uint32_t there = dir_lookup(&pb, b, bl);
    if (there == ino) {
        r = 0;                           /* a rename onto itself */
        goto done;
    }
    if (there) {
        if (load_inode(there, &old) < 0 || there == LOST_FOUND_INO) {
            fail(FS_EPERM);
            goto done;
        }
        if (!may_remove(&pb, &old)) {
            fail(FS_EPERM);
            goto done;
        }
        if (is_dir_mode(&old) != isdir) {
            fail(isdir ? FS_ENOTDIR : FS_EISDIR);
            goto done;
        }
        if (isdir && !dir_empty(&old)) {
            fail(FS_ENOTEMPTY);
            goto done;
        }
        if (dir_remove(&pb, b, bl) < 0)
            goto done;
        if (is_dir_mode(&old) && pb.links > 2)
            pb.links--;
        if (!is_dir_mode(&old) && old.links > 1) {
            old.links--;
            write_inode(&old, 0);
        } else if (!is_dir_mode(&old) &&
                   task64_slot_open(slot_of_ino(there))) {
            old.links = 0;
            orphan_add(&old);
            write_inode(&old, 0);
        } else {
            release(&old);
        }
        if (da == db)
            pa = pb;
    }
    /* the new name before the old one goes: a crash leaves two names
       for the file, which e2fsck counts, rather than none */
    if (dir_add(&pb, b, bl, ino, (uint8_t)ft_of(&in)) < 0)
        goto done;
    if (da == db)
        pa = pb;
    if (dir_remove(&pa, a, al) < 0)
        goto done;
    if (da == db)
        pb = pa;
    uint32_t t = now();
    if (isdir && da != db) {
        dir_set_parent(&in, db);
        pa.links--;
        pb.links++;
    }
    pa.mtime = pa.ctime = pb.mtime = pb.ctime = t;
    in.ctime = t;
    write_inode(&in, 0);
    if (da != db)
        write_inode(&pa, 0);
    write_inode(&pb, 0);
    r = 0;
done:
    meta_flush();
    irq_restore(flags);
    return r;
}

static int t4_chdir(int cwd, const char *path, int *newcwd)
{
    irqflags_t flags = irq_save();
    static struct inode in;
    uint32_t ino = resolve(cwd, path);
    int ok = 0;
    if (ino) {
        if (load_inode(ino, &in) < 0)
            fail(FS_EIO);
        else if (!is_dir_mode(&in))
            fail(FS_ENOTDIR);
        else if (!may_enter(&in))
            fail(FS_EACCES);
        else
            ok = 1;
    }
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
    st->type = is_dir_mode(in) ? FS_T_DIR
               : (in->mode & S_IFMT) == S_IFLNK ? FS_T_LINK : FS_T_FILE;
    st->size = in->size;
    st->owner = (int)in->uid;
    st->mode = in->mode & 07777;
    st->gid = in->gid;
    st->nlink = in->links;
    st->atime = in->atime;
    st->mtime = in->mtime;
    st->ctime = in->ctime;
    st->blocks512 = in->blocks512;
}

/* What is at a path: asking needs only the way there, not the file's
   own bits, as stat(2) */
static int stat_at(int cwd, const char *path, struct fs_stat *st, int follow)
{
    irqflags_t flags = irq_save();
    static struct inode in;
    uint32_t ino = resolve_in(ino_of_slot(cwd), path,
                              (uint32_t)strlen(path), follow, 0);
    int ok = ino && load_inode(ino, &in) == 0;
    if (ok)
        fill(&in, st);
    irq_restore(flags);
    return ok ? 0 : -1;
}

static int t4_stat_path(int cwd, const char *path, struct fs_stat *st)
{
    return stat_at(cwd, path, st, 1);
}

static int t4_lstat_path(int cwd, const char *path, struct fs_stat *st)
{
    return stat_at(cwd, path, st, 0);
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
    if (load_inode(ino_of_slot(dir), &in) < 0) {
        fail(FS_EIO);
        goto done;
    }
    if (!is_dir_mode(&in)) {
        fail(FS_ENOTDIR);
        goto done;
    }
    if (!may_read(&in)) {
        fail(FS_EACCES);
        goto done;
    }
    for (;;) {
        r = dir_next(&in, cursor, &e);
        if (r != 1)
            break;
        if ((e.name_len == 1 && e.name[0] == '.') ||
            (e.name_len == 2 && e.name[0] == '.' && e.name[1] == '.'))
            continue;
        /* lost+found is e2fsck's, not a place anybody keeps things */
        if (e.ino == LOST_FOUND_INO && in.num == ROOT_INO)
            continue;
        int have = load_inode(e.ino, &child) == 0;
        memset(out, 0, sizeof(*out));
        memcpy(out->name, e.name, e.name_len + 1);
        out->ino = e.ino;
        out->slot = slot_of_ino(e.ino);
        if (have) {
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

static int t4_can_write(int slot)
{
    static struct inode in;
    irqflags_t flags = irq_save();
    int r = fs.writable && load_inode(ino_of_slot(slot), &in) == 0 &&
            may_write(&in);
    irq_restore(flags);
    return r;
}

static int t4_set_owner(int slot, int uid)
{
    static struct inode in;
    irqflags_t flags = irq_save();
    int r = -1;
    if (fs.writable && admin() && uid >= 0 &&
        load_inode(ino_of_slot(slot), &in) == 0) {
        in.uid = (uint32_t)uid;
        in.gid = uid ? USERS_GID : 0;
        in.ctime = now();
        r = write_inode(&in, 0);
    }
    irq_restore(flags);
    return r;
}

/* The file made 'size' bytes long: cut back - the blocks past the end
   given back, the rest of the last one zeroed so that a file grown
   again reads zeros there - or grown, the bytes in between a hole */
static int t4_truncate_to(int slot, uint64_t size)
{
    static struct inode in;
    irqflags_t flags = irq_save();
    int r = -1;
    if (!fs.writable) {
        fail(FS_EROFS);
        goto done;
    }
    if (load_inode(ino_of_slot(slot), &in) < 0 || !is_reg_mode(&in)) {
        fail(FS_EINVAL);
        goto done;
    }
    if (!may_write(&in)) {
        fail(FS_EACCES);
        goto done;
    }
    if (size < in.size || size == 0) {
        cut_from(&in, (size + fs.bs - 1) / fs.bs);
        if (size % fs.bs) {
            uint32_t b = bmap(&in, size / fs.bs);
            if (b && read_block(b, bbuf) == 0) {
                memset(bbuf + size % fs.bs, 0, fs.bs - size % fs.bs);
                write_block(b, bbuf);
            }
        }
    }
    in.size = size;
    in.mtime = in.ctime = now();
    r = write_inode(&in, 0);
done:
    meta_flush();
    irq_restore(flags);
    return r;
}

static int t4_read_at(int slot, uint64_t off, void *buf, uint32_t len)
{
    static struct inode in;
    irqflags_t flags = irq_save();
    int r = -1;
    if (load_inode(ino_of_slot(slot), &in) == 0 && is_reg_mode(&in) &&
        may_read(&in))
        r = read_bytes(&in, off, buf, len);
    irq_restore(flags);
    return r;
}

static int t4_write_at(int slot, uint64_t off, const void *buf, uint32_t len)
{
    static struct inode in;
    irqflags_t flags = irq_save();
    int r = -1;
    if (fs.writable && load_inode(ino_of_slot(slot), &in) == 0 &&
        is_reg_mode(&in) && may_write(&in)) {
        r = write_bytes(&in, off, buf, len);
        in.mtime = in.ctime = now();
        if (write_inode(&in, 0) < 0)
            r = -1;
        if (r == 0 && len)
            r = -1;                    /* nothing written: the disk is full */
    }
    meta_flush();
    irq_restore(flags);
    return r;
}

/* The block under a file's index-th block, for swap and mappings; made
   when asked to (swap reserves its file this way), zeroed */
static uint32_t t4_file_block(int slot, uint32_t index, int alloc)
{
    static struct inode in;
    irqflags_t flags = irq_save();
    uint32_t b = 0;
    if (load_inode(ino_of_slot(slot), &in) == 0 && is_reg_mode(&in)) {
        if (!alloc) {
            if (may_read(&in))
                b = bmap(&in, index);
        } else if (fs.writable && may_write(&in)) {
            int fresh;
            b = bmap_alloc(&in, index, &fresh);
            if (b && fresh && zero_block(b) < 0)
                b = 0;
            write_inode(&in, 0);
            meta_flush();
        }
    }
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

/* --- POSIX's metadata ---------------------------------------------------- */

static int t4_chmod(int slot, uint32_t mode)
{
    irqflags_t flags = irq_save();
    static struct inode in;
    int r = -1;
    if (!fs.writable)
        fail(FS_EROFS);
    else if (load_inode(ino_of_slot(slot), &in) < 0)
        fail(FS_ENOENT);
    else if (!owner_or_admin(&in))
        fail(FS_EPERM);
    else {
        in.mode = (uint16_t)((in.mode & S_IFMT) | (mode & 07777));
        in.ctime = now();
        r = write_inode(&in, 0) < 0 ? fail(FS_EIO) : 0;
    }
    irq_restore(flags);
    return r;
}

/* Only an administrator gives a file away (-1 keeps what is there) */
static int t4_chown(int slot, int uid, int gid)
{
    irqflags_t flags = irq_save();
    static struct inode in;
    int r = -1;
    if (!fs.writable)
        fail(FS_EROFS);
    else if (load_inode(ino_of_slot(slot), &in) < 0)
        fail(FS_ENOENT);
    else if (!admin())
        fail(FS_EPERM);
    else {
        if (uid >= 0)
            in.uid = (uint32_t)uid;
        if (gid >= 0)
            in.gid = (uint32_t)gid;
        in.ctime = now();
        r = write_inode(&in, 0) < 0 ? fail(FS_EIO) : 0;
    }
    irq_restore(flags);
    return r;
}

/* Times given outright are the owner's to set; "now" anybody's who may
   write the file, as utimensat(2) */
static int t4_utimes(int slot, int64_t atime, int64_t mtime)
{
    irqflags_t flags = irq_save();
    static struct inode in;
    int r = -1;
    int explicit_time = (atime != FS_TIME_NOW && atime != FS_TIME_OMIT) ||
                        (mtime != FS_TIME_NOW && mtime != FS_TIME_OMIT);
    if (!fs.writable)
        fail(FS_EROFS);
    else if (load_inode(ino_of_slot(slot), &in) < 0)
        fail(FS_ENOENT);
    else if (explicit_time ? !owner_or_admin(&in)
                           : !owner_or_admin(&in) && !may_write(&in))
        fail(explicit_time ? FS_EPERM : FS_EACCES);
    else {
        uint32_t t = now();
        if (atime != FS_TIME_OMIT)
            in.atime = atime == FS_TIME_NOW ? t : (uint32_t)atime;
        if (mtime != FS_TIME_OMIT)
            in.mtime = mtime == FS_TIME_NOW ? t : (uint32_t)mtime;
        in.ctime = t;
        r = write_inode(&in, 0) < 0 ? fail(FS_EIO) : 0;
    }
    irq_restore(flags);
    return r;
}

/* A second name for a file: not for a directory, which would make the
   tree a graph */
static int t4_link(int cwd, const char *from, const char *to)
{
    irqflags_t flags = irq_save();
    static char base[FS_NAME_LEN + 1];
    static struct inode in, parent;
    int r = -1;
    uint32_t ino = fs.writable ? resolve_in(ino_of_slot(cwd), from,
                                            (uint32_t)strlen(from), 0, 0)
                               : (fail(FS_EROFS), 0);
    if (!ino)
        goto done;
    if (load_inode(ino, &in) < 0 || is_dir_mode(&in)) {
        fail(FS_EPERM);
        goto done;
    }
    if (in.links >= 65000) {
        fail(FS_EMLINK);
        goto done;
    }
    uint32_t dir = resolve_parent(cwd, to, base);
    if (!dir)
        goto done;
    uint32_t nl = (uint32_t)strlen(base);
    if (load_inode(dir, &parent) < 0) {
        fail(FS_EIO);
        goto done;
    }
    if (dir_lookup(&parent, base, nl)) {
        fail(FS_EEXIST);
        goto done;
    }
    if (!may_change(&parent) ||
        (dir == ROOT_INO && system_file(base) && !admin())) {
        fail(FS_EACCES);
        goto done;
    }
    /* the count first: a crash between leaves one name too few counted
       up, which e2fsck puts right, never a name past the count */
    in.links++;
    in.ctime = now();
    if (write_inode(&in, 0) < 0 ||
        dir_add(&parent, base, nl, ino, (uint8_t)ft_of(&in)) < 0) {
        in.links--;
        write_inode(&in, 0);
        fail(FS_ENOSPC);
        goto done;
    }
    parent.mtime = parent.ctime = in.ctime;
    write_inode(&parent, 0);
    r = 0;
done:
    meta_flush();
    irq_restore(flags);
    return r;
}

static int t4_symlink(int cwd, const char *target, const char *path)
{
    irqflags_t flags = irq_save();
    static char base[FS_NAME_LEN + 1];
    int r = -1;
    if (resolve_in(ino_of_slot(cwd), path, (uint32_t)strlen(path), 0, 0)) {
        fail(FS_EEXIST);
    } else if (fs_errno() == FS_ENOENT) {
        fs_set_errno(0);
        uint32_t dir = resolve_parent(cwd, path, base);
        if (dir && make(dir, base, K_LINK, 0777, target))
            r = 0;
        meta_flush();
    }
    irq_restore(flags);
    return r;
}

static int t4_readlink(int cwd, const char *path, char *buf, uint32_t cap)
{
    irqflags_t flags = irq_save();
    static struct inode in;
    int r = -1;
    uint32_t ino = resolve_in(ino_of_slot(cwd), path, (uint32_t)strlen(path),
                              0, 0);
    if (ino) {
        if (load_inode(ino, &in) < 0 || (in.mode & S_IFMT) != S_IFLNK) {
            fail(FS_EINVAL);
        } else {
            char *t = kmalloc(FS_PATH_MAX);
            int n = t ? link_target(&in, t, FS_PATH_MAX) : -1;
            if (n < 0) {
                fail(FS_EIO);
            } else {
                r = n < (int)cap ? n : (int)cap;
                memcpy(buf, t, (uint32_t)r);
            }
            kfree(t);
        }
    }
    irq_restore(flags);
    return r;
}

static int t4_access(int cwd, const char *path, int mode)
{
    irqflags_t flags = irq_save();
    static struct inode in;
    int r = -1;
    uint32_t ino = resolve(cwd, path);
    if (ino) {
        if (load_inode(ino, &in) < 0)
            fail(FS_EIO);
        else if ((mode & 2) && !fs.writable)
            fail(FS_EROFS);
        else if (mode & 7 && !permits(&in, mode & 7))
            fail(FS_EACCES);
        else
            r = 0;
    }
    irq_restore(flags);
    return r;
}

static int t4_resolve(int cwd, const char *path, int follow)
{
    irqflags_t flags = irq_save();
    uint32_t ino = resolve_in(ino_of_slot(cwd), path, (uint32_t)strlen(path),
                              follow, 0);
    irq_restore(flags);
    return ino ? slot_of_ino(ino) : -2;
}

const struct fs_backend tfs4_backend = {
    .name = "TFS4",
    .open = t4_open,
    .mkdir = t4_mkdir,
    .chdir = t4_chdir,
    .unlink = t4_unlink,
    .rename = t4_rename,
    .getpath = t4_getpath,
    .stat_path = t4_stat_path,
    .stat_slot = t4_stat_slot,
    .lstat_path = t4_lstat_path,
    .open_mode = t4_open_mode,
    .mkdir_mode = t4_mkdir_mode,
    .chmod = t4_chmod,
    .chown = t4_chown,
    .utimes = t4_utimes,
    .link = t4_link,
    .symlink = t4_symlink,
    .readlink = t4_readlink,
    .access = t4_access,
    .resolve = t4_resolve,
    .readdir_at = t4_readdir_at,
    .size = t4_size,
    .owner = t4_owner,
    .can_write = t4_can_write,
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
    fs.free_inodes = rd32(sb + 16);
    fs.first_data = rd32(sb + 20);
    fs.bpg = rd32(sb + 32);
    fs.ipg = rd32(sb + 40);
    fs.isize = rev >= 1 ? rd16(sb + 88) : 128;
    fs.first_ino = rev >= 1 ? rd32(sb + 84) : FIRST_INO;
    fs.compat = rev >= 1 ? rd32(sb + 92) : 0;
    fs.incompat = rev >= 1 ? rd32(sb + 96) : 0;
    fs.ro_compat = rev >= 1 ? rd32(sb + 100) : 0;
    fs.last_orphan = rd32(sb + 232);
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
        fs.free_blocks > fs.blocks || fs.free_inodes > fs.inodes ||
        fs.first_ino < 3 || fs.first_ino > fs.inodes ||
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
        gd[g].free_blocks = rd16(d + 12);
        gd[g].free_inodes = rd16(d + 14);
        gd[g].used_dirs = rd16(d + 16);
        gd[g].dirty = 0;
        uint32_t table_blocks = (fs.ipg * fs.isize + fs.bs - 1) / fs.bs;
        if (gd[g].block_bitmap >= fs.blocks ||
            gd[g].inode_bitmap >= fs.blocks || !gd[g].inode_table ||
            gd[g].inode_table >= fs.blocks ||
            fs.blocks - gd[g].inode_table < table_blocks ||
            gd[g].free_blocks > fs.bpg || gd[g].free_inodes > fs.ipg) {
            *why = "a group descriptor pointing outside the file system";
            return 0;
        }
    }
    memset(icache, 0, sizeof(icache));
    memset(&bmap_b, 0, sizeof(bmap_b));
    memset(&bmap_i, 0, sizeof(bmap_i));
    pcache_drop();
    dblk_no = 0;
    fs.mounted = 1;
    static struct inode root;
    if (load_inode(ROOT_INO, &root) < 0 || !is_dir_mode(&root)) {
        fs.mounted = 0;
        *why = "its root is not a directory";
        return 0;
    }
    /* written only where every feature it uses is one this writes too:
       no journal (ext3's, stage 7), nothing in ro_compat past sparse
       superblocks and large files */
    fs.writable = !(fs.compat & COMPAT_HAS_JOURNAL) &&
                  !(fs.ro_compat & ~RO_WRITABLE);
    if (fs.writable) {
        /* mounted, so not cleanly unmounted until it is: whoever finds
           the disk after a crash checks it */
        if (blk_read(base + 2, 2, sb) == 0) {
            wr16(sb + 58, rd16(sb + 58) & ~STATE_VALID);
            wr32(sb + 44, now());
            wr16(sb + 52, rd16(sb + 52) + 1);
            blk_write(base + 2, 2, sb);
        }
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

/* Files unlinked while open when the system stopped: nobody holds
   them now. Each goes, as its last close would have done. */
static void orphans_release(void)
{
    static struct inode in;
    int n = 0;
    for (int guard = 0; fs.last_orphan && guard < 65536; guard++) {
        uint32_t ino = fs.last_orphan;
        if (!ino_valid(ino) || load_inode(ino, &in) < 0 || in.links) {
            fs.last_orphan = 0;            /* a list that does not hold */
            fs.sb_dirty = 1;
            break;
        }
        fs.last_orphan = in.dtime;
        fs.sb_dirty = 1;
        release(&in);
        n++;
    }
    meta_flush();
    if (n)
        printk("[ OK ] TFS4: %u file(s) unlinked while open let go\n",
               (uint32_t)n);
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
    printk("[ OK ] TFS4: %u blocks of %u B, %u inodes, from sector %u%s\n",
           fs.blocks, fs.bs, fs.inodes, fs.base,
           fs.writable ? "" : " (read-only)");
    if (fs.writable)
        orphans_release();
    return 1;
}

/* --- formatting --------------------------------------------------------------
 * What mke2fs -t ext2 makes, less what this does not keep: revision 1,
 * 4 KiB blocks, 256-byte inodes (one per 16 KiB of disk), entries with
 * their file's type, superblock copies only in groups 0, 1 and powers of
 * 3, 5 and 7, large files; the root and lost+found, which e2fsck wants
 * to find. Every bitmap's bits past its group's end are set, as the
 * format says. */

static int has_backup(uint32_t g)
{
    if (g <= 1)
        return 1;
    for (uint32_t p = 3; p <= 7; p += 2) {
        uint32_t x = g;
        while (x % p == 0)
            x /= p;
        if (x == 1)
            return 1;
    }
    return 0;
}

int tfs4_format(uint32_t base, uint32_t sectors)
{
    const uint32_t bs = 4096, spb = 8, isize = 256, bpg = bs * 8;
    uint64_t total = sectors / spb;
    if (total > 0xFFFFFFFFull)
        total = 0xFFFFFFFFull;
    uint32_t blocks = (uint32_t)total;
    if (blocks < 256)
        return 0;
    uint32_t groups = (blocks + bpg - 1) / bpg;
    uint64_t want_inodes = (uint64_t)blocks * bs / 16384;
    uint32_t ipg = (uint32_t)((want_inodes + groups - 1) / groups);
    ipg = (ipg + 15) & ~15u;              /* whole inode table blocks */
    if (ipg < 32)
        ipg = 32;
    if (ipg > bs * 8)
        ipg = bs * 8;
    uint32_t itb = ipg * isize / bs;
    uint32_t gdtb = (groups * 32 + bs - 1) / bs;
    /* a last group too short for its own bookkeeping is left off */
    uint32_t last = blocks - (groups - 1) * bpg;
    uint32_t over = (has_backup(groups - 1) ? 1 + gdtb : 0) + 2 + itb;
    if (groups > 1 && last < over + 16) {
        groups--;
        blocks = groups * bpg;
        gdtb = (groups * 32 + bs - 1) / bs;
    }
    if (blocks < over + 16)
        return 0;

    uint8_t *buf = kmalloc(bs), *gdt = kmalloc(gdtb * bs);
    struct { uint32_t bb, ib, it, free_b, free_i, dirs, first_free; } *g =
        kmalloc(groups * sizeof(*g));
    if (!buf || !gdt || !g) {
        kfree(buf);
        kfree(gdt);
        kfree(g);
        return 0;
    }
#define PUT(b, data) blk_write(base + (b) * spb, spb, (data))
    int bad = 0;
    uint32_t free_blocks = 0;
    for (uint32_t i = 0; i < groups; i++) {
        uint32_t start = i * bpg;
        uint32_t at = start + (has_backup(i) ? 1 + gdtb : 0);
        g[i].bb = at;
        g[i].ib = at + 1;
        g[i].it = at + 2;
        g[i].first_free = at + 2 + itb;
        uint32_t n = i == groups - 1 ? blocks - start : bpg;
        g[i].free_b = n - (g[i].first_free - start);
        g[i].free_i = ipg;
        g[i].dirs = 0;
    }
    /* the root and lost+found: a block each, the first free of group 0;
       inodes 1 to 11 taken (the reserved ones, the root, lost+found) */
    uint32_t root_blk = g[0].first_free, lf_blk = root_blk + 1;
    g[0].free_b -= 2;
    g[0].free_i -= FIRST_INO;
    g[0].dirs = 2;
    for (uint32_t i = 0; i < groups; i++)
        free_blocks += g[i].free_b;
    uint32_t inodes = ipg * groups;
    uint32_t free_inodes = inodes - FIRST_INO;
    uint32_t t = now();

    /* the inode tables zeroed, the bitmaps written */
    memset(buf, 0, bs);
    for (uint32_t i = 0; i < groups && !bad; i++)
        for (uint32_t k = 0; k < itb && !bad; k++)
            bad |= PUT(g[i].it + k, buf) < 0;
    for (uint32_t i = 0; i < groups && !bad; i++) {
        uint32_t start = i * bpg;
        uint32_t n = i == groups - 1 ? blocks - start : bpg;
        memset(buf, 0, bs);
        uint32_t used = g[i].first_free - start + (i == 0 ? 2 : 0);
        for (uint32_t b = 0; b < bs * 8; b++)
            if (b < used || b >= n)
                buf[b / 8] |= (uint8_t)(1u << (b % 8));
        bad |= PUT(g[i].bb, buf) < 0;
        memset(buf, 0, bs);
        for (uint32_t b = 0; b < bs * 8; b++)
            if ((i == 0 && b < FIRST_INO) || b >= ipg)
                buf[b / 8] |= (uint8_t)(1u << (b % 8));
        bad |= PUT(g[i].ib, buf) < 0;
    }

    /* the root's directory block: ".", "..", lost+found */
    memset(buf, 0, bs);
    wr32(buf, ROOT_INO); wr16(buf + 4, 12); buf[6] = 1; buf[7] = FT_DIR;
    buf[8] = '.';
    wr32(buf + 12, ROOT_INO); wr16(buf + 16, 12); buf[18] = 2;
    buf[19] = FT_DIR; buf[20] = '.'; buf[21] = '.';
    wr32(buf + 24, LOST_FOUND_INO); wr16(buf + 28, bs - 24); buf[30] = 10;
    buf[31] = FT_DIR; memcpy(buf + 32, "lost+found", 10);
    bad |= PUT(root_blk, buf) < 0;
    memset(buf, 0, bs);
    wr32(buf, LOST_FOUND_INO); wr16(buf + 4, 12); buf[6] = 1;
    buf[7] = FT_DIR; buf[8] = '.';
    wr32(buf + 12, ROOT_INO); wr16(buf + 16, bs - 12); buf[18] = 2;
    buf[19] = FT_DIR; buf[20] = '.'; buf[21] = '.';
    bad |= PUT(lf_blk, buf) < 0;

    /* their inodes, in the first block of group 0's table */
    memset(buf, 0, bs);
    for (int k = 0; k < 2; k++) {
        uint32_t ino = k ? LOST_FOUND_INO : ROOT_INO;
        uint8_t *p = buf + (ino - 1) * isize;
        /* the root as /tmp: anybody makes files in it, only their
           owners take them away (TFS3's root let everybody write) */
        wr16(p, S_IFDIR | (k ? 0700 : 01777));
        wr32(p + 4, bs);
        wr32(p + 8, t);
        wr32(p + 12, t);
        wr32(p + 16, t);
        wr16(p + 26, k ? 2 : 3);
        wr32(p + 28, bs / 512);
        wr32(p + 40, k ? lf_blk : root_blk);
        wr16(p + 128, 32);
    }
    bad |= PUT(g[0].it, buf) < 0;

    /* the group descriptors */
    memset(gdt, 0, gdtb * bs);
    for (uint32_t i = 0; i < groups; i++) {
        uint8_t *d = gdt + i * 32;
        wr32(d, g[i].bb);
        wr32(d + 4, g[i].ib);
        wr32(d + 8, g[i].it);
        wr16(d + 12, g[i].free_b);
        wr16(d + 14, g[i].free_i);
        wr16(d + 16, g[i].dirs);
    }

    /* the superblock, and its copies with the descriptors' */
    static uint8_t sb[1024];
    memset(sb, 0, sizeof(sb));
    wr32(sb, inodes);
    wr32(sb + 4, blocks);
    wr32(sb + 8, blocks / 20);                /* reserved, as mke2fs */
    wr32(sb + 12, free_blocks);
    wr32(sb + 16, free_inodes);
    wr32(sb + 20, 0);                         /* first data block */
    wr32(sb + 24, 2);                         /* 1024 << 2 */
    wr32(sb + 28, 2);
    wr32(sb + 32, bpg);
    wr32(sb + 36, bpg);
    wr32(sb + 40, ipg);
    wr32(sb + 48, t);                         /* written */
    wr16(sb + 54, 0xFFFF);                    /* no forced checks */
    wr16(sb + 56, EXT2_MAGIC);
    wr16(sb + 58, STATE_VALID);
    wr16(sb + 60, 1);                         /* on errors: carry on */
    wr32(sb + 64, t);                         /* last checked */
    wr32(sb + 76, 1);                         /* revision 1 */
    wr32(sb + 84, FIRST_INO);
    wr16(sb + 88, isize);
    wr32(sb + 96, INCOMPAT_FILETYPE);
    wr32(sb + 100, RO_SPARSE_SUPER | RO_LARGE_FILE);
    random_bytes(sb + 104, 16);               /* the file system's UUID */
    sb[104 + 6] = (uint8_t)((sb[104 + 6] & 0x0F) | 0x40);
    sb[104 + 8] = (uint8_t)((sb[104 + 8] & 0x3F) | 0x80);
    memcpy(sb + 120, "ThreadOS", 9);
    wr16(sb + 0x15C, 32);                     /* inode extra sizes */
    wr16(sb + 0x15E, 32);
    for (uint32_t i = 0; i < groups && !bad; i++) {
        if (!has_backup(i))
            continue;
        wr16(sb + 90, i);                     /* which copy */
        memset(buf, 0, bs);
        memcpy(buf + (i == 0 ? 1024 : 0), sb, 1024);
        bad |= PUT(i * bpg, buf) < 0;
        for (uint32_t k = 0; k < gdtb && !bad; k++)
            bad |= PUT(i * bpg + 1 + k, gdt + k * bs) < 0;
    }
#undef PUT
    kfree(buf);
    kfree(gdt);
    kfree(g);
    if (bad)
        return 0;
    printk("[ OK ] TFS4: formatted %u blocks of %u B, %u inodes, from "
           "sector %u\n", blocks, bs, inodes, base);
    return 1;
}
