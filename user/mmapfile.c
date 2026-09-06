/* mmapfile - a file seen as memory
 *
 * It maps a file from disk to an address in its own space and reads it
 * like an ordinary array - without a single read call. It checks three
 * things, each a different way:
 *
 *   1. the bytes match what really lies in the file,
 *   2. with a given byte count it touches only that many - then it can
 *      be seen how many pages the kernel really read from disk,
 *   3. a write into the mapping is an error: this mapping is read-only,
 *      so an attempted write is to kill the process rather than quietly
 *      vanish. It is the CHILD that tries it, so that there is somebody
 *      left to tell how it went.
 *
 * Usage: run mmapfile <file> [how many bytes to check]
 */

#include "usys.h"

void _start(int argc, char **argv)
{
    const char *name = argc > 1 ? argv[1] : "map.txt";

    uint32_t size = 0;
    const unsigned char *p = sys_mmap_file(name, &size);
    if (!p || !size) {
        print("[mmapfile] the file cannot be mapped\n");
        sys_exit(1);
    }
    print("[mmapfile] the file at address ");
    print_dec((uint32_t)p);
    print("\n[mmapfile] size ");
    print_dec(size);
    print(" B\n");

    uint32_t ile = size;
    if (argc > 2) {
        uint32_t v = 0;
        for (const char *q = argv[2]; *q >= '0' && *q <= '9'; q++)
            v = v * 10 + (uint32_t)(*q - '0');
        if (v && v < size)
            ile = v;
    }

    uint32_t bad = 0;
    for (uint32_t i = 0; i < ile; i++)
        if (p[i] != (unsigned char)(i * 7 + 13))
            bad++;
    if (bad) {
        print("[mmapfile] WRONG: the bytes are not as they should be: ");
        print_dec(bad);
        print("\n");
        sys_exit(1);
    }

    print("[mmapfile] the checked bytes match, and there were ");
    print_dec(ile);
    print("\n");
    if (ile != size) {
        print("[mmapfile] I never touched the rest of the file\n");
        sys_exit(0);
    }

    /* The write is to fail. The child tries it - the parent will
       survive and tell what came of it. */
    int32_t pid = sys_fork();
    if (pid == 0) {
        unsigned char *corrupt = (unsigned char *)p;
        corrupt[0] = 0xFF;                  /* the process is to die here */
        print("[mmapfile] WRONG: writing to the mapping worked\n");
        sys_exit(1);
    }
    if (pid > 0) {
        int code = 0;
        sys_waitany(&code);
        if (p[0] != (unsigned char)13) {
            print("[mmapfile] WRONG: the file's content was changed\n");
            sys_exit(1);
        }
        print("[mmapfile] writing to the mapping was refused\n");
    }

    print("[mmapfile] the file read like memory, without a single read\n");
    sys_exit(0);
}
