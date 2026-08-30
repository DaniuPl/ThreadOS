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

#define REQUEST  (4 * 1024 * 1024)
#define PAGE_MARK  4096
#define TOUCH 4

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

static void fail_with(const char *co)
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
        fail_with("nie mam sterty");

    int n = put(0, "[heap64] sterta zaczyna sie pod ");
    n = puth(n, (uint64_t)(uintptr_t)poczatek);
    n = put(n, ", prosze o ");
    n = putn(n, REQUEST / 1024);
    n = put(n, " KiB (");
    n = putn(n, REQUEST / PAGE_MARK);
    n = put(n, " stron)\n");
    out[n] = 0;
    print(out);

    char *memory = (char *)sys_sbrk(REQUEST);
    if (!memory)
        fail_with("sbrk odmowil");

    /* Dotykamy tylko kilku stron - reszta ma nie istniec */
    for (int i = 0; i < TOUCH; i++)
        for (int j = 0; j < PAGE_MARK; j++)
            memory[i * PAGE_MARK + j] = (char)(i * 31 + j);

    for (int i = 0; i < TOUCH; i++)
        for (int j = 0; j < PAGE_MARK; j++)
            if (memory[i * PAGE_MARK + j] != (char)(i * 31 + j))
                fail_with("pamiec oddala co innego, niz wpisano");

    n = put(0, "[heap64] dotknalem ");
    n = putn(n, TOUCH);
    n = put(n, " z nich i wszystkie oddaly to, co wpisalem\n");
    out[n] = 0;
    print(out);

    /* Zwezenie oddaje pamiec od razu */
    if (!sys_sbrk(-REQUEST))
        fail_with("nie moge oddac pamieci");
    if (sys_sbrk(0) != poczatek)
        fail_with("granica nie wrocila tam, gdzie byla");
    print("[heap64] oddalem wszystko, granica wrocila na miejsce\n");

    /* Prosba poza wlasna pamiec musi zostac odrzucona */
    if (sys_sbrk(-4096))
        fail_with("jadro pozwolilo zejsc ponizej sterty");
    print("[heap64] ponizej wlasnej sterty jadro nie puszcza\n");

    sys_exit(0);
}
