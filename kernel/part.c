/* Reading a partition table - MBR and GPT
 *
 * Two formats, one of which exists to hold the other's place. The old
 * table lives in the last 66 bytes of sector zero: four entries of
 * sixteen bytes and a signature. GPT keeps its own table further in,
 * and leaves in sector zero a "protective" MBR - one entry of type
 * 0xEE covering the whole disk - so that a program that knows only the
 * old format sees a disk that is full rather than a disk that is
 * empty, and does not helpfully offer to partition it.
 *
 * That single byte is the whole of the detection: type 0xEE in sector
 * zero means look at sector one.
 *
 * Nothing here writes. The point of reading is to be able to say what
 * is on a disk before anyone proposes to change it.
 */

#include "include/part.h"
#include "include/blk.h"
#include "include/string.h"

#define SECTOR 512

static struct partition parts[PART_MAX];
static int count;
static int is_gpt;

static uint8_t sect[SECTOR] __attribute__((aligned(16)));
static uint8_t entries[SECTOR * 8] __attribute__((aligned(16)));

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t le64(const uint8_t *p)
{
    return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
}

static void say(char *dst, const char *s)
{
    int i = 0;
    while (s[i] && i < PART_NAME - 1) {
        dst[i] = s[i];
        i++;
    }
    dst[i] = '\0';
}

/* What the old table's type byte means. Only the ones worth naming -
   the rest is a number, which is still more than nothing. */
static const char *mbr_kind(uint8_t t)
{
    switch (t) {
    case 0x00: return "empty";
    case 0x07: return "NTFS/exFAT";
    case 0x0B: case 0x0C: return "FAT32";
    case 0x0E: return "FAT16";
    case 0x82: return "Linux swap";
    case 0x83: return "Linux";
    case 0xEE: return "GPT protective";
    case 0xEF: return "EFI (ESP)";
    default:   return "unknown";
    }
}

/* GPT names a partition by a sixteen-byte type GUID. Comparing whole
   GUIDs would be a table of them; the first four bytes are enough to
   tell apart the three that matter here, and anything else honestly
   says it does not know. */
static const char *gpt_kind(const uint8_t *guid)
{
    uint32_t first = le32(guid);
    switch (first) {
    case 0xC12A7328: return "EFI (ESP)";
    case 0xEBD0A0A2: return "data (NTFS/FAT)";
    case 0x0FC63DAF: return "Linux";
    case 0x0657FD6D: return "Linux swap";
    case 0xE3C9E316: return "Microsoft reserved";
    case 0xDE94BBA4: return "Windows recovery";
    default:         return "unknown";
    }
}

/* A GPT name is UTF-16 and we draw an eight-bit font, so anything
   above ASCII becomes a dot rather than half a character. */
static void gpt_label(char *dst, const uint8_t *p)
{
    int n = 0;
    for (int i = 0; i < 72 && n < PART_NAME - 1; i += 2) {
        uint16_t c = (uint16_t)(p[i] | (p[i + 1] << 8));
        if (!c)
            break;
        dst[n++] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
    }
    dst[n] = '\0';
}

static int scan_gpt(void)
{
    if (blk_read(1, 1, sect) < 0)
        return 0;
    if (memcmp(sect, "EFI PART", 8) != 0)
        return 0;

    uint64_t table = le64(sect + 72);
    uint32_t n = le32(sect + 80);
    uint32_t size = le32(sect + 84);
    if (!size || size > SECTOR || n > 128)
        return 0;

    uint32_t bytes = n * size;
    uint32_t secs = (bytes + SECTOR - 1) / SECTOR;
    if (secs > sizeof(entries) / SECTOR)
        secs = sizeof(entries) / SECTOR;
    if (blk_read((uint32_t)table, secs, entries) < 0)
        return 0;

    is_gpt = 1;
    count = 0;
    for (uint32_t i = 0; i < n && count < PART_MAX; i++) {
        const uint8_t *e = entries + (uint32_t)i * size;
        if ((uint32_t)i * size + size > secs * SECTOR)
            break;
        /* An all-zero type GUID means the slot is unused */
        uint64_t lo = le64(e), hi = le64(e + 8);
        if (!lo && !hi)
            continue;
        parts[count].start = le64(e + 32);
        parts[count].sectors = le64(e + 40) - le64(e + 32) + 1;
        say(parts[count].kind, gpt_kind(e));
        gpt_label(parts[count].label, e + 56);
        count++;
    }
    return count;
}

int part_scan(void)
{
    count = 0;
    is_gpt = 0;
    if (!blk_present())
        return -1;
    if (blk_read(0, 1, sect) < 0)
        return -1;
    if (sect[510] != 0x55 || sect[511] != 0xAA)
        return 0;                  /* no table - a disk that is all ours */

    /* The protective entry: GPT saying "do not touch, look further in" */
    for (int i = 0; i < 4; i++)
        if (sect[446 + i * 16 + 4] == 0xEE)
            return scan_gpt();

    for (int i = 0; i < 4; i++) {
        const uint8_t *e = sect + 446 + i * 16;
        uint8_t type = e[4];
        uint32_t start = le32(e + 8), n = le32(e + 12);
        if (!type || !n)
            continue;
        parts[count].start = start;
        parts[count].sectors = n;
        say(parts[count].kind, mbr_kind(type));
        parts[count].label[0] = '\0';
        count++;
    }
    return count;
}

int part_is_gpt(void)
{
    return is_gpt;
}

int part_get(int index, struct partition *out)
{
    if (index < 0 || index >= count || !out)
        return 0;
    *out = parts[index];
    return 1;
}
