/* heap64 - sterta od strony programu
 *
 * Do tej pory kazdy bufor musial byc wpisany w program na sztywno.
 * Ten prosi jadro o pamiec w biegu i uzywa jej.
 *
 * Najciekawsze jest to, ile ta prosba kosztuje. Program prosi o cwierc
 * megabajta, a potem dotyka tylko czterech stron - i tyle wlasnie
 * powinno ubyc pamieci, bo strony pojawiaja sie dopiero pod palcami.
 * Program sam tego nie widzi; widzi to jadro i mowi po wszystkim.
 *
 * Sprawdza sie sam: gdyby ktorykolwiek bajt wrocil inny, konczy
 * bledem zamiast komunikatu, ktory ktos musi przeczytac.
 */

#include "usys64.h"

#define PROSBA  (4 * 1024 * 1024)
#define STRONA  4096
#define DOTKNAC 4

static char out[128];

static int put(int n, const char *s)
{
    while (*s)
        out[n++] = *s++;
    return n;
}

static int putn(int n, uint64_t v)
{
    char tmp[24];
    utoa(v, tmp);
    return put(n, tmp);
}

static int puth(int n, uint64_t v)
{
    char tmp[24];
    uhtoa(v, tmp);
    return put(n, tmp);
}

static void koniec_z_bledem(const char *co)
{
    print("[heap64] BLAD: ");
    print(co);
    print("\n");
    sys_exit(1);
}

void _start(void)
{
    void *poczatek = sys_sbrk(0);
    if (!poczatek)
        koniec_z_bledem("nie mam sterty");

    int n = put(0, "[heap64] sterta zaczyna sie pod ");
    n = puth(n, (uint64_t)(uintptr_t)poczatek);
    n = put(n, ", prosze o ");
    n = putn(n, PROSBA / 1024);
    n = put(n, " KiB (");
    n = putn(n, PROSBA / STRONA);
    n = put(n, " stron)\n");
    out[n] = 0;
    print(out);

    char *pamiec = (char *)sys_sbrk(PROSBA);
    if (!pamiec)
        koniec_z_bledem("sbrk odmowil");

    /* Dotykamy tylko kilku stron - reszta ma nie istniec */
    for (int i = 0; i < DOTKNAC; i++)
        for (int j = 0; j < STRONA; j++)
            pamiec[i * STRONA + j] = (char)(i * 31 + j);

    for (int i = 0; i < DOTKNAC; i++)
        for (int j = 0; j < STRONA; j++)
            if (pamiec[i * STRONA + j] != (char)(i * 31 + j))
                koniec_z_bledem("pamiec oddala co innego, niz wpisano");

    n = put(0, "[heap64] dotknalem ");
    n = putn(n, DOTKNAC);
    n = put(n, " z nich i wszystkie oddaly to, co wpisalem\n");
    out[n] = 0;
    print(out);

    /* Zwezenie oddaje pamiec od razu */
    if (!sys_sbrk(-PROSBA))
        koniec_z_bledem("nie moge oddac pamieci");
    if (sys_sbrk(0) != poczatek)
        koniec_z_bledem("granica nie wrocila tam, gdzie byla");
    print("[heap64] oddalem wszystko, granica wrocila na miejsce\n");

    /* Prosba poza wlasna pamiec musi zostac odrzucona */
    if (sys_sbrk(-4096))
        koniec_z_bledem("jadro pozwolilo zejsc ponizej sterty");
    print("[heap64] ponizej wlasnej sterty jadro nie puszcza\n");

    sys_exit(0);
}
