/* Application packages (.tapp, scripts/mktapp.py), read and checked in
 * ring 3 - by tapp, in the terminal, and by gtapp, the window a package
 * opens in from the Files app.
 *
 * A package is an image in the initrd's format: a manifest first, the
 * programs and data, and a signature last - ECDSA P-256 over the SHA-256
 * of every byte before it, by the key that signs ThreadOS
 * (user/usignkey.h). It is read once, start to end, a piece at a time,
 * so a large one (Weft, 180 MB) is never in memory twice.
 */
#ifndef UTAPP_H
#define UTAPP_H

#include "uapi.h"
#include "usha256.h"
#include "uecdsa.h"
#include "usignkey.h"

#define TAPP_ENTRIES_MAX 256
#define TAPP_CHUNK 65536

struct tapp_entry {
    char name[16];
    uint32_t offset, size;
};

/* Where the bytes come from: a package the loader brought (by index),
   a file, or a package already in memory */
struct tapp_src {
    int pkg;              /* the package's index, or -1 */
    int fd;               /* the file, when pkg < 0 and there is no mem */
    const uint8_t *mem;
    uint32_t mem_len;
};

/* What a package says of itself */
struct tapp_pkg {
    struct tapp_entry entries[TAPP_ENTRIES_MAX];
    int count;
    uint32_t length;              /* where its last entry ends */
    char manifest[1024];          /* the text, cut at this length */
    char id[16], name[32], name_pl[32], exec[16], arch[16];
    uint32_t version;
    int dock;
};

/* What checking it found */
#define TAPP_SIGNED      0        /* by the system's key */
#define TAPP_NOT_SIGNED  1        /* a signature, not by that key */
#define TAPP_UNSIGNED    2        /* none, or an empty one */
#define TAPP_NOT_PACKAGE 3
#define TAPP_UNREADABLE  4        /* it ends before its listing says */

static uint8_t tapp_chunk[TAPP_CHUNK];

static inline int tapp_streq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static inline int64_t tapp_read(struct tapp_src *s, uint32_t off, void *buf,
                                uint32_t len)
{
    if (s->pkg >= 0)
        return sys_package_read(s->pkg, off, buf, len);
    if (s->mem) {
        if (off > s->mem_len)
            return -1;
        uint32_t n = s->mem_len - off < len ? s->mem_len - off : len;
        for (uint32_t i = 0; i < n; i++)
            ((uint8_t *)buf)[i] = s->mem[off + i];
        return n;
    }
    if (sys_seek(s->fd, off) < 0)
        return -1;
    uint32_t got = 0;
    while (got < len) {
        int64_t n = sys_read(s->fd, (uint8_t *)buf + got, len - got);
        if (n <= 0)
            break;
        got += (uint32_t)n;
    }
    return got;
}

/* One "key value" line of the manifest into a field */
static inline void tapp_field(const char *line, int len, const char *key,
                              char *out, int room)
{
    int k = 0;
    while (key[k] && k < len && line[k] == key[k])
        k++;
    if (key[k] || k >= len || line[k] != ' ')
        return;
    int n = 0;
    for (k++; k < len && n < room - 1; k++)
        out[n++] = line[k];
    out[n] = 0;
}

/* The listing, checked as the kernel checks it (kernel/initrd.c), and
   the manifest read. 0, or TAPP_NOT_PACKAGE with 'why' said. */
static inline int tapp_open(struct tapp_src *s, struct tapp_pkg *p,
                            const char **why)
{
    uint8_t head[8];
    for (uint32_t i = 0; i < sizeof(*p); i++)
        ((uint8_t *)p)[i] = 0;
    if (tapp_read(s, 0, head, 8) != 8 || head[0] != 'T' ||
        head[1] != 'I' || head[2] != 'R' || head[3] != 'D') {
        *why = "not an image in the initrd's format";
        return TAPP_NOT_PACKAGE;
    }
    uint32_t count = head[4] | head[5] << 8 | head[6] << 16 |
                     (uint32_t)head[7] << 24;
    if (!count || count > TAPP_ENTRIES_MAX ||
        tapp_read(s, 8, p->entries, count * 24) != (int64_t)count * 24) {
        *why = "a listing it cannot have";
        return TAPP_NOT_PACKAGE;
    }
    uint32_t end = 8 + count * 24;
    for (uint32_t i = 0; i < count; i++) {
        int n = 0;
        while (n < 16 && p->entries[i].name[n])
            n++;
        if (n == 16 || p->entries[i].offset < 8 + count * 24 ||
            p->entries[i].offset + p->entries[i].size <
                p->entries[i].offset) {
            *why = "an entry outside it";
            return TAPP_NOT_PACKAGE;
        }
        if (p->entries[i].offset + p->entries[i].size > end)
            end = p->entries[i].offset + p->entries[i].size;
    }
    if (!tapp_streq(p->entries[0].name, "manifest")) {
        *why = "no manifest first";
        return TAPP_NOT_PACKAGE;
    }
    uint8_t past;
    if (s->pkg < 0 && !s->mem && tapp_read(s, end, &past, 1) != 0) {
        *why = "bytes after its last entry";
        return TAPP_NOT_PACKAGE;
    }
    p->count = (int)count;
    p->length = end;

    uint32_t mlen = p->entries[0].size < sizeof(p->manifest) - 1
                    ? p->entries[0].size : sizeof(p->manifest) - 1;
    int64_t got = tapp_read(s, p->entries[0].offset, p->manifest, mlen);
    p->manifest[got > 0 ? got : 0] = 0;
    static const char magic[] = "ThreadOS app\n";
    for (int i = 0; magic[i]; i++)
        if (p->manifest[i] != magic[i]) {
            *why = "a manifest that does not say what it is";
            return TAPP_NOT_PACKAGE;
        }
    char version[12] = "", dock[8] = "";
    for (const char *line = p->manifest; *line; ) {
        int len = 0;
        while (line[len] && line[len] != '\n')
            len++;
        tapp_field(line, len, "id", p->id, sizeof(p->id));
        tapp_field(line, len, "name", p->name, sizeof(p->name));
        tapp_field(line, len, "name.pl", p->name_pl, sizeof(p->name_pl));
        tapp_field(line, len, "version", version, sizeof(version));
        tapp_field(line, len, "arch", p->arch, sizeof(p->arch));
        tapp_field(line, len, "exec", p->exec, sizeof(p->exec));
        tapp_field(line, len, "dock", dock, sizeof(dock));
        line += len + (line[len] == '\n');
    }
    for (const char *c = version; *c >= '0' && *c <= '9'; c++)
        p->version = p->version * 10 + (uint32_t)(*c - '0');
    p->dock = tapp_streq(dock, "yes");
    if (!p->id[0] || !p->name[0] || !p->exec[0]) {
        *why = "a manifest without its id, name or program";
        return TAPP_NOT_PACKAGE;
    }
    return 0;
}

/* The signature against the system's key: TAPP_SIGNED, TAPP_NOT_SIGNED,
   TAPP_UNSIGNED or TAPP_UNREADABLE. 'from' says where the key came from
   (SIGNKEY_*). */
static inline int tapp_verify(struct tapp_src *s, const struct tapp_pkg *p,
                              int *from)
{
    const struct tapp_entry *last = &p->entries[p->count - 1];
    *from = SIGNKEY_OFFICIAL;
    if (!tapp_streq(last->name, "signature") || last->size != 64 ||
        last->offset + 64 != p->length)
        return TAPP_UNSIGNED;
    struct sha256 h;
    sha256_init(&h);
    for (uint32_t at = 0; at < last->offset; ) {
        uint32_t want = last->offset - at < TAPP_CHUNK
                        ? last->offset - at : TAPP_CHUNK;
        if (tapp_read(s, at, tapp_chunk, want) != want)
            return TAPP_UNREADABLE;
        sha256_update(&h, tapp_chunk, want);
        at += want;
    }
    uint8_t digest[32], sig[64];
    sha256_final(&h, digest);
    if (tapp_read(s, last->offset, sig, 64) != 64)
        return TAPP_UNREADABLE;
    int zero = 1;
    for (int i = 0; i < 64; i++)
        zero &= !sig[i];
    if (zero)
        return TAPP_UNSIGNED;
    const uint8_t *key = signing_key_from(from);
    return ecdsa_verify(ec_curve_p256(), key, 65, digest, 32, sig, 32,
                        sig + 32, 32) ? TAPP_SIGNED : TAPP_NOT_SIGNED;
}

/* A package file read into memory, whole: its bytes, or NULL with 'why' */
static inline uint8_t *tapp_load(const char *path, struct tapp_pkg *p,
                                 const char **why)
{
    struct tapp_src f = { -1, (int)sys_open(path, O_READ), 0, 0 };
    if (f.fd < 0) {
        *why = "it cannot be opened";
        return 0;
    }
    if (tapp_open(&f, p, why) != 0) {
        sys_close(f.fd);
        return 0;
    }
    uint8_t *buf = malloc(p->length);
    if (!buf || tapp_read(&f, 0, buf, p->length) != (int64_t)p->length) {
        *why = "it cannot be read into memory";
        sys_close(f.fd);
        return 0;
    }
    sys_close(f.fd);
    return buf;
}

#endif
