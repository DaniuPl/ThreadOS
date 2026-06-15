/* guiapp - interaktywna aplikacja graficzna w ring 3
 *
 * Tworzy wlasne okno i rysuje przez syscalle. Kompozytor jadra dostarcza
 * jej zdarzenia (klikniecia w obszar okna) przez win_poll - aplikacja
 * reaguje: w miejscu kazdego klikniecia rysuje bialy znacznik i zlicza
 * je. Konczy sie przez kill; kompozytor sprzata okno.
 */

#include "usys.h"

#define C_BG    8    /* szare tlo okna */
#define C_RED   1
#define C_GREEN 2
#define C_BLUE  3
#define C_WHITE 4

#define MAXPT 64
static int px[MAXPT], py[MAXPT];
static int npt;

static void draw_int(int w, int x, int y, int v, int col)
{
    char tmp[12], s[12];
    int n = 0, j = 0;
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        s[j++] = tmp[--n];
    s[j] = '\0';
    win_text(w, x, y, s, col);
}

void _start(void)
{
    int w = win_create(120, 60, "Aplikacja");
    if (w < 0)
        sys_exit(1);

    for (;;) {
        /* Odbierz wszystkie oczekujace zdarzenia (klikniecia) */
        struct win_event ev;
        while (win_poll(w, &ev) == 1) {
            if (ev.type == WEV_CLICK && npt < MAXPT) {
                px[npt] = ev.x;
                py[npt] = ev.y;
                npt++;
            }
        }

        /* Przerysuj okno: tlo, paleta kwadratow, podpis, licznik klikniec */
        win_rect(w, 0, 0, 120, 60, C_BG);
        win_rect(w, 6, 6, 24, 12, C_RED);
        win_rect(w, 34, 6, 24, 12, C_GREEN);
        win_rect(w, 62, 6, 24, 12, C_BLUE);
        win_text(w, 6, 24, "klikniec:", C_WHITE);
        draw_int(w, 80, 24, npt, C_WHITE);

        /* Znaczniki w miejscach klikniec */
        for (int i = 0; i < npt; i++)
            win_rect(w, px[i] - 2, py[i] - 2, 5, 5, C_WHITE);

        sys_sleep(80);
    }
}
