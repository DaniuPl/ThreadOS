/* TFS3 - the ThreadOS File System with block allocation
 *
 * Frozen: TFS4 (the ext2 format, docs/tfs4-plan.md) takes its place,
 * and this stays behind the front (kernel/fs.c) for the disks it is on.
 *
 * The previous version gave every file a fixed area of 32 sectors: no
 * fragmentation, but a hard ceiling of 16 KiB and 16 files. Since the
 * system started pulling things off the network that is too little -
 * so blocks are allocated from a bitmap and an inode holds their list.
 *
 * Disk layout (block = 1 KiB = 2 sectors):
 *   block 0        - the superblock (magic "TFS3")
 *   blocks 1..4    - the block bitmap (4 KiB = 32768 bits)
 *   blocks 5..12   - the inode table (64 x 128 B)
 *   blocks 13..    - data
 *
 * A file has 12 direct blocks, an indirect block (256 pointers) and a
 * doubly indirect block (256 blocks of 256 pointers), that is up to
 * 64 MiB. Directories are inodes of type DIR; every inode remembers
 * its parent's slot (FS_ROOT = -1 for the root). Paths are resolved by
 * walking the components ("." and ".." are handled).
 *
 * The bitmap and the inode table sit in memory and are written to disk
 * on every change - at this scale that is simpler and safer than a
 * cache with deferred writes. Disk operations run with interrupts
 * disabled.
 */

#include "include/tfs3.h"
#include "include/ata.h"
#include "include/blk.h"
#include "include/part.h"
#include "include/string.h"
#include "include/printk.h"
#include "include/io.h"
#include "include/task.h"
#include "include/user.h"
#include "../kernel64/task64.h"
#define current_level task64_level
#define current_uid task64_uid
#define slot_open task64_slot_open

#define MAGIC          "TFS3"
#define SECTORS_PER_BLOCK (TFS3_BLOCK / ATA_SECTOR_SIZE)

#define BITMAP_START   1
#define BITMAP_BLOCKS  4
#define BITMAP_BITS    (BITMAP_BLOCKS * TFS3_BLOCK * 8)
#define INODE_START    (BITMAP_START + BITMAP_BLOCKS)
#define INODE_SIZE     128
#define INODE_BLOCKS   (TFS3_MAX_FILES * INODE_SIZE / TFS3_BLOCK)
#define DATA_START     (INODE_START + INODE_BLOCKS)

#define FS_TYPE_FILE 0
#define FS_TYPE_DIR  1
#define FS_TYPE_MASK 1
#define FS_OWNER_SHIFT 1
#define FS_NOTFOUND  (-2)
/* The parent of a file unlinked while still open: it has no name and
   is in no directory, and its blocks go when the last descriptor on it
   closes (fs_slot_closed). One left on the disk by a crash is freed at
   the next mount. */
#define FS_ORPHAN    (-3)

struct inode {
    char name[TFS3_NAME_MAX + 1];   /* 64 B, with a NUL */
    uint32_t size;                /* 4 B */
    uint8_t used;                 /* 1 B */
    uint8_t type;                 /* low bit: type; high bits: owner UID */
    int16_t parent;               /* 2 B: the parent's slot, -1 = root */
    uint32_t direct[TFS3_DIRECT];   /* 48 B: block numbers */
    uint32_t indirect;            /* 4 B: a block of pointers (0 = none) */
    uint32_t dindirect;           /* 4 B: a block of blocks of pointers */
} __attribute__((packed));        /* 128 B -> 8 inodes per block */

struct superblock {
    char magic[8];
    uint32_t ninodes;
    uint32_t nblocks;             /* how many blocks the bitmap covers */
    uint32_t data_start;
} __attribute__((packed));

/* The on-disk layout depends on the inode size - let the compiler watch it */
_Static_assert(sizeof(struct inode) == INODE_SIZE,
               "an inode has to be exactly INODE_SIZE bytes");

static int ready;
static struct inode inodes[TFS3_MAX_FILES];
static uint8_t bitmap[BITMAP_BLOCKS * TFS3_BLOCK];
static uint32_t total_blocks;
static uint8_t block_buf[TFS3_BLOCK];

/* --- block access ------------------------------------------------------ */

/* Where this filesystem begins on the disk.
 *
 * Zero means "the whole disk is ours", which is what every disk this
 * system had until now and what its own images still look like. A
 * laptop's disk is not like that: sector zero there belongs to a
 * partition table, and living on such a machine means living INSIDE a
 * partition, starting wherever that partition starts.
 *
 * The offset lives here, in the two functions that touch the disk, and
 * nowhere else - so that no code above has to know whether it is on a
 * whole disk or a slice of one. */
static uint32_t fs_base;

/* A whole block in one command: with AHCI that is a single DMA
   transfer instead of two separate per-sector requests */
/* Every block the filesystem touches goes through these two, and every
   block number in an inode or a pointer block is the disk's own word:
   one past the filesystem's end would read or write whatever lies
   behind it - another partition, the end of somebody else's disk - and
   a large one wraps the 32-bit LBA round to the front, where the
   partition table is. Only blocks of the filesystem itself. */
/* A TFS3 read from a copy in memory, not the disk: the migration to
   TFS4 formats the partition the TFS3 was on and reads its files from
   the copy taken first (kernel/tfs3to4.c). Nothing is written then. */
static const uint8_t *image;

static int block_read(uint32_t block, void *buf)
{
    if (block >= total_blocks)
        return -1;
    if (image) {
        memcpy(buf, image + block * TFS3_BLOCK, TFS3_BLOCK);
        return 0;
    }
    return blk_read(fs_base + block * SECTORS_PER_BLOCK, SECTORS_PER_BLOCK,
                    buf);
}

static int block_write(uint32_t block, const void *buf)
{
    if (block >= total_blocks || image)
        return -1;
    return blk_write(fs_base + block * SECTORS_PER_BLOCK, SECTORS_PER_BLOCK,
                     buf);
}

static void flush_inodes(void)
{
    const uint8_t *src = (const uint8_t *)inodes;
    for (uint32_t i = 0; i < INODE_BLOCKS; i++)
        block_write(INODE_START + i, src + i * TFS3_BLOCK);
}

/* Writes only the block holding a given inode. While a file is being
   written the inode changes with every data block, so rewriting the
   whole table (8 KiB through PIO) would cost more than the data
   itself. */
static void flush_inode(int slot)
{
    uint32_t blk = (uint32_t)slot * INODE_SIZE / TFS3_BLOCK;
    const uint8_t *src = (const uint8_t *)inodes;
    block_write(INODE_START + blk, src + blk * TFS3_BLOCK);
}

static void flush_bitmap(void)
{
    for (uint32_t i = 0; i < BITMAP_BLOCKS; i++)
        block_write(BITMAP_START + i, bitmap + i * TFS3_BLOCK);
}

/* As above: when allocating a block it is enough to write the piece of
   the bitmap in which a bit changed */
static void flush_bitmap_bit(uint32_t b)
{
    uint32_t blk = b / 8 / TFS3_BLOCK;
    block_write(BITMAP_START + blk, bitmap + blk * TFS3_BLOCK);
}

/* --- the block bitmap --------------------------------------------------- */

static int bit_get(uint32_t b)
{
    return (bitmap[b / 8] >> (b % 8)) & 1;
}

static void bit_set(uint32_t b, int v)
{
    if (v)
        bitmap[b / 8] |= (uint8_t)(1 << (b % 8));
    else
        bitmap[b / 8] &= (uint8_t)~(1 << (b % 8));
}

/* Allocates a zeroed block; 0 = out of space (block 0 is the superblock) */
static uint32_t block_alloc(void)
{
    for (uint32_t b = DATA_START; b < total_blocks; b++) {
        if (bit_get(b))
            continue;
        bit_set(b, 1);
        flush_bitmap_bit(b);
        memset(block_buf, 0, TFS3_BLOCK);
        block_write(b, block_buf);
        return b;
    }
    return 0;
}

static void block_free(uint32_t b)
{
    if (b < DATA_START || b >= total_blocks)
        return;
    bit_set(b, 0);
    flush_bitmap_bit(b);
}

/* Frees all of a file's blocks */
static void inode_free_blocks(struct inode *in)
{
    for (int i = 0; i < TFS3_DIRECT; i++) {
        if (in->direct[i]) {
            block_free(in->direct[i]);
            in->direct[i] = 0;
        }
    }
    static uint32_t ptrs[TFS3_INDIRECT];
    static uint32_t ptrs2[TFS3_INDIRECT];

    if (in->indirect) {
        if (block_read(in->indirect, ptrs) == 0)
            for (uint32_t i = 0; i < TFS3_INDIRECT; i++)
                if (ptrs[i])
                    block_free(ptrs[i]);
        block_free(in->indirect);
        in->indirect = 0;
    }
    if (in->dindirect) {
        if (block_read(in->dindirect, ptrs) == 0) {
            for (uint32_t i = 0; i < TFS3_INDIRECT; i++) {
                if (!ptrs[i])
                    continue;
                if (block_read(ptrs[i], ptrs2) == 0)
                    for (uint32_t j = 0; j < TFS3_INDIRECT; j++)
                        if (ptrs2[j])
                            block_free(ptrs2[j]);
                block_free(ptrs[i]);
            }
        }
        block_free(in->dindirect);
        in->dindirect = 0;
    }
    in->size = 0;
}

/* The disk block number for a file's n-th block; alloc=1 allocates a
   missing one. Returns 0 when the block does not exist (or cannot be
   allocated). */
static uint32_t inode_block(struct inode *in, uint32_t n, int alloc)
{
    if (n < TFS3_DIRECT) {
        if (!in->direct[n] && alloc)
            in->direct[n] = block_alloc();
        return in->direct[n];
    }

    n -= TFS3_DIRECT;

    static uint32_t ptrs[TFS3_INDIRECT];
    static uint32_t ptrs2[TFS3_INDIRECT];

    if (n < TFS3_INDIRECT) {               /* the indirect block */
        if (!in->indirect) {
            if (!alloc)
                return 0;
            in->indirect = block_alloc();
            if (!in->indirect)
                return 0;
        }
        if (block_read(in->indirect, ptrs) < 0)
            return 0;
        if (!ptrs[n]) {
            if (!alloc)
                return 0;
            ptrs[n] = block_alloc();
            if (!ptrs[n])
                return 0;
            block_write(in->indirect, ptrs);
        }
        return ptrs[n];
    }

    n -= TFS3_INDIRECT;                    /* the doubly indirect block */
    if (n >= (uint32_t)TFS3_DINDIRECT)
        return 0;                        /* beyond the file size ceiling */

    uint32_t hi = n / TFS3_INDIRECT, lo = n % TFS3_INDIRECT;

    if (!in->dindirect) {
        if (!alloc)
            return 0;
        in->dindirect = block_alloc();
        if (!in->dindirect)
            return 0;
    }
    if (block_read(in->dindirect, ptrs) < 0)
        return 0;
    if (!ptrs[hi]) {
        if (!alloc)
            return 0;
        ptrs[hi] = block_alloc();
        if (!ptrs[hi])
            return 0;
        block_write(in->dindirect, ptrs);
    }
    if (block_read(ptrs[hi], ptrs2) < 0)
        return 0;
    if (!ptrs2[lo]) {
        if (!alloc)
            return 0;
        ptrs2[lo] = block_alloc();
        if (!ptrs2[lo])
            return 0;
        block_write(ptrs[hi], ptrs2);
    }
    return ptrs2[lo];
}

/* --- startup and formatting --------------------------------------------- */

static void format_disk(void)
{
    struct superblock sb;
    memset(&sb, 0, sizeof(sb));
    memcpy(sb.magic, MAGIC, 4);
    sb.ninodes = TFS3_MAX_FILES;
    sb.nblocks = total_blocks;
    sb.data_start = DATA_START;

    memset(block_buf, 0, TFS3_BLOCK);
    memcpy(block_buf, &sb, sizeof(sb));
    block_write(0, block_buf);

    memset(bitmap, 0, sizeof(bitmap));
    for (uint32_t b = 0; b < DATA_START; b++)  /* metadata taken */
        bit_set(b, 1);
    flush_bitmap();

    memset(inodes, 0, sizeof(inodes));
    flush_inodes();
    printk("[ OK ] TFS: an empty disk formatted (%u blocks of %u B)\n",
           total_blocks, TFS3_BLOCK);
}

/* Fits the filesystem to a stretch of disk: where it starts and how
   many blocks it gets. Returns 0 when that stretch is too small to
   hold a filesystem at all. */
static int fit(uint32_t start, uint32_t sectors)
{
    fs_base = start;
    total_blocks = sectors / SECTORS_PER_BLOCK;
    if (total_blocks > BITMAP_BITS)
        total_blocks = BITMAP_BITS;        /* this much the bitmap covers */
    return total_blocks > DATA_START;
}

/* The bitmap and the inode table off the disk, and what a crash left
   behind cleared: files unlinked while open, whose last close never
   came */
static void load_tables(void)
{
    for (uint32_t i = 0; i < BITMAP_BLOCKS; i++)
        block_read(BITMAP_START + i, bitmap + i * TFS3_BLOCK);
    uint8_t *dst = (uint8_t *)inodes;
    for (uint32_t i = 0; i < INODE_BLOCKS; i++)
        block_read(INODE_START + i, dst + i * TFS3_BLOCK);
    int orphans = 0;
    for (int i = 0; i < TFS3_MAX_FILES; i++) {
        if (inodes[i].used && inodes[i].parent == FS_ORPHAN) {
            inode_free_blocks(&inodes[i]);
            inodes[i].used = 0;
            orphans++;
        }
    }
    if (orphans)
        flush_inodes();
}

/* Formats because somebody asked in so many words. 'partition' is 0
   for the whole disk, or a partition number counted from one - and
   that second case is the one that matters on a machine that belongs
   to somebody: the table and every other partition are left exactly
   as they were. Returns 1 when it happened. */
int tfs3_format(int partition)
{
    if (!blk_present())
        return 0;

    if (partition <= 0) {
        if (!fit(0, blk_sectors()))
            return 0;
    } else {
        struct partition pt;
        if (part_scan() < partition || !part_get(partition - 1, &pt))
            return 0;
        if (pt.start > 0xFFFFFFFFu || pt.sectors > 0xFFFFFFFFu)
            return 0;                      /* past what our LBAs reach */
        if (!fit((uint32_t)pt.start, (uint32_t)pt.sectors))
            return 0;
    }
    format_disk();
    ready = 1;
    return 1;
}

/* Looks for a filesystem of ours inside one of the partitions.
   Returns the partition number (from one) or 0. */
static int find_our_partition(void)
{
    int n = part_scan();
    for (int i = 0; i < n; i++) {
        struct partition pt;
        if (!part_get(i, &pt))
            break;
        if (pt.start > 0xFFFFFFFFu || pt.sectors > 0xFFFFFFFFu)
            continue;
        if (!fit((uint32_t)pt.start, (uint32_t)pt.sectors))
            continue;
        if (block_read(0, block_buf) < 0)
            continue;
        if (memcmp(((struct superblock *)block_buf)->magic, MAGIC, 4) == 0)
            return i + 1;
    }
    fs_base = 0;
    return 0;
}

/* Where a TFS3 is: the whole disk, or a partition found by its
   superblock. Nothing is mounted or written. 1 when there is one. */
int tfs3_probe(struct tfs3_where *w)
{
    image = 0;
    if (!blk_present())
        return 0;
    w->partition = 0;
    if (fit(0, blk_sectors()) && block_read(0, block_buf) == 0 &&
        memcmp(((struct superblock *)block_buf)->magic, MAGIC, 4) == 0) {
        w->base = 0;
        w->sectors = blk_sectors();
    } else {
        int p = find_our_partition();
        if (!p)
            return 0;
        struct partition pt;
        part_get(p - 1, &pt);
        w->partition = p;
        w->base = (uint32_t)pt.start;
        w->sectors = (uint32_t)pt.sectors;
    }
    /* how much of it TFS3 uses: its bitmap's reach, at most 32 MiB */
    struct superblock *sb = (struct superblock *)block_buf;
    if (block_read(0, block_buf) < 0)
        return 0;
    w->blocks = sb->nblocks && sb->nblocks <= total_blocks ? sb->nblocks
                                                           : total_blocks;
    w->bytes = w->blocks * TFS3_BLOCK;
    return 1;
}

int tfs3_mount_at(uint32_t base, uint32_t sectors)
{
    image = 0;
    if (!fit(base, sectors))
        return 0;
    if (block_read(0, block_buf) < 0 ||
        memcmp(((struct superblock *)block_buf)->magic, MAGIC, 4) != 0)
        return 0;
    load_tables();
    ready = 1;
    return 1;
}

/* The TFS3 of a copy in memory, to read: 'len' bytes from its block 0 */
int tfs3_open_image(const void *img, uint32_t len)
{
    const struct superblock *sb = img;
    if (len < TFS3_BLOCK * 16 || memcmp(sb->magic, MAGIC, 4) != 0)
        return 0;
    image = img;
    fs_base = 0;
    total_blocks = len / TFS3_BLOCK;
    if (total_blocks > BITMAP_BITS)
        total_blocks = BITMAP_BITS;
    for (uint32_t i = 0; i < BITMAP_BLOCKS; i++)
        block_read(BITMAP_START + i, bitmap + i * TFS3_BLOCK);
    uint8_t *dst = (uint8_t *)inodes;
    for (uint32_t i = 0; i < INODE_BLOCKS; i++)
        block_read(INODE_START + i, dst + i * TFS3_BLOCK);
    ready = 1;
    return 1;
}

void tfs3_close(void)
{
    image = 0;
    ready = 0;
}

static uint32_t t3_block_size(void)
{
    return TFS3_BLOCK;
}

static void t3_usage(uint64_t *used, uint64_t *total)
{
    uint32_t u = 0;
    for (uint32_t b = 0; b < total_blocks; b++)
        if (bit_get(b))
            u++;
    if (used)
        *used = u;
    if (total)
        *total = total_blocks;
}

/* --- Helpers: directories and path resolution --- */

static int alloc_inode(void)
{
    for (int i = 0; i < TFS3_MAX_FILES; i++)
        if (!inodes[i].used)
            return i;
    return -1;
}

static int is_dir(int slot)
{
    return slot == FS_ROOT || (slot >= 0 &&
           (inodes[slot].type & FS_TYPE_MASK) == FS_TYPE_DIR);
}

static int inode_type(const struct inode *in)
{
    return in->type & FS_TYPE_MASK;
}

static int inode_owner(const struct inode *in)
{
    return in->type >> FS_OWNER_SHIFT;
}

/* The files in the root that only an administrator may create, write,
   rename or delete. user.c keeps the list of them; the dock's list is
   one more, because the desktop starts the programs it names for
   whoever clicks a tile, an administrator included - a plain account
   that could write it could have its own program run with an
   administrator's powers. */
static int system_file(const char *name)
{
    return user_is_system_file(name) || !strcmp(name, "desktop.icons");
}

static int can_access(int slot)
{
    if (slot < 0 || current_level() == USER_ADMIN)
        return 1;
    /* UID 0 owns the machine's files, but dropping the admin session
       must still make those files read-only to that account. Check the
       resolved inode name here, after path components like '..' have
       been handled by resolve(). */
    if (inodes[slot].parent == FS_ROOT &&
        system_file(inodes[slot].name))
        return 0;
    return inode_owner(&inodes[slot]) == current_uid();
}

static int can_read(int slot)
{
    if (can_access(slot))
        return 1;
    /* Machine settings and icons are public; account hashes and swap data
       stay private even though they are also system-owned files. So is
       the key updates are checked against: it is a public key, and
       'update check' run by anybody has to check with the one the
       admin chose rather than fall back to the built-in one. And the
       dock's list is read by the desktop as whoever is signed in - and
       before anybody is; system_file() keeps the writing of it to
       administrators. */
    return slot >= 0 && inode_type(&inodes[slot]) == FS_TYPE_FILE &&
           inodes[slot].parent == FS_ROOT &&
           (!strcmp(inodes[slot].name, "system.cfg") ||
            !strcmp(inodes[slot].name, "icons.pos") ||
            !strcmp(inodes[slot].name, "update.key") ||
            !strcmp(inodes[slot].name, "desktop.icons"));
}

static int parent_of(int slot)
{
    return slot == FS_ROOT ? FS_ROOT : inodes[slot].parent;
}

/* Looks for an entry named s (length len) in directory 'parent';
   -1 when there is none */
static int dir_find(int parent, const char *s, int len)
{
    if (len <= 0 || len > TFS3_NAME_MAX)
        return -1;
    for (int i = 0; i < TFS3_MAX_FILES; i++) {
        if (!inodes[i].used || inodes[i].parent != parent)
            continue;
        if ((int)strlen(inodes[i].name) == len &&
            memcmp(inodes[i].name, s, len) == 0)
            return i;
    }
    return -1;
}

/* Resolves the first n bytes of a path to a slot (>=0 or FS_ROOT) or
   FS_NOTFOUND - a path's directory part is resolved in place, with no
   copy of it to fit into a buffer */
static int resolve_n(int cwd, const char *path, uint32_t n)
{
    int cur = (n && path[0] == '/') ? FS_ROOT : cwd;
    const char *p = path, *end = path + n;
    while (p < end) {
        while (p < end && *p == '/')
            p++;
        if (p == end)
            break;
        const char *s = p;
        while (p < end && *p != '/')
            p++;
        int len = (int)(p - s);
        if (len == 1 && s[0] == '.')
            continue;
        if (len == 2 && s[0] == '.' && s[1] == '.') {
            cur = parent_of(cur);
            continue;
        }
        if (!is_dir(cur))
            return FS_NOTFOUND;
        int nx = dir_find(cur, s, len);
        if (nx < 0)
            return FS_NOTFOUND;
        cur = nx;
    }
    return cur;
}

static int resolve(int cwd, const char *path)
{
    return resolve_n(cwd, path, (uint32_t)strlen(path));
}

/* Resolves the parent directory of the last component and extracts the
   base name. Returns the directory's slot (>=0 or FS_ROOT) or
   FS_NOTFOUND. */
static int resolve_parent(int cwd, const char *path, char *base)
{
    const char *last = (void *)0;
    for (const char *p = path; *p; p++)
        if (*p == '/')
            last = p;

    const char *bname;
    int dir;
    if (!last) {
        dir = cwd;
        bname = path;
    } else {
        uint32_t n = (uint32_t)(last - path);
        dir = (n == 0) ? FS_ROOT : resolve_n(cwd, path, n);  /* "/x" -> root */
        bname = last + 1;
    }
    if (dir == FS_NOTFOUND || !is_dir(dir))
        return FS_NOTFOUND;

    int bl = (int)strlen(bname);
    if (bl <= 0 || bl > TFS3_NAME_MAX)
        return FS_NOTFOUND;
    memcpy(base, bname, bl);
    base[bl] = '\0';
    return dir;
}

static int create_inode(int dir, const char *base, int type)
{
    int idx = alloc_inode();
    if (idx < 0)
        return -1;
    memset(&inodes[idx], 0, sizeof(inodes[idx]));
    memcpy(inodes[idx].name, base, strlen(base));
    inodes[idx].used = 1;
    int owner = current_uid();
    if (dir == FS_ROOT && system_file(base))
        owner = 0;
    inodes[idx].type = (uint8_t)((owner << FS_OWNER_SHIFT) | type);
    inodes[idx].parent = (int16_t)dir;
    flush_inodes();
    return idx;
}

/* --- Public path operations --- */

static int t3_open(int cwd, const char *path, int create)
{
    if (!ready)
        return -1;

    uint32_t flags = irq_save();
    int slot = resolve(cwd, path);
    if (slot >= 0 && inode_type(&inodes[slot]) == FS_TYPE_FILE) {
        if (!can_read(slot)) {
            irq_restore(flags);
            return -1;
        }
        irq_restore(flags);
        return slot;                       /* an existing file */
    }
    if (slot == FS_ROOT || (slot >= 0 && is_dir(slot))) {
        irq_restore(flags);
        return -1;                         /* that is a directory, not a file */
    }
    if (!create) {
        irq_restore(flags);
        return -1;
    }

    char base[TFS3_NAME_MAX + 1];
    int dir = resolve_parent(cwd, path, base);
    if (dir == FS_NOTFOUND || !can_access(dir) ||
        (dir == FS_ROOT && system_file(base) &&
         current_level() != USER_ADMIN)) {
        irq_restore(flags);
        return -1;
    }
    int idx = create_inode(dir, base, FS_TYPE_FILE);
    irq_restore(flags);
    return idx;
}

static int t3_mkdir(int cwd, const char *path)
{
    if (!ready)
        return -1;
    uint32_t flags = irq_save();
    if (resolve(cwd, path) != FS_NOTFOUND) {   /* it already exists */
        irq_restore(flags);
        return -1;
    }
    char base[TFS3_NAME_MAX + 1];
    int dir = resolve_parent(cwd, path, base);
    if (dir == FS_NOTFOUND || !can_access(dir) ||
        (dir == FS_ROOT && system_file(base) &&
         current_level() != USER_ADMIN)) {
        irq_restore(flags);
        return -1;
    }
    int idx = create_inode(dir, base, FS_TYPE_DIR);
    irq_restore(flags);
    return idx < 0 ? -1 : 0;
}

static int t3_chdir(int cwd, const char *path, int *newcwd)
{
    if (!ready)
        return -1;
    uint32_t flags = irq_save();
    int slot = resolve(cwd, path);
    int ok = (slot != FS_NOTFOUND) && is_dir(slot) && can_access(slot);
    if (ok)
        *newcwd = slot;
    irq_restore(flags);
    return ok ? 0 : -1;
}

static int t3_unlink(int cwd, const char *path)
{
    if (!ready)
        return -1;
    uint32_t flags = irq_save();
    int slot = resolve(cwd, path);
    if (slot < 0) {                        /* none, or the root directory */
        irq_restore(flags);
        return -1;
    }
    if (!can_access(slot) || !can_access(inodes[slot].parent)) {
        irq_restore(flags);
        return -1;
    }
    if (inode_type(&inodes[slot]) == FS_TYPE_DIR) { /* a directory has to be empty */
        for (int i = 0; i < TFS3_MAX_FILES; i++)
            if (inodes[i].used && inodes[i].parent == slot) {
                irq_restore(flags);
                return -1;
            }
    }
    /* A file somebody still has open loses its name now and its data
       at the last close - the Unix way, which a C library's tmpfile()
       and a database's temporary files count on: create, unlink at
       once, and go on using the descriptor */
    if (inode_type(&inodes[slot]) == FS_TYPE_FILE && slot_open(slot)) {
        inodes[slot].parent = FS_ORPHAN;
        inodes[slot].name[0] = '\0';
        flush_inode(slot);
        irq_restore(flags);
        return 0;
    }
    inode_free_blocks(&inodes[slot]);
    inodes[slot].used = 0;
    flush_inodes();
    irq_restore(flags);
    return 0;
}

static void t3_slot_closed(int slot)
{
    if (!ready || slot < 0 || slot >= TFS3_MAX_FILES)
        return;
    uint32_t flags = irq_save();
    if (inodes[slot].used && inodes[slot].parent == FS_ORPHAN &&
        !slot_open(slot)) {
        inode_free_blocks(&inodes[slot]);
        inodes[slot].used = 0;
        flush_inode(slot);
    }
    irq_restore(flags);
}

/* Changing a name - and, when the paths point at different
 * directories, changing a parent.
 *
 * Until now 'mv' read the whole file and wrote it out again under the
 * other name: moving four megabytes meant reading and writing four
 * megabytes, to change sixty-four bytes of a directory entry. The
 * blocks do not move when a file is renamed - only the entry that
 * names them does, which is what this is.
 *
 * The rules are the ones every filesystem has: the source must exist,
 * the target's directory must exist, the target must not already be
 * there (a rename is not a way to delete something by accident), and a
 * directory may not be moved into itself or into anything below it -
 * that would cut a piece of the tree off from the root and leave it
 * pointing in a circle. */
static int t3_rename(int cwd, const char *from, const char *to)
{
    if (!ready || !from || !to)
        return -1;

    uint32_t flags = irq_save();
    int slot = resolve(cwd, from);
    if (slot < 0) {                        /* nothing there, or the root */
        irq_restore(flags);
        return -1;
    }
    if (!can_access(slot) || !can_access(inodes[slot].parent)) {
        irq_restore(flags);
        return -1;
    }

    char base[TFS3_NAME_MAX + 1];
    int dir = resolve_parent(cwd, to, base);
    if (dir == FS_NOTFOUND || !can_access(dir) ||
        (dir == FS_ROOT && system_file(base) &&
         current_level() != USER_ADMIN)) {
        irq_restore(flags);
        return -1;
    }
    if (dir_find(dir, base, (int)strlen(base)) >= 0) {
        irq_restore(flags);            /* the name is taken */
        return -1;
    }

    /* A directory into itself, or into its own descendant */
    if (inode_type(&inodes[slot]) == FS_TYPE_DIR) {
        for (int up = dir; up != FS_ROOT && up >= 0; up = inodes[up].parent)
            if (up == slot) {
                irq_restore(flags);
                return -1;
            }
        if (dir == slot) {
            irq_restore(flags);
            return -1;
        }
    }

    memset(inodes[slot].name, 0, sizeof(inodes[slot].name));
    memcpy(inodes[slot].name, base, strlen(base));
    inodes[slot].parent = (int16_t)dir;
    flush_inodes();
    irq_restore(flags);
    return 0;
}

/* The path of a directory: the names from the slot up to the root,
   then written out in reverse - each checked against the room left,
   since a chain of 63-byte names deep enough is longer than any
   buffer a caller keeps */
static int t3_getpath(int slot, char *buf, uint32_t cap)
{
    int chain[TFS3_MAX_FILES];
    int n = 0;
    int cur = slot;
    while (cur != FS_ROOT && cur >= 0 && cur < TFS3_MAX_FILES &&
           n < TFS3_MAX_FILES) {
        chain[n++] = cur;
        cur = inodes[cur].parent;
    }
    if (cap < 2)
        return -1;
    uint32_t pos = 0;
    for (int i = n - 1; i >= 0; i--) {
        const char *nm = inodes[chain[i]].name;
        uint32_t l = (uint32_t)strlen(nm);
        if (pos + 1 + l + 1 > cap) {
            buf[0] = '/';
            buf[1] = '\0';
            return -1;
        }
        buf[pos++] = '/';
        memcpy(buf + pos, nm, l);
        pos += l;
    }
    if (!pos)
        buf[pos++] = '/';
    buf[pos] = '\0';
    return (int)pos;
}

/* The inode number a program sees: TFS3 has no numbers of its own, so
   the root is 2, as ext2's is, and the slots follow it */
static uint64_t ino_of(int slot)
{
    return slot == FS_ROOT ? 2 : (uint64_t)slot + 3;
}

static void fill_stat(int slot, struct fs_stat *st)
{
    memset(st, 0, sizeof(*st));
    st->slot = slot;
    st->ino = ino_of(slot);
    if (slot == FS_ROOT) {
        st->type = FS_T_DIR;
        return;
    }
    st->type = inode_type(&inodes[slot]) == FS_TYPE_DIR ? FS_T_DIR : FS_T_FILE;
    st->size = inodes[slot].size;
    st->owner = inode_owner(&inodes[slot]);
}

/* What is at a path, as a listing of its directory would show it: a
   file one may not read is not there */
static int t3_stat_path(int cwd, const char *path, struct fs_stat *st)
{
    uint32_t flags = irq_save();
    int slot = resolve(cwd, path);
    int ok = slot != FS_NOTFOUND && (slot == FS_ROOT || can_read(slot));
    if (ok)
        fill_stat(slot, st);
    irq_restore(flags);
    return ok ? 0 : -1;
}

static int t3_stat_slot(int slot, struct fs_stat *st)
{
    if (slot != FS_ROOT && (slot < 0 || slot >= TFS3_MAX_FILES ||
                            !inodes[slot].used || !can_read(slot)))
        return -1;
    fill_stat(slot, st);
    return 0;
}

/* The listing goes through the inode table in order; the cursor is the
   table index to look at next */
static int t3_readdir_at(int dir, uint64_t *cursor, struct fs_dirent *e)
{
    uint32_t flags = irq_save();
    if (!is_dir(dir) || !can_access(dir)) {
        irq_restore(flags);
        return -1;
    }
    for (uint64_t i = *cursor; i < TFS3_MAX_FILES; i++) {
        if (!inodes[i].used || inodes[i].parent != dir || !can_read((int)i))
            continue;
        memset(e, 0, sizeof(*e));
        memcpy(e->name, inodes[i].name, strlen(inodes[i].name));
        e->size = inodes[i].size;
        e->slot = (int)i;
        e->ino = ino_of((int)i);
        e->is_dir = inode_type(&inodes[i]) == FS_TYPE_DIR;
        *cursor = i + 1;
        irq_restore(flags);
        return 1;
    }
    *cursor = TFS3_MAX_FILES;
    irq_restore(flags);
    return 0;
}

/* --- Operations by slot --- */

/* The disk block number at which a file's 'index'-th block lies.
   alloc=1 allocates it when the file does not reach that far yet.
   0 = error.
   Swapping uses this: it reserves space through the filesystem (so
   'df' and 'fls' tell the truth) and then writes over it directly
   through ATA - without stepping into the TFS code, which has one
   shared block buffer. */
static uint32_t t3_file_block(int slot, uint32_t index, int alloc)
{
    if (!ready || slot < 0 || slot >= TFS3_MAX_FILES || !inodes[slot].used ||
        !(alloc ? can_access(slot) : can_read(slot)))
        return 0;
    uint32_t blk = inode_block(&inodes[slot], index, alloc);
    if (blk && alloc)
        flush_inode(slot);
    return blk;
}

/* The first sector of a given TFS block, on the disk. Mapped files,
   big programs and swap read and write straight through blk_*, so the
   partition's start has to be in the number: without it, on a disk
   where TFS lives in a partition, block 13 was sector 26 - the GPT's
   entries. */
static uint32_t t3_block_lba(uint32_t block)
{
    return fs_base + block * SECTORS_PER_BLOCK;
}

static int64_t t3_size(int slot)
{
    if (!ready || slot < 0 || slot >= TFS3_MAX_FILES || !inodes[slot].used ||
        !can_read(slot))
        return -1;
    return (int)inodes[slot].size;
}

static int t3_owner(int slot)
{
    if (!ready || slot < 0 || slot >= TFS3_MAX_FILES || !inodes[slot].used)
        return -1;
    return inode_owner(&inodes[slot]);
}

static int t3_can_write(int slot)
{
    return ready && slot >= 0 && slot < TFS3_MAX_FILES &&
           inodes[slot].used && can_access(slot);
}

static int t3_set_owner(int slot, int uid)
{
    if (!ready || current_level() != USER_ADMIN || slot < 0 ||
        slot >= TFS3_MAX_FILES || !inodes[slot].used || uid < 0 || uid > 127)
        return -1;
    uint32_t flags = irq_save();
    inodes[slot].type = (uint8_t)((uid << FS_OWNER_SHIFT) |
                                  inode_type(&inodes[slot]));
    flush_inode(slot);
    irq_restore(flags);
    return 0;
}

/* Gives back a file's blocks from the keep-th on, and the pointer
   blocks left with nothing to point at */
static void inode_cut(struct inode *in, uint32_t keep)
{
    static uint32_t ptrs[TFS3_INDIRECT];
    static uint32_t ptrs2[TFS3_INDIRECT];

    for (uint32_t i = keep; i < TFS3_DIRECT; i++) {
        if (in->direct[i]) {
            block_free(in->direct[i]);
            in->direct[i] = 0;
        }
    }

    uint32_t from = keep > TFS3_DIRECT ? keep - TFS3_DIRECT : 0;
    if (in->indirect && from < TFS3_INDIRECT &&
        block_read(in->indirect, ptrs) == 0) {
        for (uint32_t i = from; i < TFS3_INDIRECT; i++) {
            if (ptrs[i]) {
                block_free(ptrs[i]);
                ptrs[i] = 0;
            }
        }
        if (!from) {
            block_free(in->indirect);
            in->indirect = 0;
        } else {
            block_write(in->indirect, ptrs);
        }
    }

    uint32_t base = TFS3_DIRECT + TFS3_INDIRECT;
    from = keep > base ? keep - base : 0;
    if (in->dindirect && block_read(in->dindirect, ptrs) == 0) {
        for (uint32_t hi = 0; hi < TFS3_INDIRECT; hi++) {
            uint32_t first = hi * TFS3_INDIRECT;
            if (!ptrs[hi] || first + TFS3_INDIRECT <= from)
                continue;                    /* nothing there, or all kept */
            uint32_t lo0 = from > first ? from - first : 0;
            if (block_read(ptrs[hi], ptrs2) < 0)
                continue;
            for (uint32_t lo = lo0; lo < TFS3_INDIRECT; lo++) {
                if (ptrs2[lo]) {
                    block_free(ptrs2[lo]);
                    ptrs2[lo] = 0;
                }
            }
            if (!lo0) {
                block_free(ptrs[hi]);
                ptrs[hi] = 0;
            } else {
                block_write(ptrs[hi], ptrs2);
            }
        }
        if (!from) {
            block_free(in->dindirect);
            in->dindirect = 0;
        } else {
            block_write(in->dindirect, ptrs);
        }
    }
}

/* A file made exactly 'size' bytes long (ftruncate). Shortening gives
   the blocks past the end back and zeroes the rest of the last one:
   bytes past the end must read as zeros if the file grows again, and a
   C library's ftruncate is how a database cuts itself back after a
   rollback. Growing only moves the size - the blocks in between are
   holes, which read as zeros. */
static int t3_truncate_to(int slot, uint64_t size)
{
    if (!ready || slot < 0 || slot >= TFS3_MAX_FILES || !inodes[slot].used ||
        !can_access(slot) || size > TFS3_MAX_SIZE)
        return -1;
    uint32_t flags = irq_save();
    struct inode *in = &inodes[slot];
    if (size == 0) {
        /* everything goes, also blocks past the size that swap took
           through t3_file_block without writing that far */
        inode_free_blocks(in);
    } else if (size < in->size) {
        inode_cut(in, (size + TFS3_BLOCK - 1) / TFS3_BLOCK);
        if (size % TFS3_BLOCK) {
            uint32_t bno = inode_block(in, size / TFS3_BLOCK, 0);
            if (bno && block_read(bno, block_buf) == 0) {
                memset(block_buf + size % TFS3_BLOCK, 0,
                       TFS3_BLOCK - size % TFS3_BLOCK);
                block_write(bno, block_buf);
            }
        }
    }
    in->size = (uint32_t)size;
    flush_inode(slot);
    irq_restore(flags);
    return 0;
}

static int t3_read_at(int slot, uint64_t off64, void *buf, uint32_t len)
{
    if (!ready || slot < 0 || slot >= TFS3_MAX_FILES || !inodes[slot].used ||
        !can_read(slot))
        return -1;
    if (off64 >= TFS3_MAX_SIZE)
        return 0;                          /* past anything TFS3 holds */
    uint32_t off = (uint32_t)off64;

    uint32_t flags = irq_save();
    struct inode *in = &inodes[slot];
    if (off >= in->size) {
        irq_restore(flags);
        return 0;
    }
    if (off + len > in->size)
        len = in->size - off;

    uint8_t *dst = buf;
    uint32_t done = 0;

    while (done < len) {
        uint32_t pos = off + done;
        uint32_t bno = inode_block(in, pos / TFS3_BLOCK, 0);
        uint32_t boff = pos % TFS3_BLOCK;
        uint32_t chunk = TFS3_BLOCK - boff;
        if (chunk > len - done)
            chunk = len - done;

        if (!bno) {                        /* a hole in the file - zeros */
            memset(dst + done, 0, chunk);
        } else {
            if (block_read(bno, block_buf) < 0) {
                irq_restore(flags);
                return -1;
            }
            memcpy(dst + done, block_buf + boff, chunk);
        }
        done += chunk;
    }
    irq_restore(flags);
    return (int)done;
}

static int t3_write_at(int slot, uint64_t off64, const void *buf, uint32_t len)
{
    if (!ready || slot < 0 || slot >= TFS3_MAX_FILES || !inodes[slot].used ||
        !can_access(slot))
        return -1;
    if (off64 + len > TFS3_MAX_SIZE)
        return -1;
    uint32_t off = (uint32_t)off64;

    uint32_t flags = irq_save();
    struct inode *in = &inodes[slot];
    const uint8_t *src = buf;
    uint32_t done = 0;
    int meta_dirty = 0;

    while (done < len) {
        uint32_t pos = off + done;
        uint32_t boff = pos % TFS3_BLOCK;
        uint32_t chunk = TFS3_BLOCK - boff;
        if (chunk > len - done)
            chunk = len - done;

        uint32_t had = inode_block(in, pos / TFS3_BLOCK, 0);
        uint32_t bno = had ? had : inode_block(in, pos / TFS3_BLOCK, 1);
        if (!had && bno)
            meta_dirty = 1;                /* a new block was added to the inode */
        if (!bno) {                        /* the disk is full */
            break;
        }

        if (chunk != TFS3_BLOCK) {           /* a partial block - read it in first */
            if (block_read(bno, block_buf) < 0) {
                irq_restore(flags);
                return -1;
            }
        }
        memcpy(block_buf + boff, src + done, chunk);
        if (block_write(bno, block_buf) < 0) {
            irq_restore(flags);
            return -1;
        }
        done += chunk;
    }

    if (off + done > in->size) {
        in->size = off + done;
        meta_dirty = 1;
    }
    if (meta_dirty)
        flush_inode(slot);
    irq_restore(flags);
    return done ? (int)done : -1;
}

/* Why a call on a path failed. TFS3's own code says only -1; the
   reason is worked out after the fact, from what is at the path, so a
   C library has an errno to hand on (a missing file is ENOENT, which
   is how it knows to look further - /dev/urandom is made up by it) */
static int why_make(int cwd, const char *path)
{
    char base[TFS3_NAME_MAX + 1];
    const char *last = path;
    for (const char *q = path; *q; q++)
        if (*q == '/' && q[1])
            last = q + 1;
    if (strlen(last) > TFS3_NAME_MAX)
        return FS_ENAMETOOLONG;
    int dir = resolve_parent(cwd, path, base);
    if (dir == FS_NOTFOUND)
        return FS_ENOENT;
    if (!is_dir(dir))
        return FS_ENOTDIR;
    if (!can_access(dir) ||
        (dir == FS_ROOT && system_file(base) && current_level() != USER_ADMIN))
        return FS_EACCES;
    return FS_ENOSPC;                      /* the 64 inodes are taken */
}

static int with_why(int r, int cwd, const char *path, int op)
{
    if (r >= 0 || fs_errno())
        return r;
    uint32_t flags = irq_save();
    int slot = resolve(cwd, path), e;
    switch (op) {
    case 0:                                /* open, no create */
        e = slot == FS_NOTFOUND ? FS_ENOENT
            : is_dir(slot) ? FS_EISDIR : FS_EACCES;
        break;
    case 1:                                /* open, create */
        e = slot == FS_NOTFOUND ? why_make(cwd, path)
            : is_dir(slot) ? FS_EISDIR : FS_EACCES;
        break;
    case 2:                                /* mkdir */
        e = slot != FS_NOTFOUND ? FS_EEXIST : why_make(cwd, path);
        break;
    case 3:                                /* chdir */
        e = slot == FS_NOTFOUND ? FS_ENOENT
            : !is_dir(slot) ? FS_ENOTDIR : FS_EACCES;
        break;
    default: {                             /* unlink */
        e = slot == FS_NOTFOUND ? FS_ENOENT
            : slot == FS_ROOT ? FS_EPERM : FS_EACCES;
        if (slot >= 0 && inode_type(&inodes[slot]) == FS_TYPE_DIR)
            for (int i = 0; i < TFS3_MAX_FILES; i++)
                if (inodes[i].used && inodes[i].parent == slot)
                    e = FS_ENOTEMPTY;
        break;
    }
    }
    irq_restore(flags);
    fs_set_errno(e);
    return r;
}

static int t3_open_why(int cwd, const char *path, int create)
{
    return with_why(t3_open(cwd, path, create), cwd, path, create ? 1 : 0);
}

static int t3_mkdir_why(int cwd, const char *path)
{
    return with_why(t3_mkdir(cwd, path), cwd, path, 2);
}

static int t3_chdir_why(int cwd, const char *path, int *newcwd)
{
    return with_why(t3_chdir(cwd, path, newcwd), cwd, path, 3);
}

static int t3_unlink_why(int cwd, const char *path)
{
    return with_why(t3_unlink(cwd, path), cwd, path, 4);
}

static int t3_rename_why(int cwd, const char *from, const char *to)
{
    int r = t3_rename(cwd, from, to);
    if (r < 0 && !fs_errno()) {
        uint32_t flags = irq_save();
        int e = resolve(cwd, from) == FS_NOTFOUND ? FS_ENOENT
                : resolve(cwd, to) != FS_NOTFOUND ? FS_EEXIST
                : why_make(cwd, to) == FS_ENOSPC ? FS_EACCES
                : why_make(cwd, to);
        irq_restore(flags);
        fs_set_errno(e);
    }
    return r;
}

/* POSIX's metadata, which TFS3 has nowhere to keep: a change of mode,
   owner or times is taken and dropped, as the C library always made it
   look; a second name or a symbolic link it cannot make */
static int t3_lstat_path(int cwd, const char *path, struct fs_stat *st)
{
    return t3_stat_path(cwd, path, st);
}

static int t3_open_mode(int cwd, const char *path, int create, uint32_t mode)
{
    (void)mode;
    return t3_open_why(cwd, path, create);
}

static int t3_mkdir_mode(int cwd, const char *path, uint32_t mode)
{
    (void)mode;
    return t3_mkdir_why(cwd, path);
}

static int t3_keep_nothing(int slot)
{
    if (slot != FS_ROOT &&
        (slot < 0 || slot >= TFS3_MAX_FILES || !inodes[slot].used)) {
        fs_set_errno(FS_ENOENT);
        return -1;
    }
    return 0;
}

static int t3_chmod(int slot, uint32_t mode)
{
    (void)mode;
    return t3_keep_nothing(slot);
}

static int t3_chown(int slot, int uid, int gid)
{
    (void)uid;
    (void)gid;
    return t3_keep_nothing(slot);
}

static int t3_utimes(int slot, int64_t atime, int64_t mtime)
{
    (void)atime;
    (void)mtime;
    return t3_keep_nothing(slot);
}

static int t3_link(int cwd, const char *from, const char *to)
{
    (void)cwd;
    (void)from;
    (void)to;
    fs_set_errno(FS_EPERM);
    return -1;
}

static int t3_symlink(int cwd, const char *target, const char *path)
{
    (void)cwd;
    (void)target;
    (void)path;
    fs_set_errno(FS_EPERM);
    return -1;
}

static int t3_readlink(int cwd, const char *path, char *buf, uint32_t cap)
{
    (void)cwd;
    (void)path;
    (void)buf;
    (void)cap;
    fs_set_errno(FS_EINVAL);           /* nothing on TFS3 is a link */
    return -1;
}

static int t3_resolve(int cwd, const char *path, int follow)
{
    (void)follow;
    irqflags_t flags = irq_save();
    int slot = resolve(cwd, path);
    irq_restore(flags);
    if (slot == FS_NOTFOUND)
        fs_set_errno(FS_ENOENT);
    return slot == FS_NOTFOUND ? -2 : slot;
}

static int t3_access(int cwd, const char *path, int mode)
{
    irqflags_t flags = irq_save();
    int slot = resolve(cwd, path);
    int ok = slot != FS_NOTFOUND &&
             (!(mode & 4) || slot == FS_ROOT || can_read(slot)) &&
             (!(mode & 2) || can_access(slot));
    irq_restore(flags);
    if (!ok)
        fs_set_errno(slot == FS_NOTFOUND ? FS_ENOENT : FS_EACCES);
    return ok ? 0 : -1;
}

const struct fs_backend tfs3_backend = {
    .name = "TFS3",
    .open = t3_open_why,
    .mkdir = t3_mkdir_why,
    .chdir = t3_chdir_why,
    .unlink = t3_unlink_why,
    .rename = t3_rename_why,
    .getpath = t3_getpath,
    .stat_path = t3_stat_path,
    .stat_slot = t3_stat_slot,
    .lstat_path = t3_lstat_path,
    .open_mode = t3_open_mode,
    .mkdir_mode = t3_mkdir_mode,
    .chmod = t3_chmod,
    .chown = t3_chown,
    .utimes = t3_utimes,
    .link = t3_link,
    .symlink = t3_symlink,
    .readlink = t3_readlink,
    .access = t3_access,
    .resolve = t3_resolve,
    .readdir_at = t3_readdir_at,
    .size = t3_size,
    .owner = t3_owner,
    .can_write = t3_can_write,
    .set_owner = t3_set_owner,
    .truncate_to = t3_truncate_to,
    .slot_closed = t3_slot_closed,
    .read_at = t3_read_at,
    .write_at = t3_write_at,
    .file_block = t3_file_block,
    .block_lba = t3_block_lba,
    .block_size = t3_block_size,
    .usage = t3_usage,
};
