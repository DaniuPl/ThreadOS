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
    const char *name = argc > 1 ? argv[1] : "mapa.txt";

    uint32_t size = 0;
    const unsigned char *p = sys_mmap_file(name, &size);
    if (!p || !size) {
        print("[mmapfile] nie da sie odwzorowac pliku\n");
        sys_exit(1);
    }
    print("[mmapfile] plik pod adresem ");
    print_dec((uint32_t)p);
    print("\n[mmapfile] rozmiar ");
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
        print("[mmapfile] ZLE: bajtow nie tak, jak trzeba: ");
        print_dec(bad);
        print("\n");
        sys_exit(1);
    }

    print("[mmapfile] sprawdzone bajty zgadzaja sie, bylo ich ");
    print_dec(ile);
    print("\n");
    if (ile != size) {
        print("[mmapfile] reszty pliku nie tknalem\n");
        sys_exit(0);
    }

    /* The write is to fail. The child tries it - the parent will
       survive and tell what came of it. */
    int32_t pid = sys_fork();
    if (pid == 0) {
        unsigned char *corrupt = (unsigned char *)p;
        corrupt[0] = 0xFF;                  /* the process is to die here */
        print("[mmapfile] ZLE: zapis do odwzorowania sie udal\n");
        sys_exit(1);
    }
    if (pid > 0) {
        int code = 0;
        sys_waitany(&code);
        if (p[0] != (unsigned char)13) {
            print("[mmapfile] ZLE: tresc pliku zostala zmieniona\n");
            sys_exit(1);
        }
        print("[mmapfile] zapis do odwzorowania odrzucony\n");
    }

    print("[mmapfile] plik czytany jak pamiec, bez ani jednego read\n");
    sys_exit(0);
}
