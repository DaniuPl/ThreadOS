/* guiapp64 - aplikacja graficzna w pierscieniu 3, jadro 64-bitowe
 *
 * Pulpit dzialal do tej pory sam ze soba: kompozytor byl w jadrze,
 * a ikony wskazywaly programy, ktorych w trybie dlugim nie bylo.
 * Ten program jest po drugiej stronie tej granicy - rysuje przez
 * wywolania systemowe, we wlasnym oknie, i dostaje z powrotem
 * zdarzenia: klikniecia w obszar okna i klawisze, gdy okno ma fokus.
 *
 * Kolory to indeksy palety pulpitu, nie RGB - jadro trzyma palete,
 * program nie musi o niej nic wiedziec.
 */

#include "usys64.h"

#define C_RED   1
#define C_GREEN 2
#define C_BLUE  3
#define C_WHITE 4
#define C_BG    8    /* szare tlo okna */

#define OKNO_W 120
#define OKNO_H 60

#define MAXPT 64
static int px[MAXPT], py[MAXPT];
static int npt;

static char wpisane[16];
static int dlug;

static void rysuj_liczbe(int w, int x, int y, int v, int kolor)
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
    win_text(w, x, y, s, kolor);
}

void _start(void)
{
    int w = (int)win_create(OKNO_W, OKNO_H, "App64");
    if (w < 0) {
        print("guiapp64: pulpit nie dal okna\n");
        sys_exit(1);
    }

    for (;;) {
        /* Zdarzenia: klikniecia i klawisze. Kolejka jest krotka,
           wiec oprozniamy ja do konca przy kazdej klatce. */
        struct win_event ev;
        while (win_poll(w, &ev) == 1) {
            if (ev.typ == WEV_CLICK && npt < MAXPT) {
                px[npt] = ev.x;
                py[npt] = ev.y;
                npt++;
            } else if (ev.typ == WEV_KEY) {
                char c = (char)ev.val;
                if (c == '\b') {
                    if (dlug > 0)
                        dlug--;
                } else if (c >= 0x20 && c < 0x7F &&
                           dlug < (int)sizeof(wpisane) - 1) {
                    wpisane[dlug++] = c;
                }
            }
        }
        wpisane[dlug] = '\0';

        /* Cala tresc okna od nowa: tlo, paleta, podpisy, licznik */
        win_rect(w, 0, 0, OKNO_W, OKNO_H, C_BG);
        win_rect(w, 6, 6, 24, 12, C_RED);
        win_rect(w, 34, 6, 24, 12, C_GREEN);
        win_rect(w, 62, 6, 24, 12, C_BLUE);
        win_text(w, 6, 24, "klikniec:", C_WHITE);
        rysuj_liczbe(w, 80, 24, npt, C_WHITE);
        win_text(w, 6, 38, "txt:", C_GREEN);
        win_text(w, 40, 38, wpisane, C_WHITE);

        /* Znaczniki w miejscach klikniec */
        for (int i = 0; i < npt; i++)
            win_rect(w, px[i] - 2, py[i] - 2, 5, 5, C_WHITE);

        sys_sleep(80);
    }
}
