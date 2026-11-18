/* tapp - application packages (.tapp, scripts/mktapp.py)
 *
 *   tapp                 the packages the loader brought: their names,
 *   tapp list            versions and sizes
 *   tapp info FILE       what a package file says it is, and what it holds
 *   tapp verify NAME     whether a package is signed by the key that signs
 *                        ThreadOS (user/usignkey.h) - NAME is a package
 *                        the system has, or a package file
 *   tapp install FILE    a package file, signed by that key, onto the
 *                        installed system: into /EFI/ThreadOS/apps, where
 *                        the loader finds it, and running at once where
 *                        it can (an administrator's to do: sudo)
 *
 * A package is an image in the initrd's format: a manifest first, the
 * programs and data, and a signature last - ECDSA P-256 over the SHA-256
 * of every byte before it. Checking reads the package once, start to
 * end, so a large one (Weft, 180 MB) is never in this program's memory
 * at once.
 *
 * verify ends with 0 for a package signed by the system's key, 1 for
 * one that is not (or has no signature), 2 for a file that is not a
 * package. install ends with 0 when the package is on the disk, and
 * says whether it runs now or from the next start.
 */

#include "uapi.h"
#include "usha256.h"
#include "uecdsa.h"
#include "usignkey.h"

#define ENTRIES_MAX 256
#define CHUNK 65536

static uint8_t chunk[CHUNK];

struct entry {
    char name[16];
    uint32_t offset, size;
};
static struct entry entries[ENTRIES_MAX];

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static void say(const char *a, const char *b, const char *c)
{
    print(a);
    if (b)
        print(b);
    if (c)
        print(c);
}

/* Where the bytes come from: a package the loader brought, by its index,
   or a file */
struct source {
    int pkg;              /* the package's index, or -1 */
    int fd;               /* the file, when pkg < 0 and there is no mem */
    const uint8_t *mem;   /* or the package read into memory, */
    uint32_t mem_len;     /* this long */
};

static int64_t source_read(struct source *s, uint32_t off, void *buf,
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

/* The listing, checked as the kernel checks it (kernel/initrd.c): the
   magic, each entry inside the package. The package's length is where
   its last entry ends - nothing may follow it. The count, or -1 with
   'why' said. */
static int listing(struct source *s, uint32_t *length, const char **why)
{
    uint8_t head[8];
    if (source_read(s, 0, head, 8) != 8 || head[0] != 'T' ||
        head[1] != 'I' || head[2] != 'R' || head[3] != 'D') {
        *why = "not an image in the initrd's format";
        return -1;
    }
    uint32_t count = head[4] | head[5] << 8 | head[6] << 16 |
                     (uint32_t)head[7] << 24;
    if (!count || count > ENTRIES_MAX ||
        source_read(s, 8, entries, count * 24) != (int64_t)count * 24) {
        *why = "a listing it cannot have";
        return -1;
    }
    uint32_t end = 8 + count * 24;
    for (uint32_t i = 0; i < count; i++) {
        int n = 0;
        while (n < 16 && entries[i].name[n])
            n++;
        if (n == 16 || entries[i].offset < 8 + count * 24 ||
            entries[i].offset + entries[i].size < entries[i].offset) {
            *why = "an entry outside it";
            return -1;
        }
        if (entries[i].offset + entries[i].size > end)
            end = entries[i].offset + entries[i].size;
    }
    if (!streq(entries[0].name, "manifest")) {
        *why = "no manifest first";
        return -1;
    }
    uint8_t past;
    if (source_read(s, end, &past, 1) != 0 && s->pkg < 0 && !s->mem) {
        *why = "bytes after its last entry";
        return -1;
    }
    *length = end;
    return (int)count;
}

/* 0 signed by the key, 1 not, 2 not a package */
static int verify(struct source *s, const char *what)
{
    uint32_t length;
    const char *why;
    int count = listing(s, &length, &why);
    if (count < 0) {
        say(what, ": not a package - ", why);
        print("\n");
        return 2;
    }
    const struct entry *last = &entries[count - 1];
    if (!streq(last->name, "signature") || last->size != 64 ||
        last->offset + 64 != length) {
        say(what, ": has no signature\n", 0);
        return 1;
    }
    struct sha256 h;
    sha256_init(&h);
    for (uint32_t at = 0; at < last->offset; ) {
        uint32_t want = last->offset - at < CHUNK ? last->offset - at : CHUNK;
        if (source_read(s, at, chunk, want) != want) {
            say(what, ": cannot be read to its end\n", 0);
            return 2;
        }
        sha256_update(&h, chunk, want);
        at += want;
    }
    uint8_t digest[32], sig[64];
    sha256_final(&h, digest);
    if (source_read(s, last->offset, sig, 64) != 64) {
        say(what, ": cannot be read to its end\n", 0);
        return 2;
    }
    int zero = 1;
    for (int i = 0; i < 64; i++)
        zero &= !sig[i];
    if (zero) {
        say(what, ": has no signature (an unsigned build)\n", 0);
        return 1;
    }
    int from;
    const uint8_t *key = signing_key_from(&from);
    int ok = ecdsa_verify(ec_curve_p256(), key, 65, digest, 32, sig, 32,
                          sig + 32, 32);
    say(what, ok ? ": signed by the system's key" :
              ": NOT signed by the system's key",
        from == SIGNKEY_DISK ? " (update.key)\n" : "\n");
    return ok ? 0 : 1;
}

static int list(void)
{
    struct tapp_info t;
    int n = 0;
    for (; sys_package(n, &t) == 0; n++) {
        say(t.id, " ", 0);
        print_dec(t.version);
        say(" - ", t.name, 0);
        if (t.name_pl[0])
            say(" / ", t.name_pl, 0);
        print(", ");
        print_dec(t.size / 1024);
        say(" KiB, runs ", t.exec, t.dock ? ", in the dock\n" : "\n");
    }
    if (!n)
        print("no packages\n");
    return 0;
}

static int info(const char *path)
{
    struct source s = { -1, (int)sys_open(path, O_READ), 0, 0 };
    if (s.fd < 0) {
        say("tapp: cannot open ", path, "\n");
        return 2;
    }
    uint32_t length;
    const char *why;
    int count = listing(&s, &length, &why);
    if (count < 0) {
        say(path, ": not a package - ", why);
        print("\n");
        sys_close(s.fd);
        return 2;
    }
    /* The manifest as it is: text, a line each */
    uint32_t n = entries[0].size < CHUNK - 1 ? entries[0].size : CHUNK - 1;
    int64_t got = source_read(&s, entries[0].offset, chunk, n);
    chunk[got > 0 ? got : 0] = 0;
    print((const char *)chunk);
    for (int i = 1; i < count; i++) {
        say("  ", entries[i].name, " ");
        print_dec(entries[i].size);
        print(" B\n");
    }
    sys_close(s.fd);
    return 0;
}

/* The whole file read into memory, checked as verify checks it, and -
   only when it is signed by the system's key - handed to the kernel */
static int install(const char *path)
{
    struct source f = { -1, (int)sys_open(path, O_READ), 0, 0 };
    if (f.fd < 0) {
        say("tapp: cannot open ", path, "\n");
        return 2;
    }
    uint32_t length;
    const char *why;
    if (listing(&f, &length, &why) < 0) {
        say(path, ": not a package - ", why);
        print("\n");
        sys_close(f.fd);
        return 2;
    }
    uint8_t *buf = malloc(length);
    if (!buf || source_read(&f, 0, buf, length) != (int64_t)length) {
        say(path, ": cannot be read into memory\n", 0);
        sys_close(f.fd);
        return 2;
    }
    sys_close(f.fd);
    struct source m = { -1, -1, buf, length };
    if (verify(&m, path) != 0) {
        say(path, ": not installed - only a package signed by the "
            "system's key is\n", 0);
        return 1;
    }
    int rc = sys_package_install(buf, length);
    switch (rc) {
    case 0:
        say(path, ": installed - its programs run now\n", 0);
        return 0;
    case 1:
        say(path, ": installed - it starts with the next start of "
            "the system\n", 0);
        return 0;
    case -2:
        say("tapp: only an administrator installs packages "
            "(sudo run tapp install ", path, ")\n");
        return 1;
    case -3:
        print("tapp: this is not a system started from its disk - a "
              "package goes onto the disk the system starts from\n");
        return 1;
    case -4:
        say(path, ": not a package for this machine\n", 0);
        return 1;
    case -5:
        print("tapp: there is no room for it on the EFI partition\n");
        return 1;
    case -6:
        say(path, ": that version or a newer one is installed already\n",
            0);
        return 1;
    default:
        say(path, ": writing it failed\n", 0);
        return 1;
    }
}

void _start(int argc, char **argv)
{
    if (argc < 2 || streq(argv[1], "list"))
        sys_exit(list());
    if (streq(argv[1], "info") && argc == 3)
        sys_exit(info(argv[2]));
    if (streq(argv[1], "verify") && argc == 3) {
        struct tapp_info t;
        for (int i = 0; sys_package(i, &t) == 0; i++) {
            if (streq(t.id, argv[2])) {
                struct source s = { i, -1, 0, 0 };
                sys_exit(verify(&s, t.id));
            }
        }
        struct source s = { -1, (int)sys_open(argv[2], O_READ), 0, 0 };
        if (s.fd < 0) {
            say("tapp: no package and no file called ", argv[2], "\n");
            sys_exit(2);
        }
        int rc = verify(&s, argv[2]);
        sys_close(s.fd);
        sys_exit(rc);
    }
    if (streq(argv[1], "install") && argc == 3)
        sys_exit(install(argv[2]));
    print("usage: tapp [list] | tapp info FILE | tapp verify NAME | "
          "tapp install FILE\n");
    sys_exit(2);
}
