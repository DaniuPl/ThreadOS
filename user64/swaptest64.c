/* swaptest64 - wymiana stron na dysk widziana od strony programu
 *
 * Bierze wiecej pamieci, niz maszyna ma RAM-u, i zapisuje w niej wzorzec
 * zalezny od numeru strony. Jadro musi po drodze wyrzucic czesc stron
 * na dysk. Potem program czyta wszystko z powrotem: kazda strona, ktora
 * poszla na dysk, wraca przy pierwszym dotknieciu - i musi wrocic
 * z dokladnie ta sama zawartoscia.
 *
 * Program o wymianie nie wie nic i wlasnie o to chodzi: dla niego
 * pamiec jest po prostu pamiecia. Sprawdza sie sam - gdyby wrocil
 * choc jeden inny bajt, konczy bledem zamiast komunikatem, ktory ktos
 * musialby przeczytac.
 *
 * Uzycie: run swaptest64 [MiB] (domyslnie 14).
 */

#include "usys64.h"

#define STRONA 4096

static char out[96];

static void powiedz(const char *a, uint64_t v, const char *b)
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
    out[n] = '\0';
    print(out);
}

static unsigned char znacznik(uint64_t strona)
{
    return (unsigned char)(strona * 7 + 13);
}

void _start(int argc, char **argv)
{
    uint64_t mib = 14;
    if (argc > 1) {
        int64_t v = uatoi(argv[1]);
        if (v > 0)
            mib = (uint64_t)v;
    }
    uint64_t bajtow = mib * 1024u * 1024u;
    uint64_t stron = bajtow / STRONA;

    unsigned char *p = sys_sbrk((int64_t)bajtow);
    if (!p) {
        print("[swaptest64] sbrk odmowil\n");
        sys_exit(1);
    }
    p = (unsigned char *)(((uintptr_t)p + STRONA - 1) & ~(uintptr_t)(STRONA - 1));

    powiedz("[swaptest64] zapisuje ", mib, " MiB\n");
    for (uint64_t i = 0; i < stron; i++)
        p[i * STRONA] = znacznik(i);

    print("[swaptest64] czytam z powrotem\n");
    uint64_t zle = 0;
    for (uint64_t i = 0; i < stron; i++)
        if (p[i * STRONA] != znacznik(i))
            zle++;

    if (zle) {
        powiedz("[swaptest64] ZLE: stron z bledna zawartoscia ", zle, "\n");
        sys_exit(1);
    }
    powiedz("[swaptest64] wszystkie ", stron, " stron wrocilo w calosci\n");
    sys_exit(0);
}
