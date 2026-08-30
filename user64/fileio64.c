/* fileio64 - plik od strony programu
 *
 * Do tej pory do dysku siegalo tylko jadro. Ten program robi to sam:
 * zaklada plik, zapisuje do niego, zamyka, otwiera z powrotem i czyta.
 * Nie ma przy tym zadnych szczegolnych uprawnien - deskryptor jest
 * zwyklym numerem w jego wlasnej tablicy, a i-wezel, na ktory numer
 * wskazuje, zna juz tylko jadro.
 *
 * Sprawdza sie sam: gdyby odczyt oddal cokolwiek innego niz zapis,
 * konczy sie bledem, a nie komunikatem, ktory ktos musi przeczytac.
 */

#include "usys64.h"

static const char BODY[] = "ProtonOS 64: plik zapisany z ring 3\n";
static char buf[128];
static char out[160];

static int put(int n, const char *s)
{
    while (*s)
        out[n++] = *s++;
    return n;
}

static int putn(int n, int64_t v)
{
    char tmp[24];
    if (v < 0) {
        out[n++] = '-';
        v = -v;
    }
    utoa((uint64_t)v, tmp);
    return put(n, tmp);
}

static void err(const char *co, int64_t v)
{
    int n = put(0, "[fileio64] BLAD: ");
    n = put(n, co);
    n = put(n, " -> ");
    n = putn(n, v);
    out[n++] = '\n';
    out[n] = 0;
    print(out);
    sys_exit(1);
}

void _start(void)
{

    uint64_t len = ustrlen(BODY);

    print("[fileio64] zakladam plik i pisze do niego z ring 3\n");
    int64_t fd = sys_open("zring3", 1);
    if (fd < 0)
        err("open do zapisu", fd);

    int64_t n = sys_write((int)fd, BODY, len);
    if (n != (int64_t)len)
        err("write", n);
    sys_close((int)fd);

    /* Drugie otwarcie zaczyna od poczatku pliku - miejsce, w ktorym
       stal poprzedni deskryptor, znikneło razem z nim */
    print("[fileio64] otwieram go z powrotem i czytam\n");
    fd = sys_open("zring3", 0);
    if (fd < 0)
        err("open do odczytu", fd);

    n = sys_read((int)fd, buf, sizeof(buf));
    sys_close((int)fd);
    if (n != (int64_t)len)
        err("read oddal inna dlugosc", n);

    for (uint64_t i = 0; i < len; i++)
        if (buf[i] != BODY[i])
            err("tresc sie nie zgadza na pozycji", (int64_t)i);

    int m = put(0, "[fileio64] zapisane i odczytane zgadza sie co do bajtu (");
    m = putn(m, (int64_t)len);
    m = put(m, " B)\n");
    out[m] = 0;
    print(out);

    /* Plik ma nie istniec dopiero wtedy, gdy go nie ma */
    if (sys_open("nie-ma-takiego", 0) >= 0)
        err("open wymyslil sobie plik", 0);
    print("[fileio64] nieistniejacego pliku jadro nie otwiera\n");

    sys_exit(0);
}
