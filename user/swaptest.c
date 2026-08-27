/* swaptest - wymiana stron na dysk
 *
 * Bierze wiecej pamieci, niz maszyna ma RAM-u, i zapisuje w niej wzorzec
 * zalezny od numeru strony. Jadro musi po drodze wyrzucic czesc stron
 * na dysk. Potem program czyta wszystko z powrotem: kazda strona, ktora
 * poszla na dysk, wraca przy pierwszym dotknieciu - i musi wrocic
 * z dokladnie ta sama zawartoscia.
 *
 * Uzycie: run swaptest [MiB] (domyslnie 14).
 */

#include "usys.h"

#define PAGE 4096

static unsigned char stamp(uint32_t page)
{
    return (unsigned char)(page * 7 + 13);
}

void _start(int argc, char **argv)
{
    uint32_t mib = 14;
    if (argc > 1) {
        mib = 0;
        for (const char *p = argv[1]; *p >= '0' && *p <= '9'; p++)
            mib = mib * 10 + (uint32_t)(*p - '0');
        if (!mib)
            mib = 14;
    }
    uint32_t bytes = mib * 1024u * 1024u;
    uint32_t pages = bytes / PAGE;

    unsigned char *p = sys_sbrk((int32_t)bytes);
    if (p == (void *)-1) {
        print("[swaptest] sbrk odmowil\n");
        sys_exit(1);
    }
    p = (unsigned char *)(((uint32_t)p + PAGE - 1) & ~(uint32_t)(PAGE - 1));

    print("[swaptest] zapisuje ");
    print_dec(mib);
    print(" MiB\n");
    for (uint32_t i = 0; i < pages; i++)
        p[i * PAGE] = stamp(i);

    print("[swaptest] czytam z powrotem\n");
    uint32_t bad = 0;
    for (uint32_t i = 0; i < pages; i++)
        if (p[i * PAGE] != stamp(i))
            bad++;

    if (bad) {
        print("[swaptest] ZLE: stron z bledna zawartoscia ");
        print_dec(bad);
        print("\n");
        sys_exit(1);
    }
    print("[swaptest] wszystkie ");
    print_dec(pages);
    print(" stron wrocilo w calosci\n");
    sys_exit(0);
}
