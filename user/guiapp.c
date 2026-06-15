/* guiapp - aplikacja graficzna w ring 3
 *
 * Tworzy wlasne okno na pulpicie i rysuje w nim przez syscalle
 * (win_rect / win_text) - jadrowy kompozytor sklada jego bufor na
 * ekran. Co ~0.5 s odswieza zawartosc (licznik), wiec widac, ze to
 * zywy proces uzytkownika rysujacy do swojego okna. Konczy sie przez
 * kill; kompozytor sprzata jego okno, gdy proces zniknie.
 */

#include "usys.h"

/* Indeksy palety pulpitu */
#define C_BG    8    /* szare tlo okna */
#define C_RED   1
#define C_GREEN 2
#define C_BLUE  3
#define C_WHITE 4

void _start(void)
{
    int w = win_create(120, 50, "Aplikacja");
    if (w < 0)
        sys_exit(1);

    int n = 0;
    for (;;) {
        win_rect(w, 0, 0, 120, 50, C_BG);          /* wyczysc tlo */
        win_rect(w, 6, 6, 30, 16, C_RED);          /* kolorowe kwadraty */
        win_rect(w, 40, 6, 30, 16, C_GREEN);
        win_rect(w, 74, 6, 30, 16, C_BLUE);
        win_text(w, 6, 30, "ring3 rysuje:", C_WHITE);

        /* licznik klatek jako tekst (dowod zywej aplikacji) */
        char buf[12];
        int v = n, len = 0;
        char tmp[12];
        do {
            tmp[len++] = (char)('0' + v % 10);
            v /= 10;
        } while (v);
        int j = 0;
        while (len)
            buf[j++] = tmp[--len];
        buf[j] = '\0';
        win_text(w, 100, 30, buf, C_WHITE);

        n++;
        sys_sleep(500);
    }
}
