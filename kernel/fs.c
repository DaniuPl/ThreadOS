/* PFS3 - the ProtonOS File System with block allocation
 *
 * The previous version gave every file a fixed area of 32 sectors: no
 * fragmentation, but a hard ceiling of 16 KiB and 16 files. Since the
 * system started pulling things off the network that is too little -
 * so blocks are allocated from a bitmap and an inode holds their list.
 *
 * Disk layout (block = 1 KiB = 2 sectors):
 *   block 0        - the superblock (magic "PFS3")
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

#include "include/fs.h"
#include "include/ata.h"
#include "include/blk.h"
#include "include/part.h"
#include "include/string.h"
#include "include/printk.h"
#include "include/io.h"

#define MAGIC          "PFS3"
#define SECTORS_PER_BLOCK (FS_BLOCK / ATA_SECTOR_SIZE)

#define BITMAP_START   1
#define BITMAP_BLOCKS  4
#define BITMAP_BITS    (BITMAP_BLOCKS * FS_BLOCK * 8)
#define INODE_START    (BITMAP_START + BITMAP_BLOCKS)
#define INODE_SIZE     128
#define INODE_BLOCKS   (FS_MAX_FILES * INODE_SIZE / FS_BLOCK)
#define DATA_START     (INODE_START + INODE_BLOCKS)

#define FS_TYPE_FILE 0
#define FS_TYPE_DIR  1
#define FS_NOTFOUND  (-2)

struct inode {
    char name[FS_NAME_MAX + 1];   /* 64 B, with a NUL */
    uint32_t size;                /* 4 B */
    uint8_t used;                 /* 1 B */
    uint8_t type;                 /* 1 B: FS_TYPE_FILE / FS_TYPE_DIR */
    int16_t parent;               /* 2 B: the parent's slot, -1 = root */
    uint32_t direct[FS_DIRECT];   /* 48 B: block numbers */
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
               "i-wezel musi miec dokladnie INODE_SIZE bajtow");

static int ready;
static struct inode inodes[FS_MAX_FILES];
static uint8_t bitmap[BITMAP_BLOCKS * FS_BLOCK];
static uint32_t total_blocks;
static uint8_t block_buf[FS_BLOCK];

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
static int block_read(uint32_t block, void *buf)
{
    return blk_read(fs_base + block * SECTORS_PER_BLOCK, SECTORS_PER_BLOCK,
                    buf);
}

static int block_write(uint32_t block, const void *buf)
{
    return blk_write(fs_base + block * SECTORS_PER_BLOCK, SECTORS_PER_BLOCK,
                     buf);
}

static void flush_inodes(void)
{
    const uint8_t *src = (const uint8_t *)inodes;
    for (uint32_t i = 0; i < INODE_BLOCKS; i++)
        block_write(INODE_START + i, src + i * FS_BLOCK);
}

/* Writes only the block holding a given inode. While a file is being
   written the inode changes with every data block, so rewriting the
   whole table (8 KiB through PIO) would cost more than the data
   itself. */
static void flush_inode(int slot)
{
    uint32_t blk = (uint32_t)slot * INODE_SIZE / FS_BLOCK;
    const uint8_t *src = (const uint8_t *)inodes;
    block_write(INODE_START + blk, src + blk * FS_BLOCK);
}

static void flush_bitmap(void)
{
    for (uint32_t i = 0; i < BITMAP_BLOCKS; i++)
        block_write(BITMAP_START + i, bitmap + i * FS_BLOCK);
}

/* As above: when allocating a block it is enough to write the piece of
   the bitmap in which a bit changed */
static void flush_bitmap_bit(uint32_t b)
{
    uint32_t blk = b / 8 / FS_BLOCK;
    block_write(BITMAP_START + blk, bitmap + blk * FS_BLOCK);
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
        memset(block_buf, 0, FS_BLOCK);
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
    for (int i = 0; i < FS_DIRECT; i++) {
        if (in->direct[i]) {
            block_free(in->direct[i]);
            in->direct[i] = 0;
        }
    }
    static uint32_t ptrs[FS_INDIRECT];
    static uint32_t ptrs2[FS_INDIRECT];

    if (in->indirect) {
        if (block_read(in->indirect, ptrs) == 0)
            for (uint32_t i = 0; i < FS_INDIRECT; i++)
                if (ptrs[i])
                    block_free(ptrs[i]);
        block_free(in->indirect);
        in->indirect = 0;
    }
    if (in->dindirect) {
        if (block_read(in->dindirect, ptrs) == 0) {
            for (uint32_t i = 0; i < FS_INDIRECT; i++) {
                if (!ptrs[i])
                    continue;
                if (block_read(ptrs[i], ptrs2) == 0)
                    for (uint32_t j = 0; j < FS_INDIRECT; j++)
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
    if (n < FS_DIRECT) {
        if (!in->direct[n] && alloc)
            in->direct[n] = block_alloc();
        return in->direct[n];
    }

    n -= FS_DIRECT;

    static uint32_t ptrs[FS_INDIRECT];
    static uint32_t ptrs2[FS_INDIRECT];

    if (n < FS_INDIRECT) {               /* the indirect block */
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

    n -= FS_INDIRECT;                    /* the doubly indirect block */
    if (n >= (uint32_t)FS_DINDIRECT)
        return 0;                        /* beyond the file size ceiling */

    uint32_t hi = n / FS_INDIRECT, lo = n % FS_INDIRECT;

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
    sb.ninodes = FS_MAX_FILES;
    sb.nblocks = total_blocks;
    sb.data_start = DATA_START;

    memset(block_buf, 0, FS_BLOCK);
    memcpy(block_buf, &sb, sizeof(sb));
    block_write(0, block_buf);

    memset(bitmap, 0, sizeof(bitmap));
    for (uint32_t b = 0; b < DATA_START; b++)  /* metadata taken */
        bit_set(b, 1);
    flush_bitmap();

    memset(inodes, 0, sizeof(inodes));
    flush_inodes();
    printk("[ OK ] PFS: pusty dysk sformatowany (%u blokow po %u B)\n",
           total_blocks, FS_BLOCK);
}

/* Whether the disk has never had anything written to it.
 *
 * "Blank" is answered conservatively, because the cost of getting it
 * wrong one way is a fresh disk the person has to format by hand, and
 * the other way is somebody's data. A partition table announces itself
 * with a signature in the last two bytes of sector zero; beyond that,
 * a disk anyone has ever used has SOMETHING in its first sixty-four
 * kilobytes - a boot sector, a superblock, a stray header. All zeros
 * there is what a new disk, and an image made by a test, looks like. */
static int disk_is_blank(void)
{
    static uint8_t probe[512];
    for (uint32_t lba = 0; lba < 128; lba++) {
        if (blk_read(lba, 1, probe) < 0)
            return 0;              /* cannot read it - certainly not ours */
        if (lba == 0 && probe[510] == 0x55 && probe[511] == 0xAA)
            return 0;              /* a partition table */
        for (int i = 0; i < 512; i++)
            if (probe[i])
                return 0;
    }
    return 1;
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

/* Formats because somebody asked in so many words. 'partition' is 0
   for the whole disk, or a partition number counted from one - and
   that second case is the one that matters on a machine that belongs
   to somebody: the table and every other partition are left exactly
   as they were. Returns 1 when it happened. */
int fs_format_now(int partition)
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

int fs_init(void)
{
    if (!blk_present()) {
        printk("[ !! ] FS: brak dysku\n");
        return 0;
    }

    if (!fit(0, blk_sectors())) {
        printk("[ !! ] FS: dysk za maly (%u blokow)\n", total_blocks);
        return 0;
    }

    if (block_read(0, block_buf) < 0) {
        printk("[ !! ] FS: blad odczytu superbloku\n");
        return 0;
    }

    struct superblock *sb = (struct superblock *)block_buf;
    if (memcmp(sb->magic, MAGIC, 4) != 0) {
        /* A disk that is not ours is NOT ours to take.
         *
         * This used to format anything whose first block did not say
         * PFS3 - which was harmless while every disk this system ever
         * saw was an empty file made by a test, and is a catastrophe
         * the moment it boots on a real machine: a laptop's disk does
         * not say PFS3 either, and the first thing the system would do
         * is throw away its partition table, its Windows and
         * everything on it, without asking, in the first second of
         * the first boot.
         *
         * So the disk is formatted only when it is genuinely blank.
         * Anything else is left alone and said out loud, and the
         * person who really does want this disk says so with the
         * 'formatuj' command. */
        /* Not at the front of the disk - but perhaps in a partition,
           which is where a system living next to somebody else's
           actually lives. */
        int p = find_our_partition();
        if (p) {
            printk("[ OK ] PFS: w partycji %u (od sektora %u)\n",
                   (uint32_t)p, fs_base);
            for (uint32_t i = 0; i < BITMAP_BLOCKS; i++)
                block_read(BITMAP_START + i, bitmap + i * FS_BLOCK);
            uint8_t *dst = (uint8_t *)inodes;
            for (uint32_t i = 0; i < INODE_BLOCKS; i++)
                block_read(INODE_START + i, dst + i * FS_BLOCK);
            ready = 1;
            return 1;
        }

        if (!disk_is_blank()) {
            printk("[ !! ] FS: na dysku cos JEST, a nie jest to PFS - "
                   "nie ruszam go.\n");
            printk("       Jesli caly dysk ma byc dla ProtonOS-a, wpisz "
                   "'formatuj' (skasuje wszystko);\n");
            printk("       zeby zajac jedna partycje - 'formatuj N'.\n");
            return 0;
        }
        format_disk();
    } else {
        for (uint32_t i = 0; i < BITMAP_BLOCKS; i++)
            block_read(BITMAP_START + i, bitmap + i * FS_BLOCK);
        uint8_t *dst = (uint8_t *)inodes;
        for (uint32_t i = 0; i < INODE_BLOCKS; i++)
            block_read(INODE_START + i, dst + i * FS_BLOCK);
    }
    ready = 1;
    return 1;
}

int fs_ready(void)
{
    return ready;
}

void fs_usage(uint32_t *used, uint32_t *total)
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
    for (int i = 0; i < FS_MAX_FILES; i++)
        if (!inodes[i].used)
            return i;
    return -1;
}

static int is_dir(int slot)
{
    return slot == FS_ROOT || (slot >= 0 && inodes[slot].type == FS_TYPE_DIR);
}

static int parent_of(int slot)
{
    return slot == FS_ROOT ? FS_ROOT : inodes[slot].parent;
}

/* Looks for an entry named s (length len) in directory 'parent';
   -1 when there is none */
static int dir_find(int parent, const char *s, int len)
{
    if (len <= 0 || len > FS_NAME_MAX)
        return -1;
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (!inodes[i].used || inodes[i].parent != parent)
            continue;
        if ((int)strlen(inodes[i].name) == len &&
            memcmp(inodes[i].name, s, len) == 0)
            return i;
    }
    return -1;
}

/* Resolves a whole path to a slot (>=0 or FS_ROOT) or FS_NOTFOUND */
static int resolve(int cwd, const char *path)
{
    int cur = (path[0] == '/') ? FS_ROOT : cwd;
    const char *p = path;
    while (*p) {
        while (*p == '/')
            p++;
        if (!*p)
            break;
        const char *s = p;
        while (*p && *p != '/')
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
        int n = (int)(last - path);
        if (n >= 240)
            return FS_NOTFOUND;
        char tmp[256];
        memcpy(tmp, path, n);
        tmp[n] = '\0';
        dir = (n == 0) ? FS_ROOT : resolve(cwd, tmp);  /* "/x" -> root */
        bname = last + 1;
    }
    if (dir == FS_NOTFOUND || !is_dir(dir))
        return FS_NOTFOUND;

    int bl = (int)strlen(bname);
    if (bl <= 0 || bl > FS_NAME_MAX)
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
    inodes[idx].type = (uint8_t)type;
    inodes[idx].parent = (int16_t)dir;
    flush_inodes();
    return idx;
}

/* --- Public path operations --- */

int fs_open(int cwd, const char *path, int create)
{
    if (!ready)
        return -1;

    uint32_t flags = irq_save();
    int slot = resolve(cwd, path);
    if (slot >= 0 && inodes[slot].type == FS_TYPE_FILE) {
        irq_restore(flags);
        return slot;                       /* an existing file */
    }
    if (slot == FS_ROOT || (slot >= 0 && inodes[slot].type == FS_TYPE_DIR)) {
        irq_restore(flags);
        return -1;                         /* that is a directory, not a file */
    }
    if (!create) {
        irq_restore(flags);
        return -1;
    }

    char base[FS_NAME_MAX + 1];
    int dir = resolve_parent(cwd, path, base);
    if (dir == FS_NOTFOUND) {
        irq_restore(flags);
        return -1;
    }
    int idx = create_inode(dir, base, FS_TYPE_FILE);
    irq_restore(flags);
    return idx;
}

int fs_mkdir(int cwd, const char *path)
{
    if (!ready)
        return -1;
    uint32_t flags = irq_save();
    if (resolve(cwd, path) != FS_NOTFOUND) {   /* it already exists */
        irq_restore(flags);
        return -1;
    }
    char base[FS_NAME_MAX + 1];
    int dir = resolve_parent(cwd, path, base);
    if (dir == FS_NOTFOUND) {
        irq_restore(flags);
        return -1;
    }
    int idx = create_inode(dir, base, FS_TYPE_DIR);
    irq_restore(flags);
    return idx < 0 ? -1 : 0;
}

int fs_chdir(int cwd, const char *path, int *newcwd)
{
    if (!ready)
        return -1;
    uint32_t flags = irq_save();
    int slot = resolve(cwd, path);
    int ok = (slot != FS_NOTFOUND) && is_dir(slot);
    if (ok)
        *newcwd = slot;
    irq_restore(flags);
    return ok ? 0 : -1;
}

int fs_unlink(int cwd, const char *path)
{
    if (!ready)
        return -1;
    uint32_t flags = irq_save();
    int slot = resolve(cwd, path);
    if (slot < 0) {                        /* none, or the root directory */
        irq_restore(flags);
        return -1;
    }
    if (inodes[slot].type == FS_TYPE_DIR) { /* a directory has to be empty */
        for (int i = 0; i < FS_MAX_FILES; i++)
            if (inodes[i].used && inodes[i].parent == slot) {
                irq_restore(flags);
                return -1;
            }
    }
    inode_free_blocks(&inodes[slot]);
    inodes[slot].used = 0;
    flush_inodes();
    irq_restore(flags);
    return 0;
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
int fs_rename(int cwd, const char *from, const char *to)
{
    if (!ready || !from || !to)
        return -1;

    uint32_t flags = irq_save();
    int slot = resolve(cwd, from);
    if (slot < 0) {                        /* nothing there, or the root */
        irq_restore(flags);
        return -1;
    }

    char base[FS_NAME_MAX + 1];
    int dir = resolve_parent(cwd, to, base);
    if (dir == FS_NOTFOUND) {
        irq_restore(flags);
        return -1;
    }
    if (dir_find(dir, base, (int)strlen(base)) >= 0) {
        irq_restore(flags);            /* the name is taken */
        return -1;
    }

    /* A directory into itself, or into its own descendant */
    if (inodes[slot].type == FS_TYPE_DIR) {
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

void fs_getpath(int slot, char *buf)
{
    if (slot == FS_ROOT || slot < 0) {
        buf[0] = '/';
        buf[1] = '\0';
        return;
    }
    /* Collect the names from the slot up to the root, then reverse the order */
    int chain[FS_MAX_FILES];
    int n = 0;
    int cur = slot;
    while (cur != FS_ROOT && n < FS_MAX_FILES) {
        chain[n++] = cur;
        cur = inodes[cur].parent;
    }
    int pos = 0;
    for (int i = n - 1; i >= 0; i--) {
        buf[pos++] = '/';
        const char *nm = inodes[chain[i]].name;
        while (*nm)
            buf[pos++] = *nm++;
    }
    buf[pos] = '\0';
}

int fs_readdir(int dir, int index, char *name, uint32_t *size, int *is_d)
{
    if (!ready)
        return 0;
    uint32_t flags = irq_save();
    int count = 0;
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (!inodes[i].used || inodes[i].parent != dir)
            continue;
        if (count == index) {
            int j = 0;
            while (inodes[i].name[j]) {
                name[j] = inodes[i].name[j];
                j++;
            }
            name[j] = '\0';
            if (size)
                *size = inodes[i].size;
            if (is_d)
                *is_d = (inodes[i].type == FS_TYPE_DIR);
            irq_restore(flags);
            return 1;
        }
        count++;
    }
    irq_restore(flags);
    return 0;
}

/* --- Operations by slot --- */

/* The disk block number at which a file's 'index'-th block lies.
   alloc=1 allocates it when the file does not reach that far yet.
   0 = error.
   Swapping uses this: it reserves space through the filesystem (so
   'df' and 'fls' tell the truth) and then writes over it directly
   through ATA - without stepping into the PFS code, which has one
   shared block buffer. */
uint32_t fs_file_block(int slot, uint32_t index, int alloc)
{
    if (!ready || slot < 0 || slot >= FS_MAX_FILES || !inodes[slot].used)
        return 0;
    uint32_t blk = inode_block(&inodes[slot], index, alloc);
    if (blk && alloc)
        flush_inode(slot);
    return blk;
}

/* The first ATA sector of a given PFS block */
uint32_t fs_block_lba(uint32_t block)
{
    return block * SECTORS_PER_BLOCK;
}

int fs_size(int slot)
{
    if (!ready || slot < 0 || slot >= FS_MAX_FILES || !inodes[slot].used)
        return -1;
    return (int)inodes[slot].size;
}

int fs_truncate(int slot)
{
    if (!ready || slot < 0 || slot >= FS_MAX_FILES || !inodes[slot].used)
        return -1;
    uint32_t flags = irq_save();
    inode_free_blocks(&inodes[slot]);
    flush_inode(slot);
    irq_restore(flags);
    return 0;
}

int fs_read_at(int slot, uint32_t off, void *buf, uint32_t len)
{
    if (!ready || slot < 0 || slot >= FS_MAX_FILES || !inodes[slot].used)
        return -1;

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
        uint32_t bno = inode_block(in, pos / FS_BLOCK, 0);
        uint32_t boff = pos % FS_BLOCK;
        uint32_t chunk = FS_BLOCK - boff;
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

int fs_write_at(int slot, uint32_t off, const void *buf, uint32_t len)
{
    if (!ready || slot < 0 || slot >= FS_MAX_FILES || !inodes[slot].used)
        return -1;
    if (off + len > FS_MAX_SIZE)
        return -1;

    uint32_t flags = irq_save();
    struct inode *in = &inodes[slot];
    const uint8_t *src = buf;
    uint32_t done = 0;
    int meta_dirty = 0;

    while (done < len) {
        uint32_t pos = off + done;
        uint32_t boff = pos % FS_BLOCK;
        uint32_t chunk = FS_BLOCK - boff;
        if (chunk > len - done)
            chunk = len - done;

        uint32_t had = inode_block(in, pos / FS_BLOCK, 0);
        uint32_t bno = had ? had : inode_block(in, pos / FS_BLOCK, 1);
        if (!had && bno)
            meta_dirty = 1;                /* a new block was added to the inode */
        if (!bno) {                        /* the disk is full */
            break;
        }

        if (chunk != FS_BLOCK) {           /* a partial block - read it in first */
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

/* --- Convenience for the kernel shell (the root directory) --- */

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
    if (!ready) {
        printk("FS: brak dysku\n");
        return;
    }
    char name[FS_NAME_MAX + 1];
    uint32_t size;
    int is_d;
    int n = 0;
    printk("Katalog glowny:\n");
    for (int i = 0; fs_readdir(FS_ROOT, i, name, &size, &is_d); i++) {
        if (is_d)
            printk("  %s/\n", name);
        else
            printk("  %s  (%u B)\n", name, size);
        n++;
    }
    if (!n)
        printk("  (pusty)\n");
}
