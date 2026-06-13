/* PFS - ProtonOS File System
 *
 * Najprostszy mozliwy zapisywalny system plikow na dysku ATA.
 * Uklad dysku (sektory po 512 B):
 *   sektor 0       - superblok (magic "PFS1" + liczba i-wezlow)
 *   sektor 1       - tablica i-wezlow (16 x 32 B = 512 B)
 *   sektor 2+i*32  - dane pliku i (kazdy plik ma staly obszar 32 sektorow)
 *
 * Alokacja jest statyczna: plik o indeksie i zawsze lezy w tym samym
 * miejscu, wiec nie ma fragmentacji ani potrzeby mapy zajetosci.
 * Pusty (niesformatowany) dysk jest automatycznie formatowany przy
 * starcie. Operacje dyskowe biegna z wylaczonymi przerwaniami -
 * krotko, a chroni to wspoldzielony bufor sektora przed wywlaszczeniem.
 */

#include "include/fs.h"
#include "include/ata.h"
#include "include/string.h"
#include "include/printk.h"
#include "include/io.h"

#define MAGIC      "PFS1"
#define INODE_SECTOR 1
#define DATA_START 2
#define FILE_SECTORS 32

struct inode {
    char name[FS_NAME_MAX + 1];   /* 24 B, z NUL */
    uint32_t size;
    uint32_t used;
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

static int find_inode(const char *name)
{
    for (int i = 0; i < FS_MAX_FILES; i++)
        if (inodes[i].used && !strcmp(inodes[i].name, name))
            return i;
    return -1;
}

static int alloc_inode(void)
{
    for (int i = 0; i < FS_MAX_FILES; i++)
        if (!inodes[i].used)
            return i;
    return -1;
}

int fs_write(const char *name, const void *data, uint32_t len)
{
    if (!ready || len > FS_MAX_SIZE || strlen(name) > FS_NAME_MAX)
        return -1;

    uint32_t flags = irq_save();
    int idx = find_inode(name);
    if (idx < 0)
        idx = alloc_inode();
    if (idx < 0) {
        irq_restore(flags);
        return -1;          /* brak wolnego i-wezla */
    }

    uint32_t base = DATA_START + idx * FILE_SECTORS;
    uint32_t sectors = (len + ATA_SECTOR_SIZE - 1) / ATA_SECTOR_SIZE;
    const uint8_t *src = data;

    for (uint32_t s = 0; s < sectors; s++) {
        memset(sector_buf, 0, ATA_SECTOR_SIZE);
        uint32_t chunk = len - s * ATA_SECTOR_SIZE;
        if (chunk > ATA_SECTOR_SIZE)
            chunk = ATA_SECTOR_SIZE;
        memcpy(sector_buf, src + s * ATA_SECTOR_SIZE, chunk);
        if (ata_write_sector(base + s, sector_buf) < 0) {
            irq_restore(flags);
            return -1;
        }
    }

    memset(inodes[idx].name, 0, sizeof(inodes[idx].name));
    memcpy(inodes[idx].name, name, strlen(name));
    inodes[idx].size = len;
    inodes[idx].used = 1;
    flush_inodes();

    irq_restore(flags);
    return 0;
}

int fs_read(const char *name, void *buf, uint32_t max)
{
    if (!ready)
        return -1;

    uint32_t flags = irq_save();
    int idx = find_inode(name);
    if (idx < 0) {
        irq_restore(flags);
        return -1;
    }

    uint32_t len = inodes[idx].size;
    if (len > max)
        len = max;
    uint32_t base = DATA_START + idx * FILE_SECTORS;
    uint32_t sectors = (len + ATA_SECTOR_SIZE - 1) / ATA_SECTOR_SIZE;
    uint8_t *dst = buf;

    for (uint32_t s = 0; s < sectors; s++) {
        if (ata_read_sector(base + s, sector_buf) < 0) {
            irq_restore(flags);
            return -1;
        }
        uint32_t chunk = len - s * ATA_SECTOR_SIZE;
        if (chunk > ATA_SECTOR_SIZE)
            chunk = ATA_SECTOR_SIZE;
        memcpy(dst + s * ATA_SECTOR_SIZE, sector_buf, chunk);
    }

    irq_restore(flags);
    return (int)len;
}

int fs_open(const char *name, int create)
{
    if (!ready || strlen(name) > FS_NAME_MAX)
        return -1;

    uint32_t flags = irq_save();
    int idx = find_inode(name);
    if (idx < 0 && create) {
        idx = alloc_inode();
        if (idx >= 0) {
            memset(inodes[idx].name, 0, sizeof(inodes[idx].name));
            memcpy(inodes[idx].name, name, strlen(name));
            inodes[idx].size = 0;
            inodes[idx].used = 1;
            flush_inodes();
        }
    }
    irq_restore(flags);
    return idx;
}

int fs_size(int idx)
{
    if (!ready || idx < 0 || idx >= FS_MAX_FILES || !inodes[idx].used)
        return -1;
    return (int)inodes[idx].size;
}

int fs_truncate(int idx)
{
    if (!ready || idx < 0 || idx >= FS_MAX_FILES || !inodes[idx].used)
        return -1;
    uint32_t flags = irq_save();
    inodes[idx].size = 0;
    flush_inodes();
    irq_restore(flags);
    return 0;
}

int fs_read_at(int idx, uint32_t off, void *buf, uint32_t len)
{
    if (!ready || idx < 0 || idx >= FS_MAX_FILES || !inodes[idx].used)
        return -1;

    uint32_t flags = irq_save();
    uint32_t size = inodes[idx].size;
    if (off >= size) {
        irq_restore(flags);
        return 0;                       /* za koncem pliku */
    }
    if (off + len > size)
        len = size - off;

    uint32_t base = DATA_START + idx * FILE_SECTORS;
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

int fs_write_at(int idx, uint32_t off, const void *buf, uint32_t len)
{
    if (!ready || idx < 0 || idx >= FS_MAX_FILES || !inodes[idx].used)
        return -1;
    if (off + len > FS_MAX_SIZE)
        return -1;

    uint32_t flags = irq_save();
    uint32_t base = DATA_START + idx * FILE_SECTORS;
    const uint8_t *src = buf;
    uint32_t done = 0;

    while (done < len) {
        uint32_t pos = off + done;
        uint32_t sector = pos / ATA_SECTOR_SIZE;
        uint32_t soff = pos % ATA_SECTOR_SIZE;
        uint32_t chunk = ATA_SECTOR_SIZE - soff;
        if (chunk > len - done)
            chunk = len - done;

        /* Czytaj-modyfikuj-zapisz, bo zmieniamy fragment sektora */
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

    if (off + len > inodes[idx].size) {
        inodes[idx].size = off + len;
        flush_inodes();
    }

    irq_restore(flags);
    return (int)done;
}

int fs_get(int slot, char *name, uint32_t *size)
{
    if (!ready || slot < 0 || slot >= FS_MAX_FILES || !inodes[slot].used)
        return -1;
    uint32_t flags = irq_save();
    int i = 0;
    while (inodes[slot].name[i]) {
        name[i] = inodes[slot].name[i];
        i++;
    }
    name[i] = '\0';
    if (size)
        *size = inodes[slot].size;
    int ret = (int)inodes[slot].size;
    irq_restore(flags);
    return ret;
}

int fs_delete(const char *name)
{
    if (!ready)
        return -1;
    uint32_t flags = irq_save();
    int idx = find_inode(name);
    if (idx < 0) {
        irq_restore(flags);
        return -1;
    }
    inodes[idx].used = 0;
    flush_inodes();
    irq_restore(flags);
    return 0;
}

void fs_list(void)
{
    if (!ready) {
        printk("FS: brak dysku\n");
        return;
    }
    int n = 0;
    printk("Pliki na dysku:\n");
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (inodes[i].used) {
            printk("  %s  (%u B)\n", inodes[i].name, inodes[i].size);
            n++;
        }
    }
    if (!n)
        printk("  (dysk pusty)\n");
}
