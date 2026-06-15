/* PFS - ProtonOS File System (z katalogami)
 *
 * Uklad dysku (sektory po 512 B):
 *   sektor 0       - superblok (magic "PFS2")
 *   sektor 1       - tablica i-wezlow (16 x 32 B = 512 B)
 *   sektor 2+i*32  - dane pliku i (kazdy plik ma staly obszar 32 sektorow)
 *
 * Katalogi to i-wezly typu DIR; kazdy i-wezel pamieta slot rodzica
 * (FS_ROOT = -1 dla katalogu glownego). Sciezki sa rozwiazywane przez
 * chodzenie po komponentach ("." i ".." obslugiwane). Alokacja danych
 * jest statyczna (plik o slocie i zawsze w tym samym miejscu), wiec nie
 * ma fragmentacji. Operacje dyskowe biegna z wylaczonymi przerwaniami.
 */

#include "include/fs.h"
#include "include/ata.h"
#include "include/string.h"
#include "include/printk.h"
#include "include/io.h"

#define MAGIC      "PFS2"
#define INODE_SECTOR 1
#define DATA_START 2
#define FILE_SECTORS 32

#define FS_TYPE_FILE 0
#define FS_TYPE_DIR  1
#define FS_NOTFOUND  (-2)

struct inode {
    char name[FS_NAME_MAX + 1];   /* 24 B, z NUL */
    uint32_t size;                /* 4 B */
    uint8_t used;                 /* 1 B */
    uint8_t type;                 /* 1 B: FS_TYPE_FILE / FS_TYPE_DIR */
    int8_t parent;                /* 1 B: slot rodzica, -1 = root */
    uint8_t reserved;             /* 1 B */
} __attribute__((packed));        /* 32 B -> 16 i-wezlow na sektor */

struct superblock {
    char magic[8];
    uint32_t ninodes;
} __attribute__((packed));

static int ready;
static struct inode inodes[FS_MAX_FILES];
static uint8_t sector_buf[ATA_SECTOR_SIZE];

static void flush_inodes(void)
{
    memset(sector_buf, 0, ATA_SECTOR_SIZE);
    memcpy(sector_buf, inodes, sizeof(inodes));
    ata_write_sector(INODE_SECTOR, sector_buf);
}

static void format_disk(void)
{
    struct superblock sb;
    memset(&sb, 0, sizeof(sb));
    memcpy(sb.magic, MAGIC, 4);
    sb.ninodes = FS_MAX_FILES;

    memset(sector_buf, 0, ATA_SECTOR_SIZE);
    memcpy(sector_buf, &sb, sizeof(sb));
    ata_write_sector(0, sector_buf);

    memset(inodes, 0, sizeof(inodes));
    flush_inodes();
    printk("[ OK ] PFS: pusty dysk sformatowany\n");
}

int fs_init(void)
{
    if (!ata_present()) {
        printk("[ !! ] FS: brak dysku ATA\n");
        return 0;
    }
    if (ata_read_sector(0, sector_buf) < 0) {
        printk("[ !! ] FS: blad odczytu superbloku\n");
        return 0;
    }

    struct superblock *sb = (struct superblock *)sector_buf;
    if (memcmp(sb->magic, MAGIC, 4) != 0) {
        format_disk();
    } else {
        ata_read_sector(INODE_SECTOR, sector_buf);
        memcpy(inodes, sector_buf, sizeof(inodes));
    }
    ready = 1;
    return 1;
}

int fs_ready(void)
{
    return ready;
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
        if (n >= 120)
            return FS_NOTFOUND;
        char tmp[128];
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
    memset(inodes[idx].name, 0, sizeof(inodes[idx].name));
    memcpy(inodes[idx].name, base, strlen(base));
    inodes[idx].size = 0;
    inodes[idx].used = 1;
    inodes[idx].type = (uint8_t)type;
    inodes[idx].parent = (int8_t)dir;
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
    inodes[slot].size = 0;
    flush_inodes();
    irq_restore(flags);
    return 0;
}

int fs_read_at(int slot, uint32_t off, void *buf, uint32_t len)
{
    if (!ready || slot < 0 || slot >= FS_MAX_FILES || !inodes[slot].used)
        return -1;

    uint32_t flags = irq_save();
    uint32_t size = inodes[slot].size;
    if (off >= size) {
        irq_restore(flags);
        return 0;
    }
    if (off + len > size)
        len = size - off;

    uint32_t base = DATA_START + slot * FILE_SECTORS;
    uint8_t *dst = buf;
    uint32_t done = 0;

    while (done < len) {
        uint32_t pos = off + done;
        uint32_t sector = pos / ATA_SECTOR_SIZE;
        uint32_t soff = pos % ATA_SECTOR_SIZE;
        uint32_t chunk = ATA_SECTOR_SIZE - soff;
        if (chunk > len - done)
            chunk = len - done;
        if (ata_read_sector(base + sector, sector_buf) < 0) {
            irq_restore(flags);
            return -1;
        }
        memcpy(dst + done, sector_buf + soff, chunk);
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
    uint32_t base = DATA_START + slot * FILE_SECTORS;
    const uint8_t *src = buf;
    uint32_t done = 0;

    while (done < len) {
        uint32_t pos = off + done;
        uint32_t sector = pos / ATA_SECTOR_SIZE;
        uint32_t soff = pos % ATA_SECTOR_SIZE;
        uint32_t chunk = ATA_SECTOR_SIZE - soff;
        if (chunk > len - done)
            chunk = len - done;
        if (ata_read_sector(base + sector, sector_buf) < 0) {
            irq_restore(flags);
            return -1;
        }
        memcpy(sector_buf + soff, src + done, chunk);
        if (ata_write_sector(base + sector, sector_buf) < 0) {
            irq_restore(flags);
            return -1;
        }
        done += chunk;
    }

    if (off + len > inodes[slot].size) {
        inodes[slot].size = off + len;
        flush_inodes();
    }
    irq_restore(flags);
    return (int)done;
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
