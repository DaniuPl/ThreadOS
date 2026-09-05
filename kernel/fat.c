/* FAT12/16/32
 *
 * This is the one filesystem here that is not ours, and it is here for
 * exactly one reason: the EFI system partition. Firmware reads FAT and
 * reads nothing else, so a boot loader anywhere else will never be
 * started - and installing this system next to another one means
 * writing into a FAT partition that already exists, already has
 * somebody else's loader in it, and has to keep working afterwards.
 *
 * That is the whole shape of what follows. Reading is complete.
 * Writing does what an installer needs - a directory, a file, its
 * contents - and nothing else: nothing here formats, nothing here
 * moves anybody's files, nothing here rewrites a structure it did not
 * have to touch. Every write goes to all copies of the allocation
 * table, because the system that boots this machine tomorrow may read
 * either one.
 *
 * The layout, in order: the boot sector, then the reserved sectors,
 * then N copies of the allocation table, then - only on FAT12 and
 * FAT16 - a root directory of a fixed size, and then the data. Which
 * of the three formats it is is decided by nothing written in the boot
 * sector: it is decided by the NUMBER OF CLUSTERS, counted from the
 * sizes. That is not an awkwardness someone chose; it is the only
 * thing actually true of a FAT volume, and reading the name out of the
 * string in the boot sector - which is where everybody's first bug
 * lives - gets it wrong on volumes that say "FAT16   " and are not.
 */

#include "include/fat.h"
#include "include/blk.h"
#include "include/string.h"
#include "include/rtc.h"

#define SECTOR   512
#define ENTRY    32
#define PER_SEC  (SECTOR / ENTRY)

#define ATTR_RO     0x01
#define ATTR_LABEL  0x08
#define ATTR_DIR    0x10
#define ATTR_ARCH   0x20
#define ATTR_LFN    0x0F

static struct {
    int type;                  /* 12, 16, 32; 0 = nothing mounted */
    uint32_t base;             /* the partition's first sector */
    uint32_t sec_per_clus;
    uint32_t nfats;
    uint32_t fat_start;        /* absolute LBA of the first copy */
    uint32_t fat_sectors;      /* sectors in ONE copy */
    uint32_t root_entries;     /* FAT12/16 */
    uint32_t root_start;       /* FAT12/16: absolute LBA of the root */
    uint32_t root_sectors;
    uint32_t root_clus;        /* FAT32 */
    uint32_t data_start;       /* absolute LBA of cluster two */
    uint32_t clusters;         /* how many data clusters exist */
    uint32_t total_sectors;
    uint32_t fsinfo;           /* FAT32: absolute LBA, 0 = none */
    uint32_t hint;             /* where to start looking for a free cluster */
    char label[12];
} v;

/* Four buffers that must not be one buffer. The allocation table, a
   directory sector and a sector of file data are in use at the same
   moment: a file being written walks the table while its directory
   entry is half-built. Each of the first three is a cache of one
   sector, and each remembers which one - that is what keeps walking a
   directory from re-reading the same sector for every entry in it. */
static uint8_t fatbuf[SECTOR] __attribute__((aligned(16)));
static uint8_t dirbuf[SECTOR] __attribute__((aligned(16)));
static uint8_t datbuf[SECTOR] __attribute__((aligned(16)));
static uint8_t bsbuf[SECTOR] __attribute__((aligned(16)));
static uint32_t fat_rel = 0xFFFFFFFFu;   /* which sector of the table is held */
static uint32_t dirbuf_lba;              /* 0 = nothing valid in dirbuf */
static int fat_dirty;

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put16(uint8_t *p, uint16_t x)
{
    p[0] = (uint8_t)x;
    p[1] = (uint8_t)(x >> 8);
}

static void put32(uint8_t *p, uint32_t x)
{
    p[0] = (uint8_t)x;
    p[1] = (uint8_t)(x >> 8);
    p[2] = (uint8_t)(x >> 16);
    p[3] = (uint8_t)(x >> 24);
}

static char upcase(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

static char downcase(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static void copy_name(char *dst, const char *src, int max)
{
    int i = 0;
    while (src[i] && i < max - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

/* --- The allocation table ------------------------------------------
 *
 * One sector of it is held at a time. Everything else - walking a
 * chain, allocating, freeing - goes through single bytes, and that is
 * deliberate: on FAT12 an entry is twelve bits, and the entry that
 * straddles a sector boundary is exactly where a driver written for
 * whole entries breaks. Bytes straddle nothing.
 */

static int fat_flush(void)
{
    if (!fat_dirty || fat_rel == 0xFFFFFFFFu)
        return 0;
    for (uint32_t i = 0; i < v.nfats; i++)
        if (blk_write(v.fat_start + i * v.fat_sectors + fat_rel, 1, fatbuf) < 0)
            return -1;
    fat_dirty = 0;
    return 0;
}

static int fat_load(uint32_t rel)
{
    if (rel == fat_rel)
        return 0;
    if (fat_flush() < 0)
        return -1;
    if (rel >= v.fat_sectors)
        return -1;
    if (blk_read(v.fat_start + rel, 1, fatbuf) < 0)
        return -1;
    fat_rel = rel;
    return 0;
}

static int tab_get(uint32_t off, uint8_t *out)
{
    if (fat_load(off / SECTOR) < 0)
        return -1;
    *out = fatbuf[off % SECTOR];
    return 0;
}

static int tab_set(uint32_t off, uint8_t val)
{
    if (fat_load(off / SECTOR) < 0)
        return -1;
    fatbuf[off % SECTOR] = val;
    fat_dirty = 1;
    return 0;
}

static uint32_t chain_next(uint32_t clus)
{
    uint8_t a, b, c, d;
    if (clus < 2 || clus >= v.clusters + 2)
        return 0;
    if (v.type == 12) {
        uint32_t off = clus + clus / 2;
        if (tab_get(off, &a) < 0 || tab_get(off + 1, &b) < 0)
            return 0;
        uint32_t val = (uint32_t)a | ((uint32_t)b << 8);
        return (clus & 1) ? (val >> 4) : (val & 0xFFF);
    }
    if (v.type == 16) {
        if (tab_get(clus * 2, &a) < 0 || tab_get(clus * 2 + 1, &b) < 0)
            return 0;
        return (uint32_t)a | ((uint32_t)b << 8);
    }
    if (tab_get(clus * 4, &a) < 0 || tab_get(clus * 4 + 1, &b) < 0 ||
        tab_get(clus * 4 + 2, &c) < 0 || tab_get(clus * 4 + 3, &d) < 0)
        return 0;
    return ((uint32_t)a | ((uint32_t)b << 8) | ((uint32_t)c << 16) |
            ((uint32_t)d << 24)) & 0x0FFFFFFFu;
}

static int chain_put(uint32_t clus, uint32_t val)
{
    uint8_t a, b, top;
    if (clus < 2 || clus >= v.clusters + 2)
        return -1;
    if (v.type == 12) {
        uint32_t off = clus + clus / 2;
        if (tab_get(off, &a) < 0 || tab_get(off + 1, &b) < 0)
            return -1;
        if (clus & 1) {
            a = (uint8_t)((a & 0x0F) | ((val & 0x0F) << 4));
            b = (uint8_t)(val >> 4);
        } else {
            a = (uint8_t)val;
            b = (uint8_t)((b & 0xF0) | ((val >> 8) & 0x0F));
        }
        return (tab_set(off, a) < 0 || tab_set(off + 1, b) < 0) ? -1 : 0;
    }
    if (v.type == 16)
        return (tab_set(clus * 2, (uint8_t)val) < 0 ||
                tab_set(clus * 2 + 1, (uint8_t)(val >> 8)) < 0) ? -1 : 0;

    /* The top four bits of a FAT32 entry are not ours: the
       specification says to leave them exactly as they were */
    if (tab_get(clus * 4 + 3, &top) < 0)
        return -1;
    top = (uint8_t)((top & 0xF0) | ((val >> 24) & 0x0F));
    return (tab_set(clus * 4, (uint8_t)val) < 0 ||
            tab_set(clus * 4 + 1, (uint8_t)(val >> 8)) < 0 ||
            tab_set(clus * 4 + 2, (uint8_t)(val >> 16)) < 0 ||
            tab_set(clus * 4 + 3, top) < 0) ? -1 : 0;
}

static int is_eoc(uint32_t val)
{
    if (v.type == 12)
        return val >= 0xFF8;
    if (v.type == 16)
        return val >= 0xFFF8;
    return val >= 0x0FFFFFF8u;
}

static uint32_t eoc_value(void)
{
    if (v.type == 12)
        return 0xFFF;
    if (v.type == 16)
        return 0xFFFF;
    return 0x0FFFFFFFu;
}

static uint32_t clus_lba(uint32_t clus)
{
    return v.data_start + (clus - 2) * v.sec_per_clus;
}

/* --- A directory sector, held ---------------------------------------- */

static int dir_load(uint32_t lba)
{
    if (lba && lba == dirbuf_lba)
        return 0;
    if (blk_read(lba, 1, dirbuf) < 0) {
        dirbuf_lba = 0;
        return -1;
    }
    dirbuf_lba = lba;
    return 0;
}

static int dir_store(uint32_t lba)
{
    if (blk_write(lba, 1, dirbuf) < 0) {
        dirbuf_lba = 0;
        return -1;
    }
    dirbuf_lba = lba;
    return 0;
}

/* --- Writing: free clusters ---------------------------------------- */

/* FAT32 keeps a "how much is free" hint in a sector of its own. We
   change the table without recounting, so the honest thing to write
   there is "unknown" - a stale number is a number the next system to
   mount this volume will believe. */
static void fsinfo_unknown(void)
{
    if (v.type != 32 || !v.fsinfo)
        return;
    if (blk_read(v.fsinfo, 1, datbuf) < 0)
        return;
    if (le32(datbuf) != 0x41615252u || le32(datbuf + 484) != 0x61417272u)
        return;
    put32(datbuf + 488, 0xFFFFFFFFu);      /* free clusters: unknown */
    put32(datbuf + 492, 0xFFFFFFFFu);      /* where to look next: unknown */
    blk_write(v.fsinfo, 1, datbuf);
}

static uint32_t alloc_cluster(void)
{
    uint32_t start = v.hint < 2 ? 2 : v.hint;
    for (uint32_t n = 0; n < v.clusters; n++) {
        uint32_t c = start + n;
        if (c >= v.clusters + 2)
            c -= v.clusters;
        if (c < 2)
            continue;
        if (chain_next(c) == 0) {
            if (chain_put(c, eoc_value()) < 0)
                return 0;
            v.hint = c + 1;
            return c;
        }
    }
    return 0;
}

static int zero_cluster(uint32_t clus)
{
    memset(datbuf, 0, SECTOR);
    for (uint32_t s = 0; s < v.sec_per_clus; s++)
        if (blk_write(clus_lba(clus) + s, 1, datbuf) < 0)
            return -1;
    if (dirbuf_lba >= clus_lba(clus) &&
        dirbuf_lba < clus_lba(clus) + v.sec_per_clus)
        dirbuf_lba = 0;
    return 0;
}

static void free_chain(uint32_t clus)
{
    while (clus >= 2 && clus < v.clusters + 2) {
        uint32_t next = chain_next(clus);
        if (chain_put(clus, 0) < 0)
            break;
        if (v.hint > clus)
            v.hint = clus;
        if (is_eoc(next) || next < 2)
            break;
        clus = next;
    }
}

/* --- Directories as a sequence of sectors --------------------------
 *
 * A directory is either a chain of clusters or - the root of FAT12 and
 * FAT16 only - a fixed area right after the tables. The second kind
 * cannot grow, and when it fills there is nothing to be done about it.
 * Saying so is better than the alternative, which is writing past its
 * end into the first file on the volume.
 */
static uint32_t dir_sector(uint32_t clus, uint32_t n, int grow)
{
    if (clus == 0 && v.type != 32)
        return n < v.root_sectors ? v.root_start + n : 0;

    uint32_t c = clus ? clus : v.root_clus;
    uint32_t steps = n / v.sec_per_clus;
    while (steps--) {
        uint32_t next = chain_next(c);
        if (next < 2 || is_eoc(next) || next >= v.clusters + 2) {
            if (!grow)
                return 0;
            uint32_t fresh = alloc_cluster();
            if (!fresh || zero_cluster(fresh) < 0 || chain_put(c, fresh) < 0)
                return 0;
            next = fresh;
        }
        c = next;
    }
    return clus_lba(c) + n % v.sec_per_clus;
}

/* --- Names ---------------------------------------------------------- */

/* The eleven bytes of a short name as text. The two bits in the byte
   at offset twelve say the base or the extension was lower case when
   the name was written - Windows put them there so that 'readme.txt'
   would not come back as 'README.TXT', and honouring them costs two
   lines. */
static void short_text(const uint8_t *n11, int low_base, int low_ext,
                       char *out)
{
    int n = 0;
    for (int i = 0; i < 8 && n11[i] != ' '; i++)
        out[n++] = low_base ? downcase((char)n11[i]) : (char)n11[i];
    if (n11[8] != ' ') {
        out[n++] = '.';
        for (int i = 8; i < 11 && n11[i] != ' '; i++)
            out[n++] = low_ext ? downcase((char)n11[i]) : (char)n11[i];
    }
    out[n] = '\0';
}

static void short_name(const uint8_t *e, char *out)
{
    short_text(e, (e[12] & 0x08) != 0, (e[12] & 0x10) != 0, out);
}

static uint8_t short_sum(const uint8_t *name11)
{
    uint8_t s = 0;
    for (int i = 0; i < 11; i++)
        s = (uint8_t)(((s >> 1) | (uint8_t)(s << 7)) + name11[i]);
    return s;
}

/* The thirteen characters a long-name entry carries, at three places
   inside it that are not next to each other - the layout had to fit
   around the fields of the short entry it pretends to be. */
static const int LFN_OFF[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};

static int names_equal(const char *a, const char *b)
{
    while (*a && *b) {
        if (upcase(*a) != upcase(*b))
            return 0;
        a++;
        b++;
    }
    return *a == *b;
}

/* --- Reading a directory -------------------------------------------
 *
 * One pass collects both names: the long one out of the entries in
 * front, the short one out of the entry itself. A long name that is
 * interrupted - a missing piece, or a checksum that does not match the
 * short entry behind it - is thrown away rather than half-believed.
 */
struct scan {
    uint32_t dir;
    uint32_t sec;              /* sector index within the directory */
    int idx;                   /* entry index within that sector */
    char lfn[FAT_NAME_MAX];
    int lfn_ok;
    uint8_t sum;
};

static void scan_start(struct scan *s, uint32_t dir)
{
    s->dir = dir;
    s->sec = 0;
    s->idx = 0;
    s->lfn[0] = '\0';
    s->lfn_ok = 0;
    s->sum = 0;
}

/* The next entry that names something. Writes where it was found into
   *at_lba / *at_off, so that a caller which means to change it can. */
static int scan_next(struct scan *s, struct fat_entry *out,
                     uint32_t *at_lba, uint32_t *at_off)
{
    for (;;) {
        uint32_t lba = dir_sector(s->dir, s->sec, 0);
        if (!lba || dir_load(lba) < 0)
            return 0;

        while (s->idx < PER_SEC) {
            uint8_t *e = dirbuf + s->idx * ENTRY;
            uint32_t off = (uint32_t)s->idx * ENTRY;
            s->idx++;

            if (e[0] == 0x00)
                return 0;                    /* the end of the directory */
            if (e[0] == 0xE5) {
                s->lfn_ok = 0;
                continue;
            }
            if ((e[11] & ATTR_LFN) == ATTR_LFN) {
                int seq = e[0] & 0x3F;
                if (seq < 1 || seq > 20) {
                    s->lfn_ok = 0;
                    continue;
                }
                if (e[0] & 0x40) {           /* the last piece comes first */
                    for (int i = 0; i < FAT_NAME_MAX; i++)
                        s->lfn[i] = '\0';
                    s->lfn_ok = 1;
                    s->sum = e[13];
                } else if (!s->lfn_ok || s->sum != e[13]) {
                    s->lfn_ok = 0;
                    continue;
                }
                int at = (seq - 1) * 13;
                for (int i = 0; i < 13 && at + i < FAT_NAME_MAX - 1; i++) {
                    uint16_t c = le16(e + LFN_OFF[i]);
                    if (c == 0 || c == 0xFFFF)
                        break;
                    s->lfn[at + i] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
                }
                continue;
            }
            if (e[11] & ATTR_LABEL) {        /* the volume's own name */
                s->lfn_ok = 0;
                continue;
            }
            if (e[0] == '.') {               /* '.' and '..' */
                s->lfn_ok = 0;
                continue;
            }

            char sname[16];
            short_name(e, sname);
            if (s->lfn_ok && s->sum != short_sum(e))
                s->lfn_ok = 0;
            if (s->lfn_ok && s->lfn[0])
                copy_name(out->name, s->lfn, FAT_NAME_MAX);
            else
                copy_name(out->name, sname, FAT_NAME_MAX);
            out->size = le32(e + 28);
            out->clus = (uint32_t)le16(e + 26);
            if (v.type == 32)
                out->clus |= (uint32_t)le16(e + 20) << 16;
            out->is_dir = (e[11] & ATTR_DIR) != 0;
            s->lfn_ok = 0;
            if (at_lba)
                *at_lba = lba;
            if (at_off)
                *at_off = off;
            return 1;
        }
        s->idx = 0;
        s->sec++;
    }
}

int fat_readdir(uint32_t clus, int index, struct fat_entry *out)
{
    struct scan s;
    struct fat_entry e;
    if (!v.type || index < 0 || !out)
        return 0;
    scan_start(&s, clus);
    for (int i = 0; scan_next(&s, &e, 0, 0); i++)
        if (i == index) {
            *out = e;
            return 1;
        }
    return 0;
}

/* Looks for one name in one directory. */
static int lookup(uint32_t dir, const char *name, struct fat_entry *out,
                  uint32_t *at_lba, uint32_t *at_off)
{
    struct scan s;
    struct fat_entry e;
    scan_start(&s, dir);
    while (scan_next(&s, &e, at_lba, at_off))
        if (names_equal(e.name, name)) {
            if (out)
                *out = e;
            return 1;
        }
    return 0;
}

/* Cuts the next component out of a path. Returns where the rest
   begins, or 0 when there is nothing left. */
static const char *path_step(const char *p, char *comp)
{
    int n = 0;
    while (*p == '/' || *p == '\\')
        p++;
    if (!*p)
        return 0;
    while (*p && *p != '/' && *p != '\\' && n < FAT_NAME_MAX - 1)
        comp[n++] = *p++;
    comp[n] = '\0';
    return p;
}

/* Walks a path down to its last component, which is handed back
   separately. 1 = the parent directory exists. */
static int walk_parent(const char *path, uint32_t *dir, char *last)
{
    char comp[FAT_NAME_MAX], dummy[FAT_NAME_MAX];
    struct fat_entry e;
    uint32_t cur = 0;
    const char *p = path;

    last[0] = '\0';
    for (;;) {
        const char *rest = path_step(p, comp);
        if (!rest)
            break;
        if (!path_step(rest, dummy)) {       /* this one was the last */
            copy_name(last, comp, FAT_NAME_MAX);
            *dir = cur;
            return 1;
        }
        if (!lookup(cur, comp, &e, 0, 0) || !e.is_dir)
            return 0;
        cur = e.clus;
        p = rest;
    }
    *dir = cur;
    return 1;                                /* the path was the root */
}

int fat_stat(const char *path, struct fat_entry *out)
{
    char last[FAT_NAME_MAX];
    uint32_t dir;
    if (!v.type || !path)
        return 0;
    if (!walk_parent(path, &dir, last))
        return 0;
    if (!last[0]) {                          /* the root */
        if (out) {
            copy_name(out->name, "/", FAT_NAME_MAX);
            out->size = 0;
            out->clus = 0;
            out->is_dir = 1;
        }
        return 1;
    }
    return lookup(dir, last, out, 0, 0);
}

int fat_dir(const char *path, uint32_t *clus)
{
    struct fat_entry e;
    if (!fat_stat(path, &e) || !e.is_dir)
        return 0;
    if (clus)
        *clus = e.clus;
    return 1;
}

int fat_read(const char *path, uint32_t off, void *buf, uint32_t len)
{
    struct fat_entry e;
    uint8_t *dst = (uint8_t *)buf;
    uint32_t got = 0;

    if (!v.type || !fat_stat(path, &e) || e.is_dir)
        return -1;
    if (off >= e.size)
        return 0;
    if (len > e.size - off)
        len = e.size - off;

    uint32_t cbytes = v.sec_per_clus * SECTOR;
    uint32_t clus = e.clus;
    uint32_t skip = off / cbytes;
    while (skip--) {
        clus = chain_next(clus);
        if (clus < 2 || is_eoc(clus))
            return -1;
    }
    uint32_t inside = off % cbytes;

    while (got < len) {
        if (clus < 2 || is_eoc(clus) || clus >= v.clusters + 2)
            break;
        uint32_t sec = inside / SECTOR;
        while (sec < v.sec_per_clus && got < len) {
            if (blk_read(clus_lba(clus) + sec, 1, datbuf) < 0)
                return -1;
            uint32_t from = inside % SECTOR;
            uint32_t take = SECTOR - from;
            if (take > len - got)
                take = len - got;
            memcpy(dst + got, datbuf + from, take);
            got += take;
            inside += take;
            sec = inside / SECTOR;
        }
        inside = 0;
        clus = chain_next(clus);
    }
    return (int)got;
}

/* --- Mounting ------------------------------------------------------- */

const char *fat_kind(void)
{
    if (v.type == 12) return "FAT12";
    if (v.type == 16) return "FAT16";
    if (v.type == 32) return "FAT32";
    return "-";
}

const char *fat_label(void)
{
    return v.label;
}

int fat_mounted(void)
{
    return v.type != 0;
}

uint32_t fat_cluster_bytes(void)
{
    return v.type ? v.sec_per_clus * SECTOR : 0;
}

uint32_t fat_total_kib(void)
{
    return v.type ? (uint32_t)((uint64_t)v.clusters * v.sec_per_clus / 2) : 0;
}

uint32_t fat_free_kib(void)
{
    uint32_t free_clus = 0;
    if (!v.type)
        return 0;
    for (uint32_t c = 2; c < v.clusters + 2; c++)
        if (chain_next(c) == 0)
            free_clus++;
    return (uint32_t)((uint64_t)free_clus * v.sec_per_clus / 2);
}

void fat_umount(void)
{
    fat_flush();
    memset(&v, 0, sizeof(v));
    fat_rel = 0xFFFFFFFFu;
    dirbuf_lba = 0;
    fat_dirty = 0;
}

int fat_mount(uint32_t lba)
{
    fat_umount();
    if (!blk_present() || blk_read(lba, 1, bsbuf) < 0)
        return 0;

    /* Four things have to be true before any of the numbers are worth
       reading. A partition that is not FAT will fail one of them, and
       that matters more here than anywhere else in this system: on the
       other side of a wrong guess is somebody's disk. */
    if (bsbuf[510] != 0x55 || bsbuf[511] != 0xAA)
        return 0;
    if (bsbuf[0] != 0xEB && bsbuf[0] != 0xE9)
        return 0;
    if (le16(bsbuf + 11) != SECTOR)
        return 0;                       /* we speak 512-byte sectors only */
    uint32_t spc = bsbuf[13];
    if (!spc || (spc & (spc - 1)) || spc > 128)
        return 0;

    uint32_t reserved = le16(bsbuf + 14);
    uint32_t nfats = bsbuf[16];
    uint32_t root_entries = le16(bsbuf + 17);
    uint32_t total = le16(bsbuf + 19);
    uint32_t fat_secs = le16(bsbuf + 22);
    if (!reserved || !nfats || nfats > 4)
        return 0;
    if (!total)
        total = le32(bsbuf + 32);
    if (!fat_secs)
        fat_secs = le32(bsbuf + 36);
    if (!total || !fat_secs)
        return 0;

    uint32_t root_secs = (root_entries * ENTRY + SECTOR - 1) / SECTOR;
    uint32_t first_data = reserved + nfats * fat_secs + root_secs;
    if (first_data >= total)
        return 0;
    uint32_t clusters = (total - first_data) / spc;
    if (!clusters)
        return 0;

    v.base = lba;
    v.sec_per_clus = spc;
    v.nfats = nfats;
    v.fat_start = lba + reserved;
    v.fat_sectors = fat_secs;
    v.root_entries = root_entries;
    v.root_start = v.fat_start + nfats * fat_secs;
    v.root_sectors = root_secs;
    v.data_start = lba + first_data;
    v.clusters = clusters;
    v.total_sectors = total;
    v.hint = 2;

    /* The count of clusters, and nothing else, says which of the three
       this is. The thresholds are the specification's, off-by-one
       included: they are what every other driver uses, so a volume
       sitting on the boundary is read the same way by everyone. */
    if (clusters < 4085)
        v.type = 12;
    else if (clusters < 65525)
        v.type = 16;
    else
        v.type = 32;

    if (v.type == 32) {
        if (root_entries || le16(bsbuf + 22)) {
            v.type = 0;
            return 0;
        }
        v.root_clus = le32(bsbuf + 44);
        if (v.root_clus < 2 || v.root_clus >= clusters + 2) {
            v.type = 0;
            return 0;
        }
        uint32_t fsi = le16(bsbuf + 48);
        v.fsinfo = fsi ? lba + fsi : 0;
        memcpy(v.label, bsbuf + 71, 11);
    } else {
        memcpy(v.label, bsbuf + 43, 11);
    }
    v.label[11] = '\0';
    for (int i = 10; i >= 0 && v.label[i] == ' '; i--)
        v.label[i] = '\0';

    /* One reading of the table settles whether these numbers describe
       a filesystem at all: the first entry holds the media byte, and
       it has to be the one the boot sector gave. */
    uint8_t m;
    if (tab_get(0, &m) < 0 || m != bsbuf[21]) {
        v.type = 0;
        return 0;
    }
    return 1;
}

/* --- Writing -------------------------------------------------------- */

/* The characters a short name may have. Everything else - spaces,
   anything above ASCII, the punctuation the specification reserves -
   forces a long name. */
static int short_ok(char c)
{
    if (c >= 'A' && c <= 'Z') return 1;
    if (c >= '0' && c <= '9') return 1;
    return c == '$' || c == '%' || c == '\'' || c == '-' || c == '_' ||
           c == '@' || c == '~' || c == '`' || c == '!' || c == '(' ||
           c == ')' || c == '{' || c == '}' || c == '^' || c == '#' ||
           c == '&';
}

/* Does this name fit into eight characters, a dot and three - so that
   a single entry says all of it? Reports as well whether the base and
   the extension were lower case, which the entry can record. */
static int fits_short(const char *name, uint8_t *out11, int *low_base,
                      int *low_ext)
{
    int i, len, dot = -1;
    int lb = 0, le = 0, ub = 0, ue = 0;

    for (i = 0; name[i]; i++)
        if (name[i] == '.')
            dot = i;
    len = i;
    if (!len || len > 12)
        return 0;
    if (dot == 0 || (dot >= 0 && len - dot - 1 > 3) || (dot < 0 && len > 8))
        return 0;
    if (dot > 8)
        return 0;

    memset(out11, ' ', 11);
    for (i = 0; i < len; i++) {
        char c = name[i];
        if (i == dot)
            continue;
        int ext = (dot >= 0 && i > dot);
        if (c >= 'a' && c <= 'z') {
            if (ext) le = 1; else lb = 1;
        } else if (c >= 'A' && c <= 'Z') {
            if (ext) ue = 1; else ub = 1;
        }
        if (!short_ok(upcase(c)))
            return 0;
        out11[ext ? 8 + (i - dot - 1) : i] = (uint8_t)upcase(c);
    }
    /* A name mixing cases inside one part cannot be recorded with two
       bits, so it needs a long entry after all */
    if ((lb && ub) || (le && ue))
        return 0;
    *low_base = lb;
    *low_ext = le;
    return 1;
}

/* A short name for something whose real name does not fit: six
   characters, a tilde and a number, plus the first three of the
   extension. The number is bumped until nothing in the directory
   answers to it. */
static void make_short(uint32_t dir, const char *name, uint8_t *out11)
{
    int dot = -1;
    for (int i = 0; name[i]; i++)
        if (name[i] == '.')
            dot = i;

    for (int num = 1; num < 100; num++) {
        char text[16];
        int n = 0, limit = (num < 10) ? 6 : 5;
        memset(out11, ' ', 11);
        for (int i = 0; name[i] && n < limit; i++) {
            if (i == dot)
                break;
            char c = upcase(name[i]);
            if (short_ok(c))
                out11[n++] = (uint8_t)c;
        }
        if (!n)
            out11[n++] = 'X';
        out11[n++] = '~';
        if (num >= 10)
            out11[n++] = (uint8_t)('0' + num / 10);
        out11[n] = (uint8_t)('0' + num % 10);
        if (dot >= 0) {
            int e = 0;
            for (int i = dot + 1; name[i] && e < 3; i++) {
                char c = upcase(name[i]);
                if (short_ok(c))
                    out11[8 + e++] = (uint8_t)c;
            }
        }
        short_text(out11, 0, 0, text);
        if (!lookup(dir, text, 0, 0, 0))
            return;
    }
}

/* Finds 'need' free entries next to each other, growing the directory
   when there are none. Hands back the sector index and the entry index
   inside it. */
static int find_slots(uint32_t dir, int need, uint32_t *sec_out, int *idx_out)
{
    uint32_t sec = 0, run_sec = 0;
    int run = 0, run_idx = 0, grow = 0;

    for (;;) {
        uint32_t lba = dir_sector(dir, sec, grow);
        if (!lba) {
            if (grow)
                return -1;               /* the volume is full */
            /* The directory ended. A fixed root cannot be made longer,
               and there is no honest way around that. */
            if (dir == 0 && v.type != 32)
                return -1;
            grow = 1;
            continue;
        }
        if (dir_load(lba) < 0)
            return -1;
        for (int i = 0; i < PER_SEC; i++) {
            uint8_t first = dirbuf[i * ENTRY];
            if (first == 0x00 || first == 0xE5) {
                if (!run) {
                    run_sec = sec;
                    run_idx = i;
                }
                if (++run == need) {
                    *sec_out = run_sec;
                    *idx_out = run_idx;
                    return 0;
                }
            } else {
                run = 0;
            }
        }
        sec++;
    }
}

/* Writes one entry into a directory at (sector index, entry index). */
static int put_entry(uint32_t dir, uint32_t sec, int idx, const uint8_t *ent)
{
    uint32_t lba = dir_sector(dir, sec, 1);
    if (!lba || dir_load(lba) < 0)
        return -1;
    memcpy(dirbuf + idx * ENTRY, ent, ENTRY);
    return dir_store(lba);
}

static void now_stamp(uint16_t *date, uint16_t *time)
{
    struct rtc_time t;
    rtc_read(&t);
    if (t.year < 1980)
        t.year = 1980;
    if (!t.month) t.month = 1;
    if (!t.day) t.day = 1;
    *date = (uint16_t)((((uint32_t)t.year - 1980) << 9) |
                       ((uint32_t)t.month << 5) | t.day);
    *time = (uint16_t)(((uint32_t)t.hour << 11) | ((uint32_t)t.min << 5) |
                       (t.sec / 2));
}

/* Creates an entry: the long-name pieces when the name needs them, and
   then the entry itself. */
static int create_entry(uint32_t dir, const char *name, int is_dir,
                        uint32_t clus, uint32_t size)
{
    uint8_t name11[11], e[ENTRY];
    uint16_t date, time;
    int low_base = 0, low_ext = 0;
    int lfn = !fits_short(name, name11, &low_base, &low_ext);
    int chars = 0;
    uint32_t sec;
    int idx;

    while (name[chars])
        chars++;
    int pieces = lfn ? (chars + 12) / 13 : 0;
    if (pieces > 20)
        return -1;
    if (lfn)
        make_short(dir, name, name11);

    if (find_slots(dir, pieces + 1, &sec, &idx) < 0)
        return -1;

    uint8_t sum = short_sum(name11);
    for (int p = pieces; p >= 1; p--) {
        memset(e, 0, ENTRY);
        e[0] = (uint8_t)(p | (p == pieces ? 0x40 : 0));
        e[11] = ATTR_LFN;
        e[13] = sum;
        for (int i = 0; i < 13; i++) {
            int at = (p - 1) * 13 + i;
            uint16_t c;
            if (at < chars)
                c = (uint8_t)name[at];
            else if (at == chars)
                c = 0;
            else
                c = 0xFFFF;
            put16(e + LFN_OFF[i], c);
        }
        if (put_entry(dir, sec, idx, e) < 0)
            return -1;
        if (++idx == PER_SEC) {
            idx = 0;
            sec++;
        }
    }

    memset(e, 0, ENTRY);
    memcpy(e, name11, 11);
    e[11] = (uint8_t)(is_dir ? ATTR_DIR : ATTR_ARCH);
    e[12] = (uint8_t)((low_base ? 0x08 : 0) | (low_ext ? 0x10 : 0));
    now_stamp(&date, &time);
    put16(e + 14, time);
    put16(e + 16, date);
    put16(e + 18, date);
    put16(e + 22, time);
    put16(e + 24, date);
    put16(e + 26, (uint16_t)clus);
    put16(e + 20, (uint16_t)(clus >> 16));
    put32(e + 28, size);
    return put_entry(dir, sec, idx, e);
}

/* Changes the first cluster and the size of an entry already on the
   disk. */
static int touch_entry(uint32_t lba, uint32_t off, uint32_t clus,
                       uint32_t size)
{
    if (dir_load(lba) < 0)
        return -1;
    put16(dirbuf + off + 26, (uint16_t)clus);
    put16(dirbuf + off + 20, (uint16_t)(clus >> 16));
    put32(dirbuf + off + 28, size);
    return dir_store(lba);
}

int fat_mkdir(const char *path)
{
    char comp[FAT_NAME_MAX];
    struct fat_entry e;
    uint32_t cur = 0;
    const char *p = path;

    if (!v.type || !path)
        return -1;
    for (;;) {
        const char *rest = path_step(p, comp);
        if (!rest)
            break;
        if (lookup(cur, comp, &e, 0, 0)) {
            if (!e.is_dir)
                return -1;
            cur = e.clus;
            p = rest;
            continue;
        }

        uint32_t fresh = alloc_cluster();
        if (!fresh || zero_cluster(fresh) < 0)
            return -1;

        /* Every directory but the root opens with itself and its
           parent. A parent that is the root is written as cluster
           zero even on FAT32, where the root has a real number -
           that is what the specification asks for and what every
           other driver expects to find. */
        uint8_t d[ENTRY];
        uint16_t date, time;
        now_stamp(&date, &time);
        memset(dirbuf, 0, SECTOR);
        memset(d, 0, ENTRY);
        memset(d, ' ', 11);
        d[0] = '.';
        d[11] = ATTR_DIR;
        put16(d + 22, time);
        put16(d + 24, date);
        put16(d + 26, (uint16_t)fresh);
        put16(d + 20, (uint16_t)(fresh >> 16));
        memcpy(dirbuf, d, ENTRY);
        d[1] = '.';
        put16(d + 26, (uint16_t)cur);
        put16(d + 20, (uint16_t)(cur >> 16));
        memcpy(dirbuf + ENTRY, d, ENTRY);
        if (dir_store(clus_lba(fresh)) < 0)
            return -1;

        if (create_entry(cur, comp, 1, fresh, 0) < 0)
            return -1;
        cur = fresh;
        p = rest;
    }
    if (fat_flush() < 0)
        return -1;
    fsinfo_unknown();
    return 0;
}

int fat_write_file(const char *path, const void *data, uint32_t len)
{
    char last[FAT_NAME_MAX];
    uint32_t dir, at_lba = 0, at_off = 0;
    struct fat_entry e;
    const uint8_t *src = (const uint8_t *)data;

    if (!v.type || !path)
        return -1;
    if (!walk_parent(path, &dir, last) || !last[0])
        return -1;

    int existed = lookup(dir, last, &e, &at_lba, &at_off);
    if (existed && e.is_dir)
        return -1;
    if (existed && e.clus >= 2)
        free_chain(e.clus);

    /* The contents first, the entry afterwards: a file whose entry
       says "these clusters" before the clusters hold anything is a
       file that reads as rubbish if the power goes out in between. */
    uint32_t first = 0, prev = 0, done = 0;
    while (done < len) {
        uint32_t c = alloc_cluster();
        if (!c)
            return -1;
        if (prev && chain_put(prev, c) < 0)
            return -1;
        if (!first)
            first = c;
        for (uint32_t s = 0; s < v.sec_per_clus && done < len; s++) {
            uint32_t take = len - done;
            if (take > SECTOR)
                take = SECTOR;
            memset(datbuf, 0, SECTOR);
            memcpy(datbuf, src + done, take);
            if (blk_write(clus_lba(c) + s, 1, datbuf) < 0)
                return -1;
            done += take;
        }
        prev = c;
    }
    if (fat_flush() < 0)
        return -1;

    if (existed) {
        if (touch_entry(at_lba, at_off, first, len) < 0)
            return -1;
    } else if (create_entry(dir, last, 0, first, len) < 0) {
        return -1;
    }
    if (fat_flush() < 0)
        return -1;
    fsinfo_unknown();
    return 0;
}

int fat_unlink(const char *path)
{
    char last[FAT_NAME_MAX];
    uint32_t dir, at_lba = 0, at_off = 0;
    struct fat_entry e;

    if (!v.type || !path)
        return -1;
    if (!walk_parent(path, &dir, last) || !last[0])
        return -1;
    if (!lookup(dir, last, &e, &at_lba, &at_off) || e.is_dir)
        return -1;
    if (e.clus >= 2)
        free_chain(e.clus);

    /* The long-name entries sit right in front of the short one, and
       an entry left behind with nothing to belong to is exactly what
       makes another driver show a file that is not there. */
    if (dir_load(at_lba) < 0)
        return -1;
    uint8_t sum = short_sum(dirbuf + at_off);
    dirbuf[at_off] = 0xE5;
    for (int i = (int)(at_off / ENTRY) - 1; i >= 0; i--) {
        uint8_t *p = dirbuf + i * ENTRY;
        if ((p[11] & ATTR_LFN) != ATTR_LFN || p[13] != sum)
            break;
        p[0] = 0xE5;
    }
    if (dir_store(at_lba) < 0)
        return -1;
    if (fat_flush() < 0)
        return -1;
    fsinfo_unknown();
    return 0;
}
