/* Graficzny pulpit ProtonOS - prosty kompozytor
 *
 * Dziala jako watek jadra: w petli przerysowuje cala klatke do
 * framebuffera (tlo -> kursor) i oddaje procesor na ~30 ms. Przy
 * 320x200 pelne przerysowanie to 64 KB - tanio, wiec nie bawimy sie
 * w zapamietywanie tla pod kursorem.
 */

#include <stdint.h>
#include "include/desktop.h"
#include "include/video.h"
#include "include/mouse.h"
#include "include/task.h"

/* Indeksy palety pulpitu (ustawiane w desktop_thread) */
#define COL_BLACK  0
#define COL_TITLE  3      /* pasek tytulu okna (niebieski) */
#define COL_WHITE  4
#define COL_DESK   6      /* tlo pulpitu (ciemny blekit) */
#define COL_BAR    7      /* gorny pasek (jasnoszary) */
#define COL_WIN    8      /* tlo okna (szary) */

#define TITLE_H 10

#define COL_RED    1      /* przycisk zamkniecia */

struct window {
    int x, y, w, h;
    const char *title;
    int used;
    int has_button;        /* czy okno ma przycisk akcji */
    int counter;           /* licznik klikniec przycisku */
};

#define MAX_WIN 3
static struct window wins[MAX_WIN];

/* Stan przeciagania: indeks okna i uchwyt (offset kursora w oknie) */
static int drag_idx = -1;
static int drag_dx, drag_dy;
static int prev_pressed;    /* poprzedni stan LPM - do wykrycia klikniecia */

#define CLOSE_SZ 8          /* przycisk [x] w prawym rogu paska */
#define BTN_W 40
#define BTN_H 12

static int in_rect(int x, int y, int rx, int ry, int rw, int rh)
{
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static int close_x(const struct window *w) { return w->x + w->w - CLOSE_SZ - 1; }
static int close_y(const struct window *w) { return w->y + 1; }
static int btn_x(const struct window *w)   { return w->x + 8; }
static int btn_y(const struct window *w)    { return w->y + TITLE_H + 8; }

/* Strzalka kursora 8x12: bity ustawione = piksel kursora.
   Bit najstarszy (0x80) to lewa kolumna. */
static const uint8_t cursor_bmp[12] = {
    0x80, 0xC0, 0xE0, 0xF0, 0xF8, 0xFC,
    0xFE, 0xFF, 0xF8, 0xD8, 0x88, 0x18,
};

static void draw_cursor(int mx, int my)
{
    for (int row = 0; row < 12; row++) {
        for (int col = 0; col < 8; col++) {
            if (!(cursor_bmp[row] & (0x80 >> col)))
                continue;
            /* Czarny kontur (sasiednie piksele) dla widocznosci na
               kazdym tle, bialy srodek na wierzchu */
            video_putpixel(mx + col - 1, my + row, COL_BLACK);
            video_putpixel(mx + col + 1, my + row, COL_BLACK);
            video_putpixel(mx + col, my + row - 1, COL_BLACK);
            video_putpixel(mx + col, my + row + 1, COL_BLACK);
        }
    }
    for (int row = 0; row < 12; row++)
        for (int col = 0; col < 8; col++)
            if (cursor_bmp[row] & (0x80 >> col))
                video_putpixel(mx + col, my + row, COL_WHITE);
}

/* Rysuje liczbe dziesietna jako tekst */
static void draw_int(int x, int y, int v, uint8_t col)
{
    char tmp[12];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    char s[12];
    int j = 0;
    while (n)
        s[j++] = tmp[--n];
    s[j] = '\0';
    video_draw_string(x, y, s, col);
}

static void draw_window(const struct window *win)
{
    /* Tlo okna z czarna ramka */
    video_fill_rect(win->x, win->y, win->w, win->h, COL_WIN);
    video_fill_rect(win->x, win->y, win->w, 1, COL_BLACK);
    video_fill_rect(win->x, win->y + win->h - 1, win->w, 1, COL_BLACK);
    video_fill_rect(win->x, win->y, 1, win->h, COL_BLACK);
    video_fill_rect(win->x + win->w - 1, win->y, 1, win->h, COL_BLACK);
    /* Pasek tytulu + nazwa */
    video_fill_rect(win->x + 1, win->y + 1, win->w - 2, TITLE_H, COL_TITLE);
    video_draw_string(win->x + 3, win->y + 2, win->title, COL_WHITE);
    /* Przycisk zamkniecia [x] w prawym rogu paska */
    video_fill_rect(close_x(win), close_y(win), CLOSE_SZ, CLOSE_SZ, COL_RED);
    video_draw_char(close_x(win), close_y(win), 'x', COL_WHITE);

    /* Opcjonalny przycisk akcji + licznik klikniec */
    if (win->has_button) {
        video_fill_rect(btn_x(win), btn_y(win), BTN_W, BTN_H, COL_BAR);
        video_draw_string(btn_x(win) + 4, btn_y(win) + 2, "Klik", COL_BLACK);
        draw_int(btn_x(win) + BTN_W + 6, btn_y(win) + 2, win->counter,
                 COL_WHITE);
    }
}

/* Czy punkt jest na pasku tytulu okna (uchwyt do przeciagania) */
static int on_titlebar(const struct window *win, int x, int y)
{
    return x >= win->x && x < win->x + win->w &&
           y >= win->y && y < win->y + TITLE_H;
}

/* Obsluga myszy: klikniecia (zamkniecie, przycisk) i przeciaganie okien.
   Klikniecia maja pierwszenstwo przed przeciaganiem; oba liczone na
   zboczu wcisniecia LPM, by jeden klik = jedna akcja. */
static void handle_input(int mx, int my, int buttons)
{
    int pressed = buttons & 0x01;

    if (pressed && !prev_pressed) {     /* zbocze wcisniecia = klik */
        for (int i = MAX_WIN - 1; i >= 0; i--) {
            struct window *w = &wins[i];
            if (!w->used)
                continue;
            /* przycisk zamkniecia [x] */
            if (in_rect(mx, my, close_x(w), close_y(w), CLOSE_SZ, CLOSE_SZ)) {
                w->used = 0;
                break;
            }
            /* przycisk akcji w oknie */
            if (w->has_button &&
                in_rect(mx, my, btn_x(w), btn_y(w), BTN_W, BTN_H)) {
                w->counter++;
                break;
            }
            /* pasek tytulu -> rozpocznij przeciaganie */
            if (on_titlebar(w, mx, my)) {
                drag_idx = i;
                drag_dx = mx - w->x;
                drag_dy = my - w->y;
                break;
            }
            /* klik w cialo okna - pochlon, nie przebijaj do nizszych */
            if (in_rect(mx, my, w->x, w->y, w->w, w->h))
                break;
        }
    }

    if (!pressed)
        drag_idx = -1;

    if (pressed && drag_idx >= 0) {     /* w trakcie: okno podaza za kursorem */
        int nx = mx - drag_dx;
        int ny = my - drag_dy;
        if (nx < 0) nx = 0;
        if (ny < TITLE_H) ny = TITLE_H;
        if (nx > VIDEO_W - wins[drag_idx].w) nx = VIDEO_W - wins[drag_idx].w;
        if (ny > VIDEO_H - wins[drag_idx].h) ny = VIDEO_H - wins[drag_idx].h;
        wins[drag_idx].x = nx;
        wins[drag_idx].y = ny;
    }

    prev_pressed = pressed;
}

static void desktop_thread(void *arg)
{
    (void)arg;
    video_init();
    video_set_palette(COL_DESK, 0, 32, 40);   /* ciemny blekit pulpitu */
    video_set_palette(COL_BAR, 48, 48, 48);   /* jasnoszary gorny pasek */
    video_set_palette(COL_WIN, 56, 56, 56);   /* szare tlo okna */

    wins[0] = (struct window){40, 60, 120, 70, "Terminal", 1, 1, 0};
    wins[1] = (struct window){180, 95, 120, 70, "Pliki", 1, 0, 0};

    for (;;) {
        int mx, my, buttons;
        mouse_get(&mx, &my, &buttons);
        handle_input(mx, my, buttons);

        video_clear(COL_DESK);

        /* Gorny pasek z tytulem systemu */
        video_fill_rect(0, 0, VIDEO_W, 10, COL_BAR);
        video_draw_string(2, 1, "ProtonOS", COL_BLACK);

        for (int i = 0; i < MAX_WIN; i++)
            if (wins[i].used)
                draw_window(&wins[i]);

        draw_cursor(mx, my);

        task_sleep(30);                       /* ~33 klatek/s */
    }
}

void desktop_start(void)
{
    task_create("desktop", desktop_thread, (void *)0);
}
