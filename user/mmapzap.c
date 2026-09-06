/* mmapzap - writing through a file mapped into memory
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
 * Usage: run mmapzap <file> [mode]
 */

#include "usys.h"

#define CHANGED 8192u          /* exactly two pages */
#define CH_A    0xA5
#define CH_B    0x5A
#define OFF_B     (4096u + 7)
#define OFF_CHILD 100u

static unsigned char old(uint32_t i)  { return (unsigned char)(i * 7 + 13); }
static unsigned char fresh_one(uint32_t i)   { return (unsigned char)(i * 11 + 5); }

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
static int read_byte(const char *name, uint32_t offset)
{
    int32_t fd = sys_open(name, O_READ);
    if (fd < 0)
        return -1;
    uint32_t doszlo = 0;
    int result = -1;
    for (;;) {
        int32_t n = sys_read(fd, buf, sizeof(buf));
        if (n <= 0)
            break;
        if (offset >= doszlo && offset < doszlo + (uint32_t)n) {
            result = buf[offset - doszlo];
            break;
        }
        doszlo += (uint32_t)n;
    }
    sys_close(fd);
    return result;
}

/* Checks the whole file with an ordinary read against the expected
   pattern */
static uint32_t check_by_reading(const char *name)
{
    int32_t fd = sys_open(name, O_READ);
    if (fd < 0)
        return 0xFFFFFFFFu;
    uint32_t doszlo = 0, bad = 0;
    for (;;) {
        int32_t n = sys_read(fd, buf, sizeof(buf));
        if (n <= 0)
            break;
        for (int32_t i = 0; i < n; i++) {
            uint32_t poz = doszlo + (uint32_t)i;
            unsigned char ma = poz < CHANGED ? fresh_one(poz) : old(poz);
            if (buf[i] != ma)
                bad++;
        }
        doszlo += (uint32_t)n;
    }
    sys_close(fd);
    /* An empty read is not a match: a loop that has not looked at a
       single byte will always count zero errors */
    if (doszlo < CHANGED)
        return 0xFFFFFFFFu;
    return bad;
}

void _start(int argc, char **argv)
{
    const char *name = argc > 1 ? argv[1] : "written.txt";
    const char *mode = argc > 2 ? argv[2] : "write";

    if (equal(mode, "check")) {
        uint32_t bad = check_by_reading(name);
        if (bad) {
            print("[mmapzap] WRONG: an ordinary read sees other bytes: ");
            print_dec(bad);
            print("\n");
            sys_exit(1);
        }
        print("[mmapzap] an ordinary read sees what the memory wrote\n");
        sys_exit(0);
    }

    if (equal(mode, "check2")) {
        int a = read_byte(name, 0);
        int b = read_byte(name, OFF_B);
        if (a != CH_A || b != CH_B) {
            print("[mmapzap] WRONG: bytes without msync never reached the disk\n");
            sys_exit(1);
        }
        print("[mmapzap] bytes without msync reached the disk as the process ended\n");
        sys_exit(0);
    }

    uint32_t size = 0;
    unsigned char *p = sys_mmap_file_w(name, &size);
    if (!p || !size) {
        print("[mmapzap] the file cannot be mapped for writing\n");
        sys_exit(1);
    }

    if (equal(mode, "nosync")) {
        p[0] = CH_A;
        p[OFF_B] = CH_B;
        print("[mmapzap] two bytes written, ending with no msync\n");
        sys_exit(0);
    }

    if (equal(mode, "shared")) {
        int32_t pid = sys_fork();
        if (pid == 0) {
            p[OFF_CHILD] = CH_B;
            sys_exit(0);
        }
        if (pid < 0) {
            print("[mmapzap] WRONG: the fork did not work\n");
            sys_exit(1);
        }
        int code = 0;
        sys_waitany(&code);
        if (p[OFF_CHILD] != CH_B) {
            print("[mmapzap] WRONG: the parent does not see the child's write\n");
            sys_exit(1);
        }
        print("[mmapzap] the parent sees the child's write - the mapping is shared\n");
        sys_exit(0);
    }

    /* Mode 'write': we read the whole file but write only over the two
       first pages - and that is what the flushed count is to show */
    if (size <= CHANGED) {
        print("[mmapzap] WRONG: the file is too small for this test\n");
        sys_exit(1);
    }
    uint32_t bad = 0;
    for (uint32_t i = 0; i < size; i++)
        if (p[i] != old(i))
            bad++;
    if (bad) {
        print("[mmapzap] WRONG: the old pattern does not match: ");
        print_dec(bad);
        print("\n");
        sys_exit(1);
    }
    print("[mmapzap] the old pattern was read, bytes ");
    print_dec(size);
    print("\n");

    for (uint32_t i = 0; i < CHANGED; i++)
        p[i] = fresh_one(i);

    int sent_back = sys_msync();
    print("[mmapzap] pages sent back: ");
    print_dec((uint32_t)sent_back);
    print("\n");

    int drugie = sys_msync();
    print("[mmapzap] the second msync sent back: ");
    print_dec((uint32_t)drugie);
    print("\n");

    /* Memory is still to show what we wrote - flushing to disk does not
       touch anything in it */
    if (p[0] != fresh_one(0) || p[CHANGED - 1] != fresh_one(CHANGED - 1)) {
        print("[mmapzap] WRONG: the memory lost the written bytes\n");
        sys_exit(1);
    }
    print("[mmapzap] writing through memory is done\n");
    sys_exit(0);
}
