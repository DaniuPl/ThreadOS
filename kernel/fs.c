/* PFS3 - ProtonOS File System z przydzialem blokow
 *
 * Poprzednia wersja dawala kazdemu plikowi staly obszar 32 sektorow:
 * bez fragmentacji, ale z twardym sufitem 16 KiB i 16 plikow. Odkad
 * system sciaga rzeczy z sieci, to za malo - wiec bloki sa przydzielane
 * z bitmapy, a i-wezel trzyma ich liste.
 *
 * Uklad dysku (blok = 1 KiB = 2 sektory):
 *   blok 0        - superblok (magic "PFS3")
 *   bloki 1..4    - bitmapa blokow (4 KiB = 32768 bitow)
 *   bloki 5..12   - tablica i-wezlow (64 x 128 B)
 *   bloki 13..    - dane
 *
 * Plik ma 12 blokow bezposrednich, blok posredni (256 wskaznikow) i blok
 * podwojnie posredni (256 blokow po 256 wskaznikow), czyli do 64 MiB. Katalogi to i-wezly typu DIR; kazdy i-wezel pamieta
 * slot rodzica (FS_ROOT = -1 dla korzenia). Sciezki rozwiazuje sie
 * chodzeniem po komponentach ("." i ".." obslugiwane).
 *
 * Bitmapa i tablica i-wezlow siedza w pamieci i sa zapisywane na dysk
 * przy kazdej zmianie - przy tej skali to prostsze i bezpieczniejsze niz
 * pamiec podreczna z odroczonym zapisem. Operacje dyskowe biegna z
 * wylaczonymi przerwaniami.
 */

#include "include/fs.h"
#include "include/ata.h"
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
    char name[FS_NAME_MAX + 1];   /* 64 B, z NUL */
    uint32_t size;                /* 4 B */
    uint8_t used;                 /* 1 B */
    uint8_t type;                 /* 1 B: FS_TYPE_FILE / FS_TYPE_DIR */
    int16_t parent;               /* 2 B: slot rodzica, -1 = root */
    uint32_t direct[FS_DIRECT];   /* 48 B: numery blokow */
    uint32_t indirect;            /* 4 B: blok ze wskaznikami (0 = brak) */
    uint32_t dindirect;           /* 4 B: blok blokow ze wskaznikami */
} __attribute__((packed));        /* 128 B -> 8 i-wezlow na blok */

struct superblock {
    char magic[8];
    uint32_t ninodes;
    uint32_t nblocks;             /* ile blokow obejmuje bitmapa */
    uint32_t data_start;
} __attribute__((packed));

/* Uklad na dysku zalezy od rozmiaru i-wezla - niech kompilator pilnuje */
_Static_assert(sizeof(struct inode) == INODE_SIZE,
               "i-wezel musi miec dokladnie INODE_SIZE bajtow");

static int ready;
static struct inode inodes[FS_MAX_FILES];
static uint8_t bitmap[BITMAP_BLOCKS * FS_BLOCK];
static uint32_t total_blocks;
static uint8_t block_buf[FS_BLOCK];
static uint8_t sector_buf[ATA_SECTOR_SIZE];

/* --- dostep do blokow ------------------------------------------------- */

static int block_read(uint32_t block, void *buf)
{
    uint8_t *p = buf;
    for (uint32_t i = 0; i < SECTORS_PER_BLOCK; i++)
        if (ata_read_sector(block * SECTORS_PER_BLOCK + i,
                            p + i * ATA_SECTOR_SIZE) < 0)
            return -1;
    return 0;
}

static int block_write(uint32_t block, const void *buf)
{
    const uint8_t *p = buf;
    for (uint32_t i = 0; i < SECTORS_PER_BLOCK; i++) {
        memcpy(sector_buf, p + i * ATA_SECTOR_SIZE, ATA_SECTOR_SIZE);
        if (ata_write_sector(block * SECTORS_PER_BLOCK + i, sector_buf) < 0)
            return -1;
    }
    return 0;
}

static void flush_inodes(void)
{
    const uint8_t *src = (const uint8_t *)inodes;
    for (uint32_t i = 0; i < INODE_BLOCKS; i++)
        block_write(INODE_START + i, src + i * FS_BLOCK);
}

/* Zapisuje tylko blok z danym i-wezlem. Przy zapisie pliku i-wezel
   zmienia sie co blok danych, wiec przepisywanie calej tablicy (8 KiB
   przez PIO) kosztowaloby wiecej niz same dane. */
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

/* Jak wyzej: przy przydziale bloku wystarczy zapisac ten kawalek
   bitmapy, w ktorym zmienil sie bit */
static void flush_bitmap_bit(uint32_t b)
{
    uint32_t blk = b / 8 / FS_BLOCK;
    block_write(BITMAP_START + blk, bitmap + blk * FS_BLOCK);
}

/* --- bitmapa blokow --------------------------------------------------- */

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

/* Przydziela wyzerowany blok; 0 = brak miejsca (blok 0 to superblok) */
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

/* Zwalnia wszystkie bloki pliku */
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

/* Numer bloku dysku dla n-tego bloku pliku; alloc=1 przydziela brakujace.
   Zwraca 0, gdy bloku nie ma (albo nie da sie przydzielic). */
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

    if (n < FS_INDIRECT) {               /* blok posredni */
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

    n -= FS_INDIRECT;                    /* blok podwojnie posredni */
    if (n >= (uint32_t)FS_DINDIRECT)
        return 0;                        /* poza sufitem rozmiaru pliku */

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

/* --- start i formatowanie --------------------------------------------- */

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
    for (uint32_t b = 0; b < DATA_START; b++)  /* metadane zajete */
        bit_set(b, 1);
    flush_bitmap();

    memset(inodes, 0, sizeof(inodes));
    flush_inodes();
    printk("[ OK ] PFS: pusty dysk sformatowany (%u blokow po %u B)\n",
           total_blocks, FS_BLOCK);
}

int fs_init(void)
{
    if (!ata_present()) {
        printk("[ !! ] FS: brak dysku ATA\n");
        return 0;
    }

    uint32_t sectors = ata_sectors();
    total_blocks = sectors / SECTORS_PER_BLOCK;
    if (total_blocks > BITMAP_BITS)
        total_blocks = BITMAP_BITS;        /* tyle obejmuje bitmapa */
    if (total_blocks <= DATA_START) {
        printk("[ !! ] FS: dysk za maly (%u blokow)\n", total_blocks);
        return 0;
    }

    if (block_read(0, block_buf) < 0) {
        printk("[ !! ] FS: blad odczytu superbloku\n");
        return 0;
    }

    struct superblock *sb = (struct superblock *)block_buf;
    if (memcmp(sb->magic, MAGIC, 4) != 0) {
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

/* --- Pomocnicze: katalogi i rozwiazywanie sciezek --- */

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

/* Szuka wpisu o nazwie s (dlugosc len) w katalogu 'parent'; -1 gdy brak */
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

/* Rozwiazuje cala sciezke do slotu (>=0 albo FS_ROOT) lub FS_NOTFOUND */
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

/* Rozwiazuje katalog-rodzica ostatniego komponentu i wyodrebnia nazwe
   bazowa. Zwraca slot katalogu (>=0 albo FS_ROOT) lub FS_NOTFOUND. */
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

/* --- Publiczne operacje na sciezkach --- */

int fs_open(int cwd, const char *path, int create)
{
    if (!ready)
        return -1;

    uint32_t flags = irq_save();
    int slot = resolve(cwd, path);
    if (slot >= 0 && inodes[slot].type == FS_TYPE_FILE) {
        irq_restore(flags);
        return slot;                       /* istniejacy plik */
    }
    if (slot == FS_ROOT || (slot >= 0 && inodes[slot].type == FS_TYPE_DIR)) {
        irq_restore(flags);
        return -1;                         /* to katalog, nie plik */
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
    if (resolve(cwd, path) != FS_NOTFOUND) {   /* juz istnieje */
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
    if (slot < 0) {                        /* brak albo katalog glowny */
        irq_restore(flags);
        return -1;
    }
    if (inodes[slot].type == FS_TYPE_DIR) { /* katalog musi byc pusty */
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

void fs_getpath(int slot, char *buf)
{
    if (slot == FS_ROOT || slot < 0) {
        buf[0] = '/';
        buf[1] = '\0';
        return;
    }
    /* Zbierz nazwy od slotu do korzenia, potem odwroc kolejnosc */
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

/* --- Operacje po slocie --- */

/* Numer bloku dysku, pod ktorym lezy 'index'-ty blok pliku. alloc=1
   przydziela go, gdy plik jeszcze tam nie siega. 0 = blad.
   Korzysta z tego swap: miejsce rezerwuje przez system plikow (wiec
   'df' i 'fls' mowia prawde), a pisze po nim juz wprost przez ATA -
   bez wchodzenia w kod PFS, ktory ma jeden wspolny bufor na blok. */
uint32_t fs_file_block(int slot, uint32_t index, int alloc)
{
    if (!ready || slot < 0 || slot >= FS_MAX_FILES || !inodes[slot].used)
        return 0;
    uint32_t blk = inode_block(&inodes[slot], index, alloc);
    if (blk && alloc)
        flush_inode(slot);
    return blk;
}

/* Pierwszy sektor ATA danego bloku PFS */
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

        if (!bno) {                        /* dziura w pliku - zera */
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
            meta_dirty = 1;                /* doszedl nowy blok do i-wezla */
        if (!bno) {                        /* brak miejsca na dysku */
            break;
        }

        if (chunk != FS_BLOCK) {           /* czesciowy blok - doczytaj */
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

/* --- Wygoda dla shella jadra (katalog glowny) --- */

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
