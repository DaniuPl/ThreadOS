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
#include "include/keyboard.h"
#include "include/exec.h"
#include "include/io.h"

/* Indeksy palety pulpitu (ustawiane w desktop_thread) */
#define COL_BLACK  0
#define COL_TITLE  3      /* pasek tytulu okna (niebieski) */
#define COL_WHITE  4
#define COL_DESK   6      /* tlo pulpitu (ciemny blekit) */
#define COL_BAR    7      /* gorny pasek (jasnoszary) */
#define COL_WIN    8      /* tlo okna (szary) */
#define COL_TBBTN  9      /* przycisk okna na pasku zadan (ciemnoszary) */

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
#define WEV_KEY   2
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
    int z;                 /* porzadek rysowania (wyzszy = na wierzchu) */
    struct wevent evq[WEV_QLEN];  /* kolejka zdarzen dla aplikacji */
    int ev_head, ev_tail;
};

#define MAX_WIN 6
static struct window wins[MAX_WIN];

/* Stan przeciagania: indeks okna i uchwyt (offset kursora w oknie) */
static int drag_idx = -1;
static int drag_dx, drag_dy;
static int prev_pressed;    /* poprzedni stan LPM - do wykrycia klikniecia */
static int focus_idx = -1;  /* okno z fokusem klawiatury */

/* Pasek zadan i menu uruchamiania */
#define TB_H 12                       /* wysokosc paska zadan */
#define MENU_BW 40                    /* szerokosc przycisku "Menu" */
#define MENU_ROW 12                   /* wysokosc wpisu menu */
#define MENU_W 80                     /* szerokosc rozwinietego menu */
static int menu_open;
static const char *menu_items[] = {"guiapp", "gclock"};
#define MENU_N ((int)(sizeof(menu_items) / sizeof(menu_items[0])))

static int taskbar_y(void) { return VIDEO_H - TB_H; }
static int menu_item_y(int i) { return taskbar_y() - (MENU_N - i) * MENU_ROW; }

/* Lista przyciskow okien na pasku zadan (po przycisku Menu) */
#define TBL_X0 (2 + MENU_BW + 4)
#define TBL_W  52

static int z_top;           /* najwyzszy przydzielony porzadek Z */

/* Wynosi okno na wierzch (najwyzszy Z) */
static void raise_win(int i)
{
    wins[i].z = ++z_top;
}

/* Wypelnia out[] slotami uzywanych okien w kolejnosci Z (od spodu);
   zwraca ich liczbe */
static int zorder(int *out)
{
    int n = 0;
    for (int i = 0; i < MAX_WIN; i++)
        if (wins[i].used)
            out[n++] = i;
    for (int a = 1; a < n; a++) {     /* sortowanie przez wstawianie po z */
        int key = out[a], b = a - 1;
        while (b >= 0 && wins[out[b]].z > wins[key].z) {
            out[b + 1] = out[b];
            b--;
        }
        out[b + 1] = key;
    }
    return n;
}

/* Wstawia zdarzenie do kolejki okna (chronione - wolane z kompozytora,
   IRQ klawiatury i posrednio z syscalla) */
static void win_enqueue(struct window *w, int type, int x, int y, int val)
{
    if (!w->content)
        return;
    uint32_t flags = irq_save();
    int next = (w->ev_head + 1) % WEV_QLEN;
    if (next != w->ev_tail) {
        w->evq[w->ev_head] = (struct wevent){type, x, y, val};
        w->ev_head = next;
    }
    irq_restore(flags);
}

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
        /* Pasek zadan / menu uruchamiania ma pierwszenstwo (na wierzchu) */
        if (in_rect(mx, my, 2, taskbar_y() + 2, MENU_BW, TB_H - 4)) {
            menu_open = !menu_open;
            prev_pressed = pressed;
            return;
        }
        if (menu_open) {
            for (int i = 0; i < MENU_N; i++) {
                if (in_rect(mx, my, 2, menu_item_y(i), MENU_W, MENU_ROW)) {
                    exec_spawn(menu_items[i]);   /* uruchom aplikacje */
                    menu_open = 0;
                    prev_pressed = pressed;
                    return;
                }
            }
            menu_open = 0;                /* klik poza menu - zamknij */
        }
        /* Przyciski okien na pasku zadan: fokus + wyniesienie na wierzch */
        if (my >= taskbar_y()) {
            int j = 0;
            for (int i = 0; i < MAX_WIN; i++) {
                if (!wins[i].used)
                    continue;
                if (in_rect(mx, my, TBL_X0 + j * TBL_W, taskbar_y(),
                            TBL_W - 2, TB_H)) {
                    focus_idx = i;
                    raise_win(i);
                    prev_pressed = pressed;
                    return;
                }
                j++;
            }
        }

        int hit = 0;
        int ord[MAX_WIN];
        int n = zorder(ord);
        for (int k = n - 1; k >= 0; k--) {  /* od okna na wierzchu w dol */
            int i = ord[k];
            struct window *w = &wins[i];
            /* przycisk zamkniecia [x] */
            if (in_rect(mx, my, close_x(w), close_y(w), CLOSE_SZ, CLOSE_SZ)) {
                w->used = 0;
                hit = 1;
                break;
            }
            /* przycisk akcji w oknie */
            if (w->has_button &&
                in_rect(mx, my, btn_x(w), btn_y(w), BTN_W, BTN_H)) {
                w->counter++;
                focus_idx = i;
                raise_win(i);
                hit = 1;
                break;
            }
            /* pasek tytulu -> rozpocznij przeciaganie (i fokus) */
            if (on_titlebar(w, mx, my)) {
                drag_idx = i;
                drag_dx = mx - w->x;
                drag_dy = my - w->y;
                focus_idx = i;
                raise_win(i);
                hit = 1;
                break;
            }
            /* klik w cialo okna aplikacji -> fokus + zdarzenie do procesu */
            if (in_rect(mx, my, w->x, w->y, w->w, w->h)) {
                focus_idx = i;
                raise_win(i);
                hit = 1;
                int cx = mx - (w->x + 1);
                int cy = my - (w->y + TITLE_H + 1);
                if (cx >= 0 && cx < w->cw && cy >= 0 && cy < w->ch)
                    win_enqueue(w, WEV_CLICK, cx, cy, buttons & 0x07);
                break;            /* pochlon - nie przebijaj do nizszych */
            }
        }
        if (!hit)
            focus_idx = -1;       /* klik w pulpit = utrata fokusu */
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

/* Rysuje pasek zadan na dole i (jesli otwarte) menu uruchamiania */
static void draw_taskbar(void)
{
    int ty = taskbar_y();
    video_fill_rect(0, ty, VIDEO_W, TB_H, COL_BAR);
    video_fill_rect(2, ty + 2, MENU_BW, TB_H - 4, COL_TITLE);
    video_draw_string(6, ty + 3, "Menu", COL_WHITE);

    /* Lista otwartych okien - przycisk na okno; fokus podswietlony */
    int j = 0;
    for (int i = 0; i < MAX_WIN; i++) {
        if (!wins[i].used)
            continue;
        int bx = TBL_X0 + j * TBL_W;
        video_fill_rect(bx, ty, TBL_W - 2, TB_H,
                        i == focus_idx ? COL_TITLE : COL_TBBTN);
        /* tytul przyciety do szerokosci przycisku (6 znakow) */
        char label[7];
        int k = 0;
        for (; wins[i].title[k] && k < 6; k++)
            label[k] = wins[i].title[k];
        label[k] = '\0';
        video_draw_string(bx + 2, ty + 3, label,
                          i == focus_idx ? COL_WHITE : COL_BLACK);
        j++;
    }

    if (menu_open) {
        for (int i = 0; i < MENU_N; i++) {
            int iy = menu_item_y(i);
            video_fill_rect(2, iy, MENU_W, MENU_ROW, COL_WIN);
            video_fill_rect(2, iy, MENU_W, 1, COL_BLACK);
            video_draw_string(6, iy + 2, menu_items[i], COL_BLACK);
        }
    }
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
            if (focus_idx == i)
                focus_idx = -1;
        }
    }
}

/* Handler klawiatury pulpitu: klawisz trafia do okna aplikacji z fokusem.
   Gdy nic nie ma fokusu, przekazujemy klawisz do poprzedniego handlera
   (shell jadra) - dzieki temu komendy ('run ...') dalej dzialaja.
   Biegnie w kontekscie IRQ1. */
static keyboard_char_handler_t prev_key_handler;

static void desktop_on_key(char c)
{
    if (focus_idx >= 0 && focus_idx < MAX_WIN && wins[focus_idx].used &&
        wins[focus_idx].content) {
        win_enqueue(&wins[focus_idx], WEV_KEY, 0, 0, (unsigned char)c);
    } else if (prev_key_handler) {
        prev_key_handler(c);
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
        raise_win(i);              /* nowe okno na wierzchu */
        focus_idx = i;             /* i z fokusem */
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
    video_set_palette(COL_TBBTN, 26, 26, 26); /* ciemnoszary przycisk paska */

    /* Dwa okna jadra (bez wlasciciela, bez bufora treści) */
    wins[0] = (struct window){.x = 40, .y = 60, .w = 120, .h = 70,
                              .used = 1, .has_button = 1, .owner = -1};
    set_title(&wins[0], "Terminal");
    raise_win(0);
    wins[1] = (struct window){.x = 180, .y = 95, .w = 120, .h = 70,
                              .used = 1, .has_button = 0, .owner = -1};
    set_title(&wins[1], "Pliki");
    raise_win(1);

    for (;;) {
        reap_windows();

        int mx, my, buttons;
        mouse_get(&mx, &my, &buttons);
        handle_input(mx, my, buttons);

        video_clear(COL_DESK);

        /* Gorny pasek z tytulem systemu */
        video_fill_rect(0, 0, VIDEO_W, 10, COL_BAR);
        video_draw_string(2, 1, "ProtonOS", COL_BLACK);

        int ord[MAX_WIN];
        int n = zorder(ord);
        for (int k = 0; k < n; k++)      /* od spodu na wierzch (porzadek Z) */
            draw_window(&wins[ord[k]]);

        draw_taskbar();
        draw_cursor(mx, my);

        task_sleep(30);                       /* ~33 klatek/s */
    }
}

void desktop_start(void)
{
    prev_key_handler = keyboard_get_handler();   /* zachowaj shell jadra */
    keyboard_set_handler(desktop_on_key);        /* klawisze -> okno z fokusem */
    task_create("desktop", desktop_thread, (void *)0);
}
