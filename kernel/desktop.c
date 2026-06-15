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
#include "include/heap.h"
#include "include/string.h"
#include "include/font8x8.h"
#include "include/io.h"

/* Indeksy palety pulpitu (ustawiane w desktop_thread) */
#define COL_BLACK  0
#define COL_TITLE  3      /* pasek tytulu okna (niebieski) */
#define COL_WHITE  4
#define COL_DESK   6      /* tlo pulpitu (ciemny blekit) */
#define COL_BAR    7      /* gorny pasek (jasnoszary) */
#define COL_WIN    8      /* tlo okna (szary) */

#define TITLE_H 10

#define COL_RED    1      /* przycisk zamkniecia */

/* Zdarzenie dla aplikacji (uklad zgodny z usys.h struct win_event) */
struct wevent {
    int32_t type;          /* 0 = brak, 1 = klik myszy */
    int32_t x, y;          /* wspolrzedne wzgledem obszaru treści */
    int32_t val;           /* maska przyciskow */
};
#define WEV_NONE  0
#define WEV_CLICK 1
#define WEV_QLEN  8

struct window {
    int x, y, w, h;
    char title[24];
    int used;
    int has_button;        /* czy okno ma przycisk akcji */
    int counter;           /* licznik klikniec przycisku */
    int owner;             /* pid procesu-wlasciciela; -1 = okno jadra */
    uint8_t *content;      /* bufor treści aplikacji (NULL = okno jadra) */
    int cw, ch;            /* wymiary obszaru treści */
    struct wevent evq[WEV_QLEN];  /* kolejka zdarzen dla aplikacji */
    int ev_head, ev_tail;
};

#define MAX_WIN 6
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

/* --- Rysowanie do bufora treści aplikacji (indeksy palety) --- */

static void buf_rect(struct window *w, int x, int y, int rw, int rh, uint8_t c)
{
    for (int j = 0; j < rh; j++) {
        int py = y + j;
        if (py < 0 || py >= w->ch)
            continue;
        for (int i = 0; i < rw; i++) {
            int px = x + i;
            if (px >= 0 && px < w->cw)
                w->content[py * w->cw + px] = c;
        }
    }
}

static void buf_char(struct window *w, int x, int y, char ch, uint8_t c)
{
    if (ch < 0x20 || ch > 0x7E)
        return;
    const uint8_t *g = font8x8[ch - 0x20];
    for (int row = 0; row < 8; row++)
        for (int col = 0; col < 8; col++)
            if (g[row] & (1 << col)) {
                int px = x + col, py = y + row;
                if (px >= 0 && px < w->cw && py >= 0 && py < w->ch)
                    w->content[py * w->cw + px] = c;
            }
}

static void buf_text(struct window *w, int x, int y, const char *s, uint8_t c)
{
    for (int i = 0; s[i]; i++)
        buf_char(w, x + i * 8, y, s[i], c);
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

    /* Okno aplikacji: skomponuj jej bufor treści w obszarze roboczym */
    if (win->content) {
        int bx = win->x + 1;
        int by = win->y + TITLE_H + 1;
        for (int j = 0; j < win->ch; j++)
            for (int i = 0; i < win->cw; i++)
                video_putpixel(bx + i, by + j,
                               win->content[j * win->cw + i]);
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
            /* klik w cialo okna aplikacji -> zdarzenie do procesu */
            if (in_rect(mx, my, w->x, w->y, w->w, w->h)) {
                if (w->content) {
                    int cx = mx - (w->x + 1);
                    int cy = my - (w->y + TITLE_H + 1);
                    if (cx >= 0 && cx < w->cw && cy >= 0 && cy < w->ch) {
                        int next = (w->ev_head + 1) % WEV_QLEN;
                        if (next != w->ev_tail) {  /* kolejka nie pelna */
                            w->evq[w->ev_head] = (struct wevent){
                                WEV_CLICK, cx, cy, buttons & 0x07};
                            w->ev_head = next;
                        }
                    }
                }
                break;            /* pochlon - nie przebijaj do nizszych */
            }
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

/* Kopiuje tytul do okna (z przycieciem) */
static void set_title(struct window *w, const char *t)
{
    int i = 0;
    for (; t[i] && i < (int)sizeof(w->title) - 1; i++)
        w->title[i] = t[i];
    w->title[i] = '\0';
}

/* Zwalnia okna, ktorych proces-wlasciciel juz nie zyje */
static void reap_windows(void)
{
    for (int i = 0; i < MAX_WIN; i++) {
        if (wins[i].used && wins[i].owner >= 0 && !task_alive(wins[i].owner)) {
            kfree(wins[i].content);
            wins[i].content = (void *)0;
            wins[i].used = 0;
        }
    }
}

/* --- API dla syscalli aplikacji (kontekst syscalla = IF=0) --- */

int desktop_win_create(int owner, int cw, int ch, const char *title)
{
    if (cw <= 0 || ch <= 0 || cw > VIDEO_W || ch > VIDEO_H)
        return -1;
    for (int i = 0; i < MAX_WIN; i++) {
        if (wins[i].used)
            continue;
        uint8_t *buf = kmalloc(cw * ch);
        if (!buf)
            return -1;
        memset(buf, COL_WIN, cw * ch);     /* startowo szare tlo */
        struct window *w = &wins[i];
        /* kaskadowe ustawienie nowych okien aplikacji */
        w->x = 70 + i * 12;
        w->y = 30 + i * 12;
        w->w = cw + 2;
        w->h = ch + TITLE_H + 2;
        set_title(w, title);
        w->has_button = 0;
        w->counter = 0;
        w->owner = owner;
        w->content = buf;
        w->cw = cw;
        w->ch = ch;
        w->used = 1;
        return i;
    }
    return -1;
}

/* Sprawdza, czy winid jest oknem aplikacji owner */
static struct window *app_window(int winid, int owner)
{
    if (winid < 0 || winid >= MAX_WIN)
        return (void *)0;
    struct window *w = &wins[winid];
    if (!w->used || !w->content || w->owner != owner)
        return (void *)0;
    return w;
}

int desktop_win_rect(int winid, int owner, int x, int y, int w, int h,
                     uint8_t color)
{
    struct window *win = app_window(winid, owner);
    if (!win)
        return -1;
    buf_rect(win, x, y, w, h, color);
    return 0;
}

int desktop_win_text(int winid, int owner, int x, int y, const char *s,
                     uint8_t color)
{
    struct window *win = app_window(winid, owner);
    if (!win)
        return -1;
    buf_text(win, x, y, s, color);
    return 0;
}

/* Pobiera kolejne zdarzenie okna do *out. Zwraca 1, gdy bylo, inaczej 0. */
int desktop_win_poll(int winid, int owner, void *out)
{
    struct window *win = app_window(winid, owner);
    if (!win)
        return -1;
    uint32_t flags = irq_save();
    int got = 0;
    if (win->ev_tail != win->ev_head) {
        *(struct wevent *)out = win->evq[win->ev_tail];
        win->ev_tail = (win->ev_tail + 1) % WEV_QLEN;
        got = 1;
    }
    irq_restore(flags);
    return got;
}

static void desktop_thread(void *arg)
{
    (void)arg;
    video_init();
    video_set_palette(COL_DESK, 0, 32, 40);   /* ciemny blekit pulpitu */
    video_set_palette(COL_BAR, 48, 48, 48);   /* jasnoszary gorny pasek */
    video_set_palette(COL_WIN, 56, 56, 56);   /* szare tlo okna */

    /* Dwa okna jadra (bez wlasciciela, bez bufora treści) */
    wins[0] = (struct window){.x = 40, .y = 60, .w = 120, .h = 70,
                              .used = 1, .has_button = 1, .owner = -1};
    set_title(&wins[0], "Terminal");
    wins[1] = (struct window){.x = 180, .y = 95, .w = 120, .h = 70,
                              .used = 1, .has_button = 0, .owner = -1};
    set_title(&wins[1], "Pliki");

    for (;;) {
        reap_windows();

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
