/* The ProtonOS graphical desktop - a simple compositor
 *
 * It runs as a kernel thread: in a loop it redraws the whole frame
 * into the framebuffer (background -> cursor) and gives up the CPU for
 * ~30 ms. At 320x200 a full redraw is 64 KB - cheap enough that we do
 * not bother saving the background under the cursor.
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
#include "include/fs.h"
#include "include/io.h"
#include "include/config.h"

/* Desktop palette indices (set in desktop_thread) */
#define COL_BLACK  0
#define COL_TITLE  3      /* the window title bar (blue) */
#define COL_WHITE  4
#define COL_DESK   6      /* the desktop background (dark blue) */
#define COL_BAR    7      /* the top bar (light grey) */
#define COL_WIN    8      /* the window background (grey) */
#define COL_TBBTN  9      /* a window button on the taskbar (dark grey) */
#define COL_ICON   10     /* a desktop icon (orange) */

#define TITLE_H 10

/* Desktop icons: clicking one launches an application */
#define ICON_W 32
#define ICON_H 24
struct icon {
    int x, y;
    const char *label;
    const char *app;
};
static struct icon icons[] = {
    {8, 20, "App", "guiapp"},
    {8, 60, "Zegar", "gclock"},
    {8, 100, "Pliki", "gfiles"},
    {8, 140, "Monitor", "gmon"},
    {8, 180, "Term", "gterm"},
    {8, 220, "Web", "gweb"},
    {8, 260, "Ustaw", "gsettings"},
};
#define ICON_N ((int)(sizeof(icons) / sizeof(icons[0])))

/* The disk file with the remembered icon positions (x,y pairs in order) */
#define ICON_POS_FILE "icons.pos"

static int icon_drag = -1;      /* the icon being dragged (-1 = none) */
static int icon_dx, icon_dy;    /* the grab: the cursor's offset within the icon */
static int icon_press_x, icon_press_y;   /* the press position (click vs drag) */

/* Reads the icon positions from disk (if the file exists and its size
   matches) */
static void icons_load(void)
{
    if (!fs_ready())
        return;
    int32_t buf[ICON_N * 2];
    if (fs_read(ICON_POS_FILE, buf, sizeof(buf)) < (int)sizeof(buf))
        return;
    for (int k = 0; k < ICON_N; k++) {
        icons[k].x = buf[k * 2];
        icons[k].y = buf[k * 2 + 1];
    }
}

/* Writes the current icon positions to disk */
static void icons_save(void)
{
    if (!fs_ready())
        return;
    int32_t buf[ICON_N * 2];
    for (int k = 0; k < ICON_N; k++) {
        buf[k * 2] = icons[k].x;
        buf[k * 2 + 1] = icons[k].y;
    }
    fs_write(ICON_POS_FILE, buf, sizeof(buf));
}

#define COL_RED    1      /* the close button */

/* An event for an application (layout matching struct win_event in usys.h) */
struct wevent {
    int32_t type;          /* 0 = none, 1 = a mouse click */
    int32_t x, y;          /* coordinates relative to the content area */
    int32_t val;           /* the button mask */
};
#define WEV_NONE  0
#define WEV_CLICK 1
#define WEV_KEY   2
#define WEV_QLEN  8

struct window {
    int x, y, w, h;
    char title[24];
    int used;
    int has_button;        /* does the window have an action button */
    int counter;           /* the button's click counter */
    int owner;             /* the owning process's pid; -1 = a kernel window */
    uint8_t *content;      /* the application's content buffer (NULL = a kernel window) */
    int cw, ch;            /* dimensions of the content area */
    int z;                 /* drawing order (higher = on top) */
    int minimized;         /* is it minimised (hidden, visible on the bar) */
    struct wevent evq[WEV_QLEN];  /* the event queue for the application */
    int ev_head, ev_tail;
};

#define MAX_WIN 6
static struct window wins[MAX_WIN];

/* Drag state: the window index and the grab (the cursor's offset
   within the window) */
static int drag_idx = -1;
static int resize_idx = -1;
static int drag_dx, drag_dy;
static int prev_pressed;    /* the previous left-button state - to detect a click */
static int focus_idx = -1;  /* the window with keyboard focus */

/* The taskbar and the launch menu */
#define TB_H 12                       /* the taskbar height */
#define MENU_BW 40                    /* the width of the "Menu" button */
#define MENU_ROW 12                   /* the height of a menu entry */
#define MENU_W 80                     /* the width of the opened menu */
static int menu_open;
static const char *menu_items[] = {"guiapp", "gclock", "gfiles", "gweb"};
#define MENU_N ((int)(sizeof(menu_items) / sizeof(menu_items[0])))

static int taskbar_y(void) { return video_h() - TB_H; }
static int menu_item_y(int i) { return taskbar_y() - (MENU_N - i) * MENU_ROW; }

/* The list of window buttons on the taskbar (after the Menu button) */
#define TBL_X0 (2 + MENU_BW + 4)
#define TBL_W  52

static int z_top;           /* the highest Z order handed out */

/* Brings a window to the top (the highest Z) */
static void raise_win(int i)
{
    wins[i].z = ++z_top;
}

/* Fills out[] with the slots of used windows in Z order (from the
   bottom); returns their count */
static int zorder(int *out)
{
    int n = 0;
    for (int i = 0; i < MAX_WIN; i++)
        if (wins[i].used)
            out[n++] = i;
    for (int a = 1; a < n; a++) {     /* insertion sort by z */
        int key = out[a], b = a - 1;
        while (b >= 0 && wins[out[b]].z > wins[key].z) {
            out[b + 1] = out[b];
            b--;
        }
        out[b + 1] = key;
    }
    return n;
}

/* Puts an event into a window's queue (protected - called from the
   compositor, the keyboard IRQ and indirectly from a syscall) */
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

#define CLOSE_SZ 8          /* the bar buttons [_] [x] */
#define GRIP_SZ  8          /* the resize grip (bottom right corner) */
#define BTN_W 40
#define BTN_H 12
#define WIN_MIN_W 60
#define WIN_MIN_H 40

static int in_rect(int x, int y, int rx, int ry, int rw, int rh)
{
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static int close_x(const struct window *w) { return w->x + w->w - CLOSE_SZ - 1; }
static int close_y(const struct window *w) { return w->y + 1; }
static int min_x(const struct window *w)   { return w->x + w->w - 2 * CLOSE_SZ - 2; }
static int grip_x(const struct window *w)  { return w->x + w->w - GRIP_SZ; }
static int grip_y(const struct window *w)  { return w->y + w->h - GRIP_SZ; }
static int resizable(const struct window *w) { return w->content == (void *)0; }
static int btn_x(const struct window *w)   { return w->x + 8; }
static int btn_y(const struct window *w)    { return w->y + TITLE_H + 8; }

/* An 8x12 cursor arrow: set bits = cursor pixels.
   The highest bit (0x80) is the leftmost column. */
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
            /* A black outline (neighbouring pixels) for visibility on any
               background, with a white middle on top */
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

/* Draws a decimal number as text */
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

/* --- Drawing into an application's content buffer (palette indices) --- */

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
    if (win->minimized)            /* minimised - visible only on the bar */
        return;
    /* The window background with a black frame */
    video_fill_rect(win->x, win->y, win->w, win->h, COL_WIN);
    video_fill_rect(win->x, win->y, win->w, 1, COL_BLACK);
    video_fill_rect(win->x, win->y + win->h - 1, win->w, 1, COL_BLACK);
    video_fill_rect(win->x, win->y, 1, win->h, COL_BLACK);
    video_fill_rect(win->x + win->w - 1, win->y, 1, win->h, COL_BLACK);
    /* The title bar plus the name */
    video_fill_rect(win->x + 1, win->y + 1, win->w - 2, TITLE_H, COL_TITLE);
    video_draw_string(win->x + 3, win->y + 2, win->title, COL_WHITE);
    /* The minimise button [_] next to the close one */
    video_fill_rect(min_x(win), close_y(win), CLOSE_SZ, CLOSE_SZ, COL_TBBTN);
    video_draw_char(min_x(win), close_y(win), '_', COL_WHITE);
    /* The close button [x] in the right corner of the bar */
    video_fill_rect(close_x(win), close_y(win), CLOSE_SZ, CLOSE_SZ, COL_RED);
    video_draw_char(close_x(win), close_y(win), 'x', COL_WHITE);
    /* The resize grip (bottom right corner) for kernel windows */
    if (resizable(win))
        video_fill_rect(grip_x(win), grip_y(win), GRIP_SZ, GRIP_SZ, COL_BAR);

    /* An optional action button plus a click counter */
    if (win->has_button) {
        video_fill_rect(btn_x(win), btn_y(win), BTN_W, BTN_H, COL_BAR);
        video_draw_string(btn_x(win) + 4, btn_y(win) + 2, "Klik", COL_BLACK);
        draw_int(btn_x(win) + BTN_W + 6, btn_y(win) + 2, win->counter,
                 COL_WHITE);
    }

    /* An application window: compose its content buffer into the work area */
    if (win->content) {
        int bx = win->x + 1;
        int by = win->y + TITLE_H + 1;
        for (int j = 0; j < win->ch; j++)
            for (int i = 0; i < win->cw; i++)
                video_putpixel(bx + i, by + j,
                               win->content[j * win->cw + i]);
    }
}

/* Is the point on a window's title bar (the drag handle) */
static int on_titlebar(const struct window *win, int x, int y)
{
    return x >= win->x && x < win->x + win->w &&
           y >= win->y && y < win->y + TITLE_H;
}

/* Mouse handling: clicks (close, buttons) and window dragging.
   Clicks take precedence over dragging; both are counted on the
   left-button press edge, so that one click = one action. */
static void handle_input(int mx, int my, int buttons)
{
    int pressed = buttons & 0x01;

    if (pressed && !prev_pressed) {     /* a press edge = a click */
        /* The taskbar / launch menu takes precedence (it is on top) */
        if (in_rect(mx, my, 2, taskbar_y() + 2, MENU_BW, TB_H - 4)) {
            menu_open = !menu_open;
            prev_pressed = pressed;
            return;
        }
        if (menu_open) {
            for (int i = 0; i < MENU_N; i++) {
                if (in_rect(mx, my, 2, menu_item_y(i), MENU_W, MENU_ROW)) {
                    exec_spawn(menu_items[i]);   /* launch the application */
                    menu_open = 0;
                    prev_pressed = pressed;
                    return;
                }
            }
            menu_open = 0;                /* a click outside the menu - close it */
        }
        /* Window buttons on the taskbar: focus plus bring to top */
        if (my >= taskbar_y()) {
            int j = 0;
            for (int i = 0; i < MAX_WIN; i++) {
                if (!wins[i].used)
                    continue;
                if (in_rect(mx, my, TBL_X0 + j * TBL_W, taskbar_y(),
                            TBL_W - 2, TB_H)) {
                    wins[i].minimized = 0;   /* restore it if minimised */
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
        for (int k = n - 1; k >= 0; k--) {  /* from the topmost window downwards */
            int i = ord[k];
            struct window *w = &wins[i];
            if (w->minimized)               /* minimised - out of reach */
                continue;
            /* the close button [x] */
            if (in_rect(mx, my, close_x(w), close_y(w), CLOSE_SZ, CLOSE_SZ)) {
                w->used = 0;
                hit = 1;
                break;
            }
            /* the minimise button [_] */
            if (in_rect(mx, my, min_x(w), close_y(w), CLOSE_SZ, CLOSE_SZ)) {
                w->minimized = 1;
                if (focus_idx == i)
                    focus_idx = -1;
                hit = 1;
                break;
            }
            /* the resize grip (bottom right corner) */
            if (resizable(w) &&
                in_rect(mx, my, grip_x(w), grip_y(w), GRIP_SZ, GRIP_SZ)) {
                resize_idx = i;
                focus_idx = i;
                raise_win(i);
                hit = 1;
                break;
            }
            /* an action button inside the window */
            if (w->has_button &&
                in_rect(mx, my, btn_x(w), btn_y(w), BTN_W, BTN_H)) {
                w->counter++;
                focus_idx = i;
                raise_win(i);
                hit = 1;
                break;
            }
            /* the title bar -> start dragging (and focus) */
            if (on_titlebar(w, mx, my)) {
                drag_idx = i;
                drag_dx = mx - w->x;
                drag_dy = my - w->y;
                focus_idx = i;
                raise_win(i);
                hit = 1;
                break;
            }
            /* a click into an application window's body -> focus plus an event
                   to the process */
            if (in_rect(mx, my, w->x, w->y, w->w, w->h)) {
                focus_idx = i;
                raise_win(i);
                hit = 1;
                int cx = mx - (w->x + 1);
                int cy = my - (w->y + TITLE_H + 1);
                if (cx >= 0 && cx < w->cw && cy >= 0 && cy < w->ch)
                    win_enqueue(w, WEV_CLICK, cx, cy, buttons & 0x07);
                break;            /* swallow it - do not fall through to lower ones */
            }
        }
        if (!hit) {
            /* A click on the desktop background: grabbing an icon (launch or
               move is decided on release), otherwise a loss of focus */
            focus_idx = -1;
            for (int k = 0; k < ICON_N; k++)
                if (in_rect(mx, my, icons[k].x, icons[k].y, ICON_W, ICON_H)) {
                    icon_drag = k;
                    icon_dx = mx - icons[k].x;
                    icon_dy = my - icons[k].y;
                    icon_press_x = mx;
                    icon_press_y = my;
                    break;
                }
        }
    }

    if (!pressed) {
        drag_idx = -1;
        resize_idx = -1;
        if (icon_drag >= 0) {       /* releasing an icon: a click or a move? */
            int ddx = mx - icon_press_x, ddy = my - icon_press_y;
            if (ddx * ddx + ddy * ddy < 9)
                exec_spawn(icons[icon_drag].app);   /* barely moved -> a click */
            else
                icons_save();                       /* moved -> save the layout */
            icon_drag = -1;
        }
    }

    if (pressed && icon_drag >= 0) {    /* the icon follows the cursor */
        int nx = mx - icon_dx;
        int ny = my - icon_dy;
        if (nx < 0) nx = 0;
        if (ny < 10) ny = 10;                       /* below the top bar */
        if (nx > video_w() - ICON_W) nx = video_w() - ICON_W;
        if (ny > video_h() - TB_H - ICON_H) ny = video_h() - TB_H - ICON_H;
        icons[icon_drag].x = nx;
        icons[icon_drag].y = ny;
    }

    if (pressed && drag_idx >= 0) {     /* in progress: the window follows the cursor */
        int nx = mx - drag_dx;
        int ny = my - drag_dy;
        if (nx < 0) nx = 0;
        if (ny < TITLE_H) ny = TITLE_H;
        if (nx > video_w() - wins[drag_idx].w) nx = video_w() - wins[drag_idx].w;
        if (ny > video_h() - wins[drag_idx].h) ny = video_h() - wins[drag_idx].h;
        wins[drag_idx].x = nx;
        wins[drag_idx].y = ny;
    }

    if (pressed && resize_idx >= 0) {   /* in progress: the bottom right corner follows the cursor */
        struct window *w = &wins[resize_idx];
        int nw = mx - w->x + 1;
        int nh = my - w->y + 1;
        if (nw < WIN_MIN_W) nw = WIN_MIN_W;
        if (nh < WIN_MIN_H) nh = WIN_MIN_H;
        if (w->x + nw > video_w()) nw = video_w() - w->x;
        if (w->y + nh > video_h()) nh = video_h() - w->y;
        w->w = nw;
        w->h = nh;
    }

    prev_pressed = pressed;
}

/* Draws the taskbar at the bottom and (if open) the launch menu */
/* Draws the icons on the desktop background (a tile plus a caption) */
static void draw_icons(void)
{
    for (int k = 0; k < ICON_N; k++) {
        const struct icon *ic = &icons[k];
        video_fill_rect(ic->x, ic->y, ICON_W, ICON_H, COL_ICON);
        video_fill_rect(ic->x, ic->y, ICON_W, 1, COL_BLACK);
        video_fill_rect(ic->x, ic->y + ICON_H - 1, ICON_W, 1, COL_BLACK);
        video_draw_string(ic->x, ic->y + ICON_H + 2, ic->label, COL_WHITE);
    }
}

static void draw_taskbar(void)
{
    int ty = taskbar_y();
    video_fill_rect(0, ty, video_w(), TB_H, COL_BAR);
    video_fill_rect(2, ty + 2, MENU_BW, TB_H - 4, COL_TITLE);
    video_draw_string(6, ty + 3, "Menu", COL_WHITE);

    /* The list of open windows - one button per window; the focused one is
       highlighted */
    int j = 0;
    for (int i = 0; i < MAX_WIN; i++) {
        if (!wins[i].used)
            continue;
        int bx = TBL_X0 + j * TBL_W;
        video_fill_rect(bx, ty, TBL_W - 2, TB_H,
                        i == focus_idx ? COL_TITLE : COL_TBBTN);
        /* the title trimmed to the button width (6 characters) */
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

/* Copies a title into a window (trimming it) */
static void set_title(struct window *w, const char *t)
{
    int i = 0;
    for (; t[i] && i < (int)sizeof(w->title) - 1; i++)
        w->title[i] = t[i];
    w->title[i] = '\0';
}

/* Releases windows whose owning process is no longer alive */
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

/* The desktop's keyboard handler: a key goes to the focused
   application window. When nothing has focus, we pass the key on to
   the previous handler (the kernel shell) - thanks to that commands
   ('run ...') still work. Runs in IRQ1 context. */
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

/* --- API for application syscalls (syscall context = IF=0) --- */

int desktop_win_create(int owner, int cw, int ch, const char *title)
{
    if (cw <= 0 || ch <= 0 || cw > video_w() || ch > video_h())
        return -1;
    for (int i = 0; i < MAX_WIN; i++) {
        if (wins[i].used)
            continue;
        uint8_t *buf = kmalloc(cw * ch);
        if (!buf)
            return -1;
        memset(buf, COL_WIN, cw * ch);     /* a grey background to start with */
        struct window *w = &wins[i];
        /* new application windows are placed in a cascade */
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
        raise_win(i);              /* a new window on top */
        focus_idx = i;             /* and with focus */
        return i;
    }
    return -1;
}

/* Checks that winid is a window of application owner */
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

/* Takes the next window event into *out. Returns 1 when there was one,
   otherwise 0. */
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
    /* Try the high VESA resolution (640x480x32); when there is no
       framebuffer - fall back to mode 13h (320x200) */
    if (video_init_vesa(640, 480) < 0)
        video_init();
    /* The desktop's background comes from the settings, so it survives
       a restart. A small set of choices rather than a free colour: a
       number that means one of six things can be checked, and 'a red
       component of 41' cannot. */
    static const struct { uint8_t r, g, b; } backgrounds[] = {
        {  0, 32, 40 },      /* 0 - teal, as it always was */
        {  0,  0, 32 },      /* 1 - navy */
        { 20,  0, 30 },      /* 2 - purple */
        {  0, 28,  0 },      /* 3 - green */
        { 40, 20,  0 },      /* 4 - brown */
        { 12, 12, 12 },      /* 5 - grey */
    };
    uint32_t bg = config_get_num("tlo", 0);
    if (bg >= sizeof(backgrounds) / sizeof(backgrounds[0]))
        bg = 0;
    video_set_palette(COL_DESK, backgrounds[bg].r, backgrounds[bg].g,
                      backgrounds[bg].b);
    video_set_palette(COL_BAR, 48, 48, 48);   /* the light grey top bar */
    video_set_palette(COL_WIN, 56, 56, 56);   /* the grey window background */
    video_set_palette(COL_TBBTN, 26, 26, 26); /* the dark grey bar button */
    video_set_palette(COL_ICON, 63, 40, 0);   /* the orange icon */

    icons_load();              /* restore the remembered icon positions from disk */

    /* Two kernel windows (no owner, no content buffer) */
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

        /* The top bar with the system title */
        video_fill_rect(0, 0, video_w(), 10, COL_BAR);
        video_draw_string(2, 1, "ProtonOS", COL_BLACK);

        draw_icons();                         /* icons on the background (below the windows) */

        int ord[MAX_WIN];
        int n = zorder(ord);
        for (int k = 0; k < n; k++)      /* from the bottom up (Z order) */
            draw_window(&wins[ord[k]]);

        draw_taskbar();
        draw_cursor(mx, my);

        video_present();                      /* the finished frame -> the screen */
        task_sleep(30);                       /* ~33 frames/s */
    }
}

void desktop_start(void)
{
    static int running;                          /* one compositor is enough */
    if (running)
        return;
    running = 1;

    prev_key_handler = keyboard_console_handler();  /* the shell of the visible console */
    keyboard_set_override(desktop_on_key);          /* keys -> the focused window */
    task_create("desktop", desktop_thread, (void *)0);
}
