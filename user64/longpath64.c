/* longpath64 - paths longer than 63 bytes, from ring 3
 *
 * Every path a program handed the kernel used to be cut off at 63 bytes
 * in all - not each name, the whole path - so a file three directories
 * down with ordinary names could not be opened by its path at all. And
 * the working directory's path was put together in a kernel buffer of
 * 252 bytes with nothing to stop a deep enough chain of names running
 * past it.
 *
 * This program goes through usys64.h alone, with no C library between
 * it and the kernel: directories nested eight deep under names of 40
 * bytes, made, entered, listed, a file in the deepest one written by
 * its whole path (over 300 bytes), renamed by whole paths, read back,
 * removed. Then a chain built step by step - each name short enough
 * for any kernel - until the working directory's path is over 400
 * bytes, and getcwd asked for it.
 *
 * Every check prints a line; anything wrong ends the program with
 * code 1 and an ERROR line.
 */

#include "usys64.h"

#define DEPTH 8
#define NAME  40

static char path[1024], other[1024], buf[512], line[640];
static struct dirent2_64 ent;
static struct stat64k st;

static int put(int n, const char *s)
{
    while (*s && n < (int)sizeof(line) - 2)
        line[n++] = *s++;
    return n;
}

static int putn(int n, int64_t v)
{
    char tmp[24];
    if (v < 0) {
        n = put(n, "-");
        v = -v;
    }
    utoa((uint64_t)v, tmp);
    return put(n, tmp);
}

static void say(const char *what, int64_t v)
{
    int n = put(0, "[longpath64] ");
    n = put(n, what);
    n = put(n, " ");
    n = putn(n, v);
    line[n++] = '\n';
    line[n] = 0;
    print(line);
}

static void fail(const char *what, int64_t v)
{
    int n = put(0, "[longpath64] ERROR: ");
    n = put(n, what);
    n = put(n, " -> ");
    n = putn(n, v);
    line[n++] = '\n';
    line[n] = 0;
    print(line);
    sys_exit(1);
}

static uint64_t slen(const char *s)
{
    return ustrlen(s);
}

/* path := the first k directories of the chain, each NAME bytes:
   "aaaa...a1/bbbb...b2/..." */
static void chain(int k)
{
    uint64_t n = 0;
    for (int d = 0; d < k; d++) {
        if (d)
            path[n++] = '/';
        for (int i = 0; i < NAME - 1; i++)
            path[n++] = (char)('a' + d);
        path[n++] = (char)('1' + d);
    }
    path[n] = 0;
}

static void append(char *p, const char *tail)
{
    uint64_t n = slen(p);
    while (*tail)
        p[n++] = *tail++;
    p[n] = 0;
}

static int same(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static const char BODY[] = "written by a path of more than three hundred bytes";

void _start(void)
{
    /* --- the chain, made by whole paths ----------------------------- */
    for (int k = 1; k <= DEPTH; k++) {
        chain(k);
        int64_t r = sys_mkdir(path);
        if (r < 0)
            fail("mkdir by a whole path of bytes", (int64_t)slen(path));
    }
    chain(DEPTH);
    say("made a chain of directories, the deepest by a path of bytes",
        (int64_t)slen(path));

    /* a file in the deepest one, by its whole path */
    append(path, "/the-file.txt");
    int64_t fd = sys_open(path, 1);
    if (fd < 0)
        fail("open (create) by a path of bytes", (int64_t)slen(path));
    if (sys_write((int)fd, BODY, slen(BODY)) != (int64_t)slen(BODY))
        fail("write", fd);
    sys_close((int)fd);
    say("wrote a file by a path of bytes", (int64_t)slen(path));

    /* what is there, asked by name: a file of that size */
    if (sys_stat(path, &st) < 0 || st.type != DT2_FILE ||
        st.size != slen(BODY))
        fail("stat of the file, its size", (int64_t)st.size);
    say("stat by the whole path gives the size", (int64_t)st.size);

    /* renamed to another whole path, one level up */
    chain(DEPTH - 1);
    for (int i = 0; i <= (int)slen(path); i++)
        other[i] = path[i];
    append(other, "/renamed-file.txt");
    chain(DEPTH);
    append(path, "/the-file.txt");
    if (sys_rename(path, other) < 0)
        fail("rename between whole paths", (int64_t)slen(other));
    if (sys_open(path, 0) >= 0)
        fail("the old name still opens", 0);
    fd = sys_open(other, 0);
    if (fd < 0)
        fail("the new name does not open", fd);
    int64_t n = sys_read((int)fd, buf, sizeof(buf) - 1);
    sys_close((int)fd);
    buf[n > 0 ? n : 0] = 0;
    if (!same(buf, BODY))
        fail("the renamed file holds something else", n);
    say("renamed by whole paths and read back, bytes", n);

    /* listed by its path, without going there: the renamed file and
       the deepest directory, nothing else */
    chain(DEPTH - 1);
    uint64_t cursor = 0;
    int files = 0, dirs = 0, k;
    while ((k = (int)sys_readdir2(path, &cursor, &ent)) == 1) {
        if (ent.type == DT2_DIR)
            dirs++;
        else if (same(ent.name, "renamed-file.txt") &&
                 ent.size == slen(BODY))
            files++;
        else
            fail("an entry that should not be there", ent.name_len);
    }
    if (k != 0 || files != 1 || dirs != 1)
        fail("listing by the whole path", files * 10 + dirs);
    say("listed a directory by a path of bytes", (int64_t)slen(path));

    /* into the deepest one by its whole path: getcwd says where */
    chain(DEPTH);
    if (sys_chdir(path) < 0)
        fail("chdir by a whole path", (int64_t)slen(path));
    if (sys_getcwd(buf, sizeof(buf)) < 0 || buf[0] != '/' ||
        !same(buf + 1, path))
        fail("getcwd after chdir", (int64_t)slen(buf));
    say("chdir by the whole path, and getcwd says it, bytes",
        (int64_t)slen(buf));
    sys_chdir("/");

    /* everything removed again by whole paths, deepest first */
    if (sys_unlink(other) < 0)
        fail("unlink by a whole path", (int64_t)slen(other));
    for (int d = DEPTH; d >= 1; d--) {
        chain(d);
        if (sys_unlink(path) < 0)
            fail("removing a directory by a whole path", d);
    }
    if (sys_stat("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa1", &st) == 0)
        fail("the chain is still there", 0);
    say("removed it all by whole paths, directories", DEPTH);

    /* --- a chain made one step at a time --------------------------- *
     * Each name 60 bytes, each mkdir and chdir relative: any kernel
     * takes these. Seven deep is 427 bytes of path. */
    static char name[64];
    for (int i = 0; i < 59; i++)
        name[i] = 'x';
    name[60] = 0;
    for (int d = 0; d < 7; d++) {
        name[59] = (char)('1' + d);
        if (sys_mkdir(name) < 0 || sys_chdir(name) < 0)
            fail("a step down", d);
    }
    int64_t got = sys_getcwd(buf, sizeof(buf));
    uint64_t want = 7 * 61;
    if (got < 0 || slen(buf) != want || buf[0] != '/' ||
        buf[want - 1] != '7')
        fail("getcwd of a deep working directory", got);
    say("getcwd of a working directory reached step by step, bytes",
        (int64_t)slen(buf));
    /* and back up, removing as it goes */
    for (int d = 6; d >= 0; d--) {
        name[59] = (char)('1' + d);
        if (sys_chdir("..") < 0 || sys_unlink(name) < 0)
            fail("a step up", d);
    }
    print("[longpath64] all done\n");
    sys_exit(0);
}
