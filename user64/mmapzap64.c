/* mmapzap64 - writing through a file mapped into memory (long mode)
 *
 * A mapping with write permission: the program changes bytes in memory
 * and they are to land in the file on disk. Not after every byte - the
 * program itself decides when, through sys_msync (or the kernel does,
 * when the process ends). Which pages to flush is known to the
 * processor: dirty is the one somebody really wrote to.
 *
 * Modes (the second argument, 'write' by default):
 *
 *   write   - checks the old pattern, writes a new one into the first
 *             two pages and flushes them explicitly. It is to flush TWO
 *             pages, not all of them: the rest of the file we only read,
 *             and reading does not make anything dirty. The second
 *             sys_msync is to return zero - once flushed, a page stops
 *             being dirty.
 *   check   - opens the file with an ORDINARY sys_read and checks that
 *             it sees the new pattern there in the first two pages, and
 *             the old one further on.
 *   nosync  - writes two bytes and finishes WITHOUT sys_msync. It is the
 *             kernel that is to flush them when the process ends.
 *   check2  - checks with an ordinary read that those two bytes are
 *             there.
 *   shared  - forks; the child writes through the mapping and the parent
 *             is to see it in its own. A writable mapping is SHARED, not
 *             copied on write - otherwise the child's writes would never
 *             reach the file.
 *
 * Usage: run mmapzap64 <file> [mode]
 */

#include "usys64.h"

#define CHANGED 8192u          /* exactly two pages */
#define CH_A    0xA5
#define CH_B    0x5A
#define OFF_B     (4096u + 7)
#define OFF_CHILD 100u

static char out[128];

static void report(const char *a, uint64_t v, const char *b)
{
    int n = 0;
    while (*a)
        out[n++] = *a++;
    char tmp[24];
    utoa(v, tmp);
    for (int i = 0; tmp[i]; i++)
        out[n++] = tmp[i];
    while (*b)
        out[n++] = *b++;
    out[n++] = '\n';
    out[n] = '\0';
    print(out);
}

static unsigned char old(uint64_t i)  { return (unsigned char)(i * 7 + 13); }
static unsigned char fresh_one(uint64_t i)   { return (unsigned char)(i * 11 + 5); }

static int equal(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static unsigned char buf[512];

/* Reads the file with an ordinary sys_read and hands back the byte at
   the given place. Returns -1 when the file does not exist or is too
   short. */
static int read_byte(const char *name, uint64_t offset)
{
    int64_t fd = sys_open(name, 0);
    if (fd < 0)
        return -1;
    uint64_t doszlo = 0;
    int result = -1;
    for (;;) {
        int64_t n = sys_read((int)fd, buf, sizeof(buf));
        if (n <= 0)
            break;
        if (offset >= doszlo && offset < doszlo + (uint64_t)n) {
            result = buf[offset - doszlo];
            break;
        }
        doszlo += (uint64_t)n;
    }
    sys_close((int)fd);
    return result;
}

/* Checks the whole file with an ordinary read against the expected
   pattern */
static uint64_t check_by_reading(const char *name)
{
    int64_t fd = sys_open(name, 0);
    if (fd < 0)
        return (uint64_t)-1;
    uint64_t doszlo = 0, bad = 0;
    for (;;) {
        int64_t n = sys_read((int)fd, buf, sizeof(buf));
        if (n <= 0)
            break;
        for (int64_t i = 0; i < n; i++) {
            uint64_t poz = doszlo + (uint64_t)i;
            unsigned char ma = poz < CHANGED ? fresh_one(poz) : old(poz);
            if (buf[i] != ma)
                bad++;
        }
        doszlo += (uint64_t)n;
    }
    sys_close((int)fd);
    /* An empty read is not a match: a loop that has not looked at a
       single byte will always count zero errors */
    if (doszlo < CHANGED)
        return (uint64_t)-1;
    return bad;
}

void _start(int argc, char **argv)
{
    const char *name = argc > 1 ? argv[1] : "zapis.txt";
    const char *mode = argc > 2 ? argv[2] : "write";

    if (equal(mode, "check")) {
        uint64_t bad = check_by_reading(name);
        if (bad) {
            report("[mmapzap64] ZLE: zwykly odczyt widzi inne bajty: ",
                    bad, "");
            sys_exit(1);
        }
        print("[mmapzap64] zwykly odczyt widzi to, co wpisala pamiec\n");
        sys_exit(0);
    }

    if (equal(mode, "check2")) {
        int a = read_byte(name, 0);
        int b = read_byte(name, OFF_B);
        if (a != CH_A || b != CH_B) {
            print("[mmapzap64] ZLE: bajty bez msync nie doszly na dysk\n");
            sys_exit(1);
        }
        print("[mmapzap64] bajty bez msync doszly na dysk"
              " przy koncu procesu\n");
        sys_exit(0);
    }

    uint64_t size = 0;
    unsigned char *p = sys_mmap_file_w(name, &size);
    if (!p || !size) {
        print("[mmapzap64] nie da sie odwzorowac pliku do zapisu\n");
        sys_exit(1);
    }

    if (equal(mode, "nosync")) {
        p[0] = CH_A;
        p[OFF_B] = CH_B;
        print("[mmapzap64] dwa bajty wpisane, koncze bez msync\n");
        sys_exit(0);
    }

    if (equal(mode, "shared")) {
        int64_t pid = sys_fork();
        if (pid == 0) {
            p[OFF_CHILD] = CH_B;
            sys_exit(0);
        }
        if (pid < 0) {
            print("[mmapzap64] ZLE: fork sie nie udal\n");
            sys_exit(1);
        }
        int code = 0;
        sys_wait(&code);
        if (p[OFF_CHILD] != CH_B) {
            print("[mmapzap64] ZLE: rodzic nie widzi zapisu dziecka\n");
            sys_exit(1);
        }
        print("[mmapzap64] rodzic widzi zapis dziecka"
              " - odwzorowanie wspolne\n");
        sys_exit(0);
    }

    /* Mode 'write': we read the whole file but write only over the two
       first pages - and that is what the flushed count is to show */
    if (size <= CHANGED) {
        print("[mmapzap64] ZLE: plik za maly na ten test\n");
        sys_exit(1);
    }
    uint64_t bad = 0;
    for (uint64_t i = 0; i < size; i++)
        if (p[i] != old(i))
            bad++;
    if (bad) {
        report("[mmapzap64] ZLE: stary wzorzec sie nie zgadza: ", bad, "");
        sys_exit(1);
    }
    report("[mmapzap64] stary wzorzec przeczytany, bajtow ", size, "");

    for (uint64_t i = 0; i < CHANGED; i++)
        p[i] = fresh_one(i);

    int64_t sent_back = sys_msync();
    report("[mmapzap64] odeslanych stron: ", (uint64_t)sent_back, "");

    int64_t drugie = sys_msync();
    report("[mmapzap64] drugie msync odeslalo: ", (uint64_t)drugie, "");

    /* Memory is still to show what we wrote - flushing to disk does not
       touch anything in it */
    if (p[0] != fresh_one(0) || p[CHANGED - 1] != fresh_one(CHANGED - 1)) {
        print("[mmapzap64] ZLE: pamiec zgubila zapisane bajty\n");
        sys_exit(1);
    }
    print("[mmapzap64] zapis przez pamiec gotowy\n");
    sys_exit(0);
}
