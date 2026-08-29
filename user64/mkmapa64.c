/* mkmapa64 - zaklada plik z wzorcem do sprawdzania odwzorowan
 *
 * Bajt o numerze i ma wartosc (i * 7 + 13) & 0xFF - ten sam wzorzec,
 * ktorego szuka mmapfile64. Plik jest wiekszy niz jedna strona, wiec
 * jego odczyt przez odwzorowanie musi sie odbyc w kilku ratach.
 *
 * Uzycie: run mkmapa64 <plik> [ile KiB] (domyslnie 12)
 */

#include "usys64.h"

#define KAWALEK 512

static unsigned char buf[KAWALEK];
static char out[96];

void _start(int argc, char **argv)
{
    const char *nazwa = argc > 1 ? argv[1] : "mapa.txt";
    uint64_t kib = 12;
    if (argc > 2) {
        int64_t v = uatoi(argv[2]);
        if (v > 0)
            kib = (uint64_t)v;
    }
    uint64_t bajtow = kib * 1024;

    int fd = (int)sys_open(nazwa, 1);
    if (fd < 0) {
        print("[mkmapa64] nie da sie zalozyc pliku\n");
        sys_exit(1);
    }

    for (uint64_t off = 0; off < bajtow; off += KAWALEK) {
        for (uint64_t i = 0; i < KAWALEK; i++)
            buf[i] = (unsigned char)((off + i) * 7 + 13);
        if (sys_write(fd, buf, KAWALEK) != KAWALEK) {
            print("[mkmapa64] zapis sie nie udal\n");
            sys_exit(1);
        }
    }
    sys_close(fd);

    int n = 0;
    const char *a = "[mkmapa64] zapisano ";
    while (*a)
        out[n++] = *a++;
    char tmp[24];
    utoa(bajtow, tmp);
    for (int i = 0; tmp[i]; i++)
        out[n++] = tmp[i];
    const char *b = " B wzorca\n";
    while (*b)
        out[n++] = *b++;
    out[n] = '\0';
    print(out);
    sys_exit(0);
}
