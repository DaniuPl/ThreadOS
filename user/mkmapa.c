/* mkmapa - zaklada plik z wzorcem do sprawdzania odwzorowan
 *
 * Bajt o numerze i ma wartosc (i * 7 + 13) & 0xFF - ten sam wzorzec,
 * ktorego szuka mmapfile. Plik jest wiekszy niz jedna strona, wiec
 * jego odczyt przez odwzorowanie musi odbyc sie w kilku ratach.
 *
 * Uzycie: run mkmapa <plik> [ile KiB] (domyslnie 12)
 */

#include "usys.h"

#define KAWALEK 512

static unsigned char buf[KAWALEK];

void _start(int argc, char **argv)
{
    const char *nazwa = argc > 1 ? argv[1] : "mapa.txt";
    uint32_t kib = 12;
    if (argc > 2) {
        kib = 0;
        for (const char *p = argv[2]; *p >= '0' && *p <= '9'; p++)
            kib = kib * 10 + (uint32_t)(*p - '0');
        if (!kib)
            kib = 12;
    }
    uint32_t bajtow = kib * 1024;

    int32_t fd = sys_open(nazwa, O_WRITE | O_CREATE | O_TRUNC);
    if (fd < 0) {
        print("[mkmapa] nie da sie zalozyc pliku\n");
        sys_exit(1);
    }

    for (uint32_t off = 0; off < bajtow; off += KAWALEK) {
        for (uint32_t i = 0; i < KAWALEK; i++)
            buf[i] = (unsigned char)((off + i) * 7 + 13);
        if (sys_write(fd, buf, KAWALEK) != (int32_t)KAWALEK) {
            print("[mkmapa] zapis sie nie udal\n");
            sys_exit(1);
        }
    }
    sys_close(fd);

    print("[mkmapa] zapisano ");
    print_dec(bajtow);
    print(" B wzorca\n");
    sys_exit(0);
}
