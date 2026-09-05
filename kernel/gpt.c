/* Writing a GPT
 *
 * part.c reads tables and writes nothing on purpose. This writes one,
 * and it exists for a single case: a machine on which ProtonOS is to
 * be the only system needs a table, and nobody else is going to make
 * it one.
 *
 * The shape is the format's and there is no room for opinions in it:
 * a protective MBR in sector zero, the header in sector one, a hundred
 * and twenty-eight entries of a hundred and twenty-eight bytes right
 * behind it - and the same entries and the same header again at the
 * very end of the disk, backwards. The last two sectors of a GPT disk
 * are not spare space; they are the copy that makes a torn front
 * repairable.
 *
 * Both headers carry a CRC of themselves and a CRC of the whole entry
 * array. Getting those wrong does not fail loudly: the firmware
 * quietly decides the table is damaged, and a machine that will not
 * boot says nothing about why.
 */

#include "include/gpt.h"
#include "include/blk.h"
#include "include/string.h"

#define SECTOR   512
#define ENTRIES  128
#define ENT_SIZE 128
#define ENT_SECT ((ENTRIES * ENT_SIZE) / SECTOR)     /* 32 */

static uint8_t sect[SECTOR] __attribute__((aligned(16)));
static uint8_t table[ENTRIES * ENT_SIZE] __attribute__((aligned(16)));

/* CRC-32, the one everybody means by that name (the reflected
   polynomial 0xEDB88320). Computed a byte at a time from a small
   table built once - a table of 256 entries is a kilobyte and saves
   eight shifts per byte over sixteen kilobytes of entries. */
static uint32_t crc_tab[256];
static int crc_ready;

static void crc_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc_tab[i] = c;
    }
    crc_ready = 1;
}

static uint32_t crc32(const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t c = 0xFFFFFFFFu;
    if (!crc_ready)
        crc_init();
    while (len--)
        c = crc_tab[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put64(uint8_t *p, uint64_t v)
{
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}

/* A GUID as sixteen bytes in the order the format wants them: the
   first three fields little-endian, the last two as written. Spelled
   out here rather than parsed from a string, because a parser for
   three constants is more code than the constants. */
static const uint8_t GUID_ESP[16] = {
    0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
    0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B
};

/* Ours. Not borrowed from anybody: the bytes spell PROTONOS-PFS3, so
   a person looking at a table with a hex editor can tell whose it is,
   and no other system will mistake it for one of its own. */
static const uint8_t GUID_PFS[16] = {
    'P', 'R', 'O', 'T', 'O', 'N', 'O', 'S',
    '-', 'P', 'F', 'S', '3', 0, 0, 1
};

/* Disk and partition identities. A GUID is supposed to be unique and
   we have no source of randomness worth the name at this point, so
   these are built from the disk's own numbers plus a counter - unique
   on this machine, which is the property that actually matters here,
   and honest about being no more than that. */
static void make_uuid(uint8_t *out, uint32_t seed, uint32_t n)
{
    for (int i = 0; i < 16; i++)
        out[i] = (uint8_t)(seed >> ((i & 3) * 8));
    out[0] = (uint8_t)n;
    out[15] = (uint8_t)(0x80 | (n & 0x3F));
}

/* A partition's name, which GPT keeps as UTF-16 */
static void put_name(uint8_t *p, const char *s)
{
    int i = 0;
    for (; s[i] && i < 35; i++) {
        p[i * 2] = (uint8_t)s[i];
        p[i * 2 + 1] = 0;
    }
    for (; i < 36; i++) {
        p[i * 2] = 0;
        p[i * 2 + 1] = 0;
    }
}

static int write_header(uint64_t self, uint64_t other, uint64_t ent_lba,
                        uint64_t first, uint64_t last, const uint8_t *disk_id,
                        uint32_t table_crc)
{
    memset(sect, 0, SECTOR);
    memcpy(sect, "EFI PART", 8);
    put32(sect + 8, 0x00010000);           /* revision 1.0 */
    put32(sect + 12, 92);                  /* the header's own size */
    put64(sect + 24, self);
    put64(sect + 32, other);
    put64(sect + 40, first);
    put64(sect + 48, last);
    memcpy(sect + 56, disk_id, 16);
    put64(sect + 72, ent_lba);
    put32(sect + 80, ENTRIES);
    put32(sect + 84, ENT_SIZE);
    put32(sect + 88, table_crc);
    /* The header's CRC is computed with its own field zeroed, which
       it already is */
    put32(sect + 16, crc32(sect, 92));
    return blk_write((uint32_t)self, 1, sect);
}

int gpt_make(uint32_t esp_mib)
{
    if (!blk_present())
        return -1;

    uint32_t total = blk_sectors();
    if (!total || total / 2048 < GPT_MIN_MIB)
        return -1;

    uint64_t esp_start = 2048;
    uint64_t esp_sectors = (uint64_t)esp_mib * 2048;
    uint64_t sys_start = esp_start + esp_sectors;
    /* The last thirty-four sectors belong to the copy at the end: the
       header and the entries again. A partition that reaches into them
       is a partition that eats its own table. */
    uint64_t last_usable = (uint64_t)total - 34;
    if (sys_start + 2048 > last_usable)
        return -1;                          /* nothing left for a system */

    uint8_t disk_id[16];
    make_uuid(disk_id, total, 0);

    memset(table, 0, sizeof(table));

    uint8_t *e = table;
    memcpy(e, GUID_ESP, 16);
    make_uuid(e + 16, total, 1);
    put64(e + 32, esp_start);
    put64(e + 40, sys_start - 1);
    put_name(e + 56, "EFI");

    e = table + ENT_SIZE;
    memcpy(e, GUID_PFS, 16);
    make_uuid(e + 16, total, 2);
    put64(e + 32, sys_start);
    put64(e + 40, last_usable);
    put_name(e + 56, "ProtonOS");

    uint32_t table_crc = crc32(table, sizeof(table));

    /* The entries first, both copies, and only then the headers that
       point at them: a header found without its entries is a table a
       firmware will refuse, and that is the right way round for the
       moment the power goes out. */
    uint64_t back_ent = last_usable + 1;          /* total - 33 */
    for (uint32_t i = 0; i < ENT_SECT; i++) {
        if (blk_write(2 + i, 1, table + i * SECTOR) < 0)
            return -1;
        if (blk_write((uint32_t)back_ent + i, 1, table + i * SECTOR) < 0)
            return -1;
    }

    if (write_header(1, total - 1, 2, esp_start, last_usable, disk_id,
                     table_crc) < 0)
        return -1;
    if (write_header(total - 1, 1, back_ent, esp_start, last_usable, disk_id,
                     table_crc) < 0)
        return -1;

    /* The protective MBR last, because it is the thing every other
       program looks at first: until it is there, the disk still says
       "empty" to anyone reading sector zero. One entry of type 0xEE
       covering everything. */
    memset(sect, 0, SECTOR);
    sect[446 + 0] = 0x00;                  /* not bootable */
    sect[446 + 1] = 0x00;                  /* CHS: nobody reads it */
    sect[446 + 2] = 0x02;
    sect[446 + 3] = 0x00;
    sect[446 + 4] = 0xEE;
    sect[446 + 5] = 0xFF;
    sect[446 + 6] = 0xFF;
    sect[446 + 7] = 0xFF;
    put32(sect + 446 + 8, 1);
    put32(sect + 446 + 12, total - 1 > 0xFFFFFFFFu ? 0xFFFFFFFFu : total - 1);
    put16(sect + 510, 0xAA55);
    return blk_write(0, 1, sect);
}
