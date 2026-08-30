/* mkmap - creates a file with a pattern for checking mappings
 *
 * The byte with number i has the value (i * 7 + 13) & 0xFF - the same
 * pattern mmapfile looks for. The file is larger than a single page, so
 * reading it through a mapping has to happen in several instalments.
 *
 * Usage: run mkmap <file> [how many KiB] (12 by default)
 */

#include "usys.h"

#define CHUNK 512

static unsigned char buf[CHUNK];

void _start(int argc, char **argv)
{
    const char *name = argc > 1 ? argv[1] : "mapa.txt";
    uint32_t kib = 12;
    if (argc > 2) {
        kib = 0;
        for (const char *p = argv[2]; *p >= '0' && *p <= '9'; p++)
            kib = kib * 10 + (uint32_t)(*p - '0');
        if (!kib)
            kib = 12;
    }
    uint32_t nbytes = kib * 1024;

    int32_t fd = sys_open(name, O_WRITE | O_CREATE | O_TRUNC);
    if (fd < 0) {
        print("[mkmap] nie da sie zalozyc pliku\n");
        sys_exit(1);
    }

    for (uint32_t off = 0; off < nbytes; off += CHUNK) {
        for (uint32_t i = 0; i < CHUNK; i++)
            buf[i] = (unsigned char)((off + i) * 7 + 13);
        if (sys_write(fd, buf, CHUNK) != (int32_t)CHUNK) {
            print("[mkmap] zapis sie nie udal\n");
            sys_exit(1);
        }
    }
    sys_close(fd);

    print("[mkmap] zapisano ");
    print_dec(nbytes);
    print(" B wzorca\n");
    sys_exit(0);
}
