/* update - a newer ThreadOS, straight from the internet
 *
 *   run update            install the newest release, if it is newer
 *   run update check      only say whether there is one
 *   run update <url>      ...from another place than GitHub (ends in /)
 *   run update check <url>
 *   run update rollback   restore the previous system files
 *
 * A release is four files next to each other: the kernel, the initrd,
 * the loader, and update.txt saying which version they are, how long
 * each one is and what its SHA-256 is - and, on its last line, a
 * signature over all of that. The signature is ECDSA P-256 by a key
 * whose public half is built into this program (and the release
 * workflow holds the private half, .github/workflows/release.yml). So a
 * file swapped on the way, or on the server itself, is refused: the
 * hash in update.txt would not match it, and update.txt cannot be
 * changed without the key. HTTPS on top of that is the channel's
 * privacy, not its trust.
 *
 * Writing is the kernel's (SYS64_UPDATE_WRITE): the three files go
 * over the installed ones on the EFI partition. Before the first write,
 * the kernel keeps one previous system there; 'update rollback' restores
 * it. Only an admin may update or roll back the system.
 *
 * Where a release comes from:
 *   https://github.com/DaniuPl/ThreadOS/releases/latest/download/
 * GitHub answers that with a redirect to the file itself.
 *
 * update.key on the disk (a system file: only an admin writes it), if
 * it is there, holds the public key in hex instead of the built-in one
 * - for a machine that takes its updates from somebody else's builds,
 * and for the tests, which sign with a key of their own.
 */

#include "uapi.h"
#include "uhttp.h"
#include "ucert.h"
#include "usha256.h"
#include "uecdsa.h"

#define DEFAULT_URL "https://github.com/DaniuPl/ThreadOS/releases/latest/download/"
#define MANIFEST_MAX 4096
#define URL_MAX 256

/* The public half of the key that signs releases (P-256, 0x04 || X || Y) */
static const uint8_t official_key[65] = {
    0x04, 0x40, 0x6f, 0xcb, 0x12, 0x6f, 0x92, 0xdc, 0xe9, 0x07, 0x05, 0xde,
    0x16, 0x84, 0x7e, 0xc4, 0x48, 0x34, 0x31, 0xf3, 0x1e, 0x9f, 0xd2, 0x6d,
    0x48, 0xac, 0x16, 0x85, 0x09, 0x5a, 0xdb, 0x86, 0xf1, 0x18, 0xc1, 0x24,
    0xba, 0x54, 0x0e, 0x70, 0xf2, 0xb3, 0x1f, 0x77, 0xcc, 0xdd, 0x9a, 0x02,
    0xef, 0xaa, 0x08, 0xfd, 0x1a, 0xc2, 0xf4, 0xcf, 0x4b, 0xaf, 0x38, 0xb6,
    0x88, 0x32, 0x93, 0x65, 0x0e
};

/* What update.txt says, and in the kernel's order: kernel, initrd,
   loader (SYS64_UPDATE_WRITE's numbers) */
#define FILES 3
static const char *const kinds[FILES] = { "kernel", "initrd", "loader" };

struct release {
    uint32_t version;
    char name[FILES][32];
    uint32_t size[FILES];
    uint8_t hash[FILES][32];
    uint8_t sig[64];
    uint32_t signed_len;            /* the bytes the signature covers */
};

static void say(const char *s) { print("[update] "); print(s); }

static void fail(const char *s)
{
    say(s);
    print("\n");
    sys_exit(1);
}

/* --- fetching ------------------------------------------------------ */

struct sink {
    uint8_t *buf;
    uint32_t cap, len;
    int overflow;
};

static void to_buffer(const char *data, uint32_t len, void *arg)
{
    struct sink *s = arg;
    if (s->len + len > s->cap) {
        s->overflow = 1;
        return;
    }
    for (uint32_t i = 0; i < len; i++)
        s->buf[s->len + i] = (uint8_t)data[i];
    s->len += len;
}

/* One file, following redirects (GitHub sends each download on to
   where it really lies). 1 = the whole body is in the sink. */
static int fetch(const char *url, struct sink *s)
{
    static char host[128], path[2048];
    uint32_t port;
    int tls;
    if (!http_parse_url(url, host, sizeof(host), &port, path, sizeof(path),
                        &tls))
        return 0;
    struct http_result res;
    for (int hop = 0; hop < 5; hop++) {
        uint32_t ip = http_resolve(host);
        if (!ip) {
            say("I do not know the address of ");
            print(host);
            print("\n");
            return 0;
        }
        s->len = 0;
        s->overflow = 0;
        if (http_get(ip, port, host, path, tls, to_buffer, s, &res) < 0) {
            say("could not connect to ");
            print(host);
            if (tls && res.verify_err) {
                print(": ");
                print(cert_error_text(res.verify_err));
            }
            print("\n");
            return 0;
        }
        if ((res.status == 301 || res.status == 302 || res.status == 303 ||
             res.status == 307 || res.status == 308) && res.location[0]) {
            if (!http_apply_location(res.location, host, sizeof(host), &port,
                                     path, sizeof(path), &tls))
                return 0;
            continue;
        }
        if (res.status != 200) {
            say("the server answered ");
            print_dec((uint32_t)res.status);
            print("\n");
            return 0;
        }
        return !s->overflow;
    }
    say("too many redirects\n");
    return 0;
}

/* --- reading update.txt -------------------------------------------- */

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int unhex(const char *s, uint8_t *out, int n)
{
    for (int i = 0; i < n; i++) {
        int a = hexval(s[2 * i]), b = hexval(s[2 * i + 1]);
        if (a < 0 || b < 0)
            return 0;
        out[i] = (uint8_t)(a * 16 + b);
    }
    return 1;
}

static int starts(const char *s, const char *w)
{
    while (*w)
        if (*s++ != *w++)
            return 0;
    return 1;
}

static const char *word(const char *s, char *out, int max)
{
    while (*s == ' ')
        s++;
    int n = 0;
    while (*s && *s != ' ' && *s != '\n' && *s != '\r' && n < max - 1)
        out[n++] = *s++;
    out[n] = '\0';
    return s;
}

static uint32_t number(const char *s)
{
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (uint32_t)(*s++ - '0');
    return v;
}

/* 1 = every line was there and made sense */
static int parse(const char *t, uint32_t len, struct release *r)
{
    int have = 0;
    uint32_t at = 0;
    if (!starts(t, "ThreadOS update\n"))
        return 0;
    while (at < len) {
        const char *line = t + at;
        uint32_t end = at;
        while (end < len && t[end] != '\n')
            end++;
        char w[80];
        if (starts(line, "version ")) {
            r->version = number(line + 8);
            have |= 1;
        } else if (starts(line, "signature ")) {
            if (end - at < 10 + 128 || !unhex(line + 10, r->sig, 64))
                return 0;
            r->signed_len = at;      /* everything before this line */
            have |= 2;
            break;
        } else {
            const char *p = word(line, w, sizeof(w));
            for (int k = 0; k < FILES; k++) {
                if (!ustreq(w, kinds[k]))
                    continue;
                p = word(p, r->name[k], sizeof(r->name[k]));
                p = word(p, w, sizeof(w));
                r->size[k] = number(w);
                p = word(p, w, sizeof(w));
                if (ustrlen(w) != 64 || !unhex(w, r->hash[k], 32) ||
                    !r->size[k] || !r->name[k][0])
                    return 0;
                have |= 4 << k;
            }
        }
        at = end + 1;
    }
    return have == (1 | 2 | 4 | 8 | 16);
}

/* The key: update.key from the disk if an admin put one there, else
   the one this program was built with */
static const uint8_t *signing_key(void)
{
    static uint8_t key[65];
    static char text[140];
    int fd = sys_open("update.key", O_READ);
    if (fd < 0)
        return official_key;
    int64_t n = sys_read(fd, text, sizeof(text) - 1);
    sys_close(fd);
    if (n >= 130 && unhex(text, key, 65) && key[0] == 0x04) {
        say("checking signatures against update.key\n");
        return key;
    }
    say("update.key is not a key - ignoring it\n");
    return official_key;
}

static int initrd_text(const char *name, char *out, uint32_t cap)
{
    if (cap < 2)
        return 0;
    int64_t n = sys_initrd_read(name, out, cap - 1);
    if (n <= 0)
        return 0;
    out[n] = '\0';
    for (int64_t i = 0; i < n; i++)
        if (out[i] == '\n' || out[i] == '\r') {
            out[i] = '\0';
            break;
        }
    return 1;
}

static uint32_t local_version(void)
{
    char v[32];
    if (initrd_text("version-code", v, sizeof(v)))
        return number(v);
    if (initrd_text("version", v, sizeof(v)))
        return number(v);       /* images made before version-code */
    return 0;
}

static void local_version_label(char *out, uint32_t cap)
{
    if (!initrd_text("version", out, cap))
        out[0] = '\0';
}

void _start(int argc, char **argv)
{
    int only_check = 0;
    char base[URL_MAX];
    const char *url = DEFAULT_URL;

    if (argc > 1 && ustreq(argv[1], "rollback")) {
        if (argc != 2)
            fail("usage: update rollback");
        int64_t rc = sys_update_rollback();
        if (rc == -2)
            fail("only an admin may restore the system: 'sudo run update rollback'");
        if (rc == -3)
            fail("there is no installed ThreadOS on this disk");
        if (rc == -4)
            fail("rollback can only run after booting from the installed system");
        if (rc == -5)
            fail("there is no complete previous-system backup");
        if (rc < 0)
            fail("restoring the previous system failed; retry rollback");
        say("the previous system was restored; restart to use it\n");
        sys_exit(0);
    }

    for (int i = 1; i < argc; i++) {
        if (ustreq(argv[i], "check"))
            only_check = 1;
        else
            url = argv[i];
    }
    uint64_t ul = ustrlen(url);
    if (ul + 32 > URL_MAX)
        fail("that address is too long");
    for (uint64_t i = 0; i <= ul; i++)
        base[i] = url[i];
    if (base[ul - 1] != '/') {
        base[ul] = '/';
        base[ul + 1] = '\0';
    }

    static uint8_t roots[16384];
    uint32_t roots_len = cert_load_roots("roots.der", roots, sizeof(roots));
    http_set_roots(roots, roots_len, 1);

    uint32_t mine = local_version();
    char label[32];
    local_version_label(label, sizeof(label));
    say("this system is version ");
    print(label[0] ? label : "unknown");
    print(" (update sequence ");
    print_dec(mine);
    print(")");
    print("\n");

    /* --- what the newest release is ---------------------------------- */
    static uint8_t text[MANIFEST_MAX + 1];
    struct sink s = { text, MANIFEST_MAX, 0, 0 };
    char where[URL_MAX + 32];
    int n = 0;
    for (int i = 0; base[i]; i++)
        where[n++] = base[i];
    const char *m = "update.txt";
    while (*m)
        where[n++] = *m++;
    where[n] = '\0';
    say("asking ");
    print(where);
    print("\n");
    if (!fetch(where, &s))
        fail("no word about a release");
    text[s.len] = '\0';

    static struct release r;
    if (!parse((const char *)text, s.len, &r))
        fail("update.txt does not say what a release says - not touching it");

    uint8_t digest[32];
    sha256(text, r.signed_len, digest);
    if (!ecdsa_verify(ec_curve_p256(), signing_key(), 65, digest, 32,
                      r.sig, 32, r.sig + 32, 32))
        fail("the release is NOT signed by the right key - refusing it");
    say("the release is signed by the right key\n");

    say("the newest release update sequence is ");
    print_dec(r.version);
    print("\n");
    if (r.version <= mine) {
        say("this system is up to date\n");
        sys_exit(0);
    }
    if (only_check) {
        say("a newer system is available: 'sudo run update' installs it\n");
        sys_exit(0);
    }

    /* --- the files, each checked against its hash -------------------- */
    uint8_t *file[FILES];
    for (int k = 0; k < FILES; k++) {
        file[k] = sys_sbrk((int64_t)r.size[k] + 4096);
        if (!file[k])
            fail("not enough memory for the release");
        struct sink fs = { file[k], r.size[k], 0, 0 };
        n = 0;
        for (int i = 0; base[i]; i++)
            where[n++] = base[i];
        for (int i = 0; r.name[k][i]; i++)
            where[n++] = r.name[k][i];
        where[n] = '\0';
        say("downloading ");
        print(r.name[k]);
        print(" (");
        print_dec(r.size[k]);
        print(" B)\n");
        if (!fetch(where, &fs) || fs.len != r.size[k])
            fail("the download did not finish - nothing was changed");
        sha256(file[k], fs.len, digest);
        for (int i = 0; i < 32; i++)
            if (digest[i] != r.hash[k][i])
                fail("a file does not match its hash - nothing was changed");
    }
    say("all three files match the signed hashes\n");

    /* --- and onto the disk ------------------------------------------- */
    static const int order[FILES] = { 1, 0, 2 };   /* initrd, kernel, loader */
    for (int i = 0; i < FILES; i++) {
        int k = order[i];
        int64_t rc = sys_update_write(k, file[k], r.size[k]);
        if (rc == -2)
            fail("only an admin may replace the system: 'sudo run update'");
        if (rc == -3)
            fail("there is no installed ThreadOS on this disk to update");
        if (rc == -4)
            fail("updates can only be installed after booting "
                 "from an installed system");
        if (rc == -5)
            fail("the previous system could not be backed up; nothing was updated");
        if (rc == -6)
            fail("an earlier update is incomplete; use 'sudo run update rollback' first");
        if (rc < 0)
            fail("writing the new system failed");
        say("written: ");
        print(r.name[k]);
        print("\n");
    }
    say("ThreadOS updated to release sequence ");
    print_dec(r.version);
    print(" - restart to start it\n");
    say("the previous system is saved; 'sudo run update rollback' restores it\n");
    sys_exit(0);
}
