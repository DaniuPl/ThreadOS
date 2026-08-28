/* uls64 - wypisuje zawartosc katalogu roboczego
 *
 * Jadro ma wbudowane 'fls', bo do niedawna nie bylo inaczej. Ten robi
 * to samo, ale jest zwyklym programem i pyta o wpisy po kolei -
 * jeden na wywolanie, dopoki dostaje odpowiedz.
 *
 * Katalog roboczy jest wlasnoscia zadania, wiec uls64 wypisuje ten,
 * w ktorym stal shell, gdy go uruchamial.
 */

#include "usys64.h"

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

void _start(void)
{
    char sciezka[128];
    struct dirent64 wpis;

    if (sys_getcwd(sciezka, sizeof(sciezka)) < 0) {
        print("uls64: nie wiem, gdzie stoje\n");
        sys_exit(1);
    }

    int n = put(0, "uls64: ");
    n = put(n, sciezka);
    n = put(n, "\n");
    out[n] = 0;
    print(out);

    int ile = 0;
    for (int i = 0; sys_readdir(i, &wpis) == 0; i++) {
        n = put(0, "  ");
        n = put(n, wpis.name);
        if (wpis.is_dir) {
            n = put(n, "/  (katalog)");
        } else {
            n = put(n, "  (");
            n = putn(n, wpis.size);
            n = put(n, " B)");
        }
        n = put(n, "\n");
        out[n] = 0;
        print(out);
        ile++;
    }

    n = put(0, "uls64: wpisow ");
    n = putn(n, (uint64_t)ile);
    n = put(n, "\n");
    out[n] = 0;
    print(out);
    sys_exit(0);
}
