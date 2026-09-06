/* mmapfile64 - a file seen as memory
 *
 * It maps a file from disk to an address in its own space and reads it
 * like an ordinary array - without a single read call. It checks three
 * things, each a different way:
 *
 *   1. the bytes match what really lies in the file,
 *   2. reading the far end of the file works the same as the beginning
 *      (the pages arrive under your fingers, not all at once),
 *   3. a write into the mapping is an error - this is a read-only
 *      mapping, so an attempted write is to kill the process rather
 *      than quietly vanish. It is the CHILD that tries it, so that
 *      there is somebody left to tell how it went.
 *
 * Usage: run mmapfile64 <file> [how many bytes to check]
 *
 * The second argument serves to show that the pages really do arrive
 * under your fingers: with "1" the program touches a single byte and
 * finishes, and the kernel reports how many pages it had to read from
 * disk. For a hundred-page file that is to be one.
 */

#include "usys64.h"

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

void _start(int argc, char **argv)
{
    const char *name = argc > 1 ? argv[1] : "map.txt";

    uint64_t size = 0;
    const unsigned char *p = sys_mmap_file(name, &size);
    if (!p) {
        print("[mmapfile64] the file cannot be mapped\n");
        sys_exit(1);
    }
    report("[mmapfile64] the file at address ", (uint64_t)(uintptr_t)p, "");
    report("[mmapfile64] size ", size, " B");

    /* How many bytes to check. The whole file by default - but with an
       explicitly given number we touch only that many, and then it can
       be seen how many pages the kernel really read. */
    uint64_t ile = size;
    if (argc > 2) {
        int64_t v = uatoi(argv[2]);
        if (v > 0 && (uint64_t)v < size)
            ile = (uint64_t)v;
    }

    /* The file's pattern: the byte with number i has the value
       (i * 7 + 13) & 0xFF. mkmap64 wrote it, before the file reached the
       disk. */
    uint64_t bad = 0;
    for (uint64_t i = 0; i < ile; i++)
        if (p[i] != (unsigned char)(i * 7 + 13))
            bad++;

    if (bad) {
        report("[mmapfile64] WRONG: the bytes are not as they should be: ", bad, "");
        sys_exit(1);
    }
    report("[mmapfile64] the checked bytes match, and there were ",
            ile, "");
    if (ile != size) {
        print("[mmapfile64] I never touched the rest of the file\n");
        sys_exit(0);
    }

    /* The write: it is to fail. The child tries it - the parent will
       survive and tell what came of it. */
    int64_t pid = sys_fork();
    if (pid == 0) {
        unsigned char *corrupt = (unsigned char *)p;
        corrupt[0] = 0xFF;                  /* the process is to die here */
        print("[mmapfile64] WRONG: writing to the mapping worked\n");
        sys_exit(1);
    }
    if (pid > 0) {
        int code = 0;
        sys_wait(&code);
        if (p[0] != (unsigned char)13) {
            print("[mmapfile64] WRONG: the file's content was changed\n");
            sys_exit(1);
        }
        print("[mmapfile64] writing to the mapping was refused\n");
    }

    print("[mmapfile64] the file read like memory, without a single read\n");
    sys_exit(0);
}
