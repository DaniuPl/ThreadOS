/* mkmap64 - creates a file with a pattern for checking mappings
 *
 * The byte with number i has the value (i * 7 + 13) & 0xFF - the same
 * pattern mmapfile64 looks for. The file is larger than a single page,
 * so reading it through a mapping has to happen in several
 * instalments.
 *
 * Usage: run mkmap64 <file> [how many KiB] (12 by default)
 */

#include "usys64.h"

#define CHUNK 512

static unsigned char buf[CHUNK];
static char out[96];

void _start(int argc, char **argv)
{
    const char *name = argc > 1 ? argv[1] : "mapa.txt";
    uint64_t kib = 12;
    if (argc > 2) {
        int64_t v = uatoi(argv[2]);
        if (v > 0)
            kib = (uint64_t)v;
    }
    uint64_t nbytes = kib * 1024;

    int fd = (int)sys_open(name, 1);
    if (fd < 0) {
        print("[mkmap64] nie da sie zalozyc pliku\n");
        sys_exit(1);
    }

    for (uint64_t off = 0; off < nbytes; off += CHUNK) {
        for (uint64_t i = 0; i < CHUNK; i++)
            buf[i] = (unsigned char)((off + i) * 7 + 13);
        if (sys_write(fd, buf, CHUNK) != CHUNK) {
            print("[mkmap64] zapis sie nie udal\n");
            sys_exit(1);
        }
    }
    sys_close(fd);

    int n = 0;
    const char *a = "[mkmap64] zapisano ";
    while (*a)
        out[n++] = *a++;
    char tmp[24];
    utoa(nbytes, tmp);
    for (int i = 0; tmp[i]; i++)
        out[n++] = tmp[i];
    const char *b = " B wzorca\n";
    while (*b)
        out[n++] = *b++;
    out[n] = '\0';
    print(out);
    sys_exit(0);
}
