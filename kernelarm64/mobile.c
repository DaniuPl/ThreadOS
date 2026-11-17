/* ThreadOS mobile: the phone's shell
 *
 * The first screens of ThreadOS on a phone, drawn by the PC's own code
 * (kernel/gfx.c, kernel/uitext.c, kernel/mark.c, kernel/aurora.c) on
 * the ARM64 kernel's screen, 360 x 640 logical pixels at two to one:
 *
 *   the lock screen: the desktop's aurora, the time large, the date,
 *   and at the bottom the bar a finger swipes up from to open it
 *
 *   home: a status bar with the time and the mark, and the apps in a
 *   grid of four - a tap opens one
 *
 *   the apps, each on the whole screen: About (what this phone is, as
 *   the kernel found it), Clock (the time to the second) and Draw (a
 *   finger draws); a swipe up from the bottom edge goes home, as on
 *   any phone of the last years
 *
 * Every touch that ends is said on the serial port in logical pixels,
 * and every change of screen: that is how t_arm64mobile holds what the
 * shell understood against what it sent.
 */

#include <stdint.h>
#include "mobile.h"
#include "rtc.h"
#include "thread.h"
#include "touch.h"
#include "../kernel/include/aurora.h"
#include "../kernel/include/gfx.h"
#include "../kernel/include/mark.h"
#include "../kernel/include/pmm.h"
#include "../kernel/include/printk.h"
#include "../kernel/include/string.h"
#include "../kernel/include/uitext.h"
#include "../kernel/include/video.h"

/* Logical pixels; t_arm64mobile reads them */
#define MOB_BAR_H      28       /* the status bar */
#define MOB_TILE       60       /* an app's square */
#define MOB_TILE_R     14
#define MOB_GRID_X     24       /* the first column; columns 84 apart */
#define MOB_GRID_STEP  84
#define MOB_GRID_Y     60       /* the first row's top; rows 100 apart */
#define MOB_ROW_STEP   100
#define MOB_LABEL_PX   12
#define MOB_LOCK_TIME_PX 72
#define MOB_LOCK_TIME_Y  168    /* its baseline */
#define MOB_LOCK_DATE_PX 18
#define MOB_LOCK_DATE_Y  204
#define MOB_HINT_PX    14
#define MOB_HINT_Y     596
#define MOB_PILL_W     120
#define MOB_PILL_Y     622
#define MOB_EDGE       56       /* a swipe home starts this near the bottom */
#define MOB_TITLE_PX   22
#define MOB_TITLE_Y    68
#define MOB_ROW_PX     15
#define MOB_INFO_Y     260      /* About's first line */
#define MOB_INFO_STEP  30

#define WHITE   0xFFFFFF
#define MUTED   0xAEB6CC
#define APP_BG  0x101218
#define ACCENT  0x0A84FF

enum { LOCK, HOME, APP_ABOUT, APP_CLOCK, APP_DRAW };

static const struct app {
    const char *name;
    int screen;
    uint32_t colour;
} apps[] = {
    { "About", APP_ABOUT, 0x2F6BFF },
    { "Clock", APP_CLOCK, 0x1C1C1E },
    { "Draw",  APP_DRAW,  0x30B45A },
};
#define APPS ((int)(sizeof(apps) / sizeof(apps[0])))

static const struct mobile_info *info;
static int lw, lh, screen = LOCK, dirty = 1;
static uint32_t *bg;                       /* the aurora, drawn once */
static struct gfx_surface canvas;          /* Draw's page */
static uint32_t frame_bytes;
static int last_x, last_y;

/* --- text ------------------------------------------------------------- */

static char *put(char *p, const char *s)
{
    while (*s)
        *p++ = *s++;
    *p = 0;
    return p;
}

static char *put_num(char *p, uint32_t v)
{
    char tmp[12];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        *p++ = tmp[--n];
    *p = 0;
    return p;
}

static char *put2(char *p, int v)
{
    *p++ = (char)('0' + v / 10 % 10);
    *p++ = (char)('0' + v % 10);
    *p = 0;
    return p;
}

static void clock_text(char *out, const struct rtc_time *t, int seconds)
{
    char *p = put2(out, t->hour);
    p = put(p, ":");
    p = put2(p, t->min);
    if (seconds) {
        p = put(p, ":");
        put2(p, t->sec);
    }
}

static void date_text(char *out, const struct rtc_time *t)
{
    static const char *const days[] = { "Sunday", "Monday", "Tuesday",
        "Wednesday", "Thursday", "Friday", "Saturday" };
    static const char *const months[] = { "January", "February", "March",
        "April", "May", "June", "July", "August", "September", "October",
        "November", "December" };
    char *p = put(out, days[t->wday % 7]);
    p = put(p, ", ");
    p = put_num(p, (uint32_t)t->day);
    p = put(p, " ");
    put(p, months[(t->month - 1) % 12]);
}

static void centred(int font, int px, int cx, int baseline, const char *s,
                    uint32_t c)
{
    ui_font_text(font, px, cx - ui_font_width(font, px, s) / 2, baseline, s,
                 c);
}

/* --- pieces ------------------------------------------------------------ */

static void pill(uint32_t c)
{
    gfx_rrect((lw - MOB_PILL_W) / 2, MOB_PILL_Y, MOB_PILL_W, 5, 2, c, 230);
}

static void status_bar(uint32_t c)
{
    struct rtc_time t;
    char s[16];
    rtc_split(rtc_seconds(), &t);
    clock_text(s, &t, 0);
    ui_font_text(UI_BOLD, 14, 22, 19, s, c);
    mark_draw((const struct gfx_surface *)0, lw - 22 - 14, 7, 14, c, 255);
}

static void tile_xy(int k, int *x, int *y)
{
    *x = MOB_GRID_X + (k % 4) * MOB_GRID_STEP;
    *y = MOB_GRID_Y + (k / 4) * MOB_ROW_STEP;
}

static void icon(int k, int x, int y)
{
    int cx = x + MOB_TILE / 2, cy = y + MOB_TILE / 2;
    switch (apps[k].screen) {
    case APP_ABOUT:
        mark_draw((const struct gfx_surface *)0, x + 12, y + 12, 36, WHITE,
                  255);
        break;
    case APP_CLOCK:
        gfx_disc16(cx * 16, cy * 16, 21 * 16, WHITE, 255);
        gfx_line16(cx * 16, cy * 16, cx * 16, (cy - 14) * 16, 40, 0x1C1C1E,
                   255);
        gfx_line16(cx * 16, cy * 16, (cx + 10) * 16, (cy + 4) * 16, 40,
                   0x1C1C1E, 255);
        gfx_disc16(cx * 16, cy * 16, 40, 0xFF9F0A, 255);
        break;
    case APP_DRAW:
        gfx_line16((x + 14) * 16, (y + 40) * 16, (x + 26) * 16,
                   (y + 22) * 16, 64, WHITE, 255);
        gfx_line16((x + 26) * 16, (y + 22) * 16, (x + 36) * 16,
                   (y + 38) * 16, 64, WHITE, 255);
        gfx_line16((x + 36) * 16, (y + 38) * 16, (x + 46) * 16,
                   (y + 18) * 16, 64, WHITE, 255);
        break;
    }
}

/* --- screens ------------------------------------------------------------ */

static void with_background(void)
{
    memcpy(video_prow32(0), bg, frame_bytes);
}

static void draw_lock(void)
{
    struct rtc_time t;
    char s[40];
    with_background();
    rtc_split(rtc_seconds(), &t);
    clock_text(s, &t, 0);
    centred(UI_BOLD, MOB_LOCK_TIME_PX, lw / 2, MOB_LOCK_TIME_Y, s, WHITE);
    date_text(s, &t);
    centred(UI_SANS, MOB_LOCK_DATE_PX, lw / 2, MOB_LOCK_DATE_Y, s, WHITE);
    centred(UI_SANS, MOB_HINT_PX, lw / 2, MOB_HINT_Y, "Swipe up to open",
            MUTED);
    pill(WHITE);
}

static void draw_home(void)
{
    with_background();
    status_bar(WHITE);
    for (int k = 0; k < APPS; k++) {
        int x, y;
        tile_xy(k, &x, &y);
        gfx_rrect(x, y, MOB_TILE, MOB_TILE, MOB_TILE_R, apps[k].colour, 255);
        icon(k, x, y);
        centred(UI_SANS, MOB_LABEL_PX, x + MOB_TILE / 2,
                y + MOB_TILE + 18, apps[k].name, WHITE);
    }
    pill(WHITE);
}

static void title(const char *s, uint32_t c)
{
    ui_font_text(UI_BOLD, MOB_TITLE_PX, 22, MOB_TITLE_Y, s, c);
}

static void info_row(int i, const char *label, const char *value)
{
    int y = MOB_INFO_Y + i * MOB_INFO_STEP;
    ui_font_text(UI_SANS, MOB_ROW_PX, 22, y, label, MUTED);
    ui_font_text(UI_SANS, MOB_ROW_PX, lw - 22 - ui_font_width(UI_SANS,
                 MOB_ROW_PX, value), y, value, WHITE);
}

static void draw_about(void)
{
    char s[64], *p;
    gfx_fill(0, 0, lw, lh, APP_BG, 255);
    status_bar(WHITE);
    title("About", WHITE);
    mark_draw((const struct gfx_surface *)0, (lw - 72) / 2, 100, 72, WHITE,
              255);
    centred(UI_BOLD, 24, lw / 2, 210, "ThreadOS", WHITE);
    centred(UI_SANS, 14, lw / 2, 232, "mobile, first stage", MUTED);
    info_row(0, "Processor", info->cpu);
    p = put_num(s, (uint32_t)info->cores);
    info_row(1, "Cores", s);
    p = put_num(s, info->mem_mib);
    put(p, " MiB");
    info_row(2, "Memory", s);
    p = put_num(s, (uint32_t)info->screen_w);
    p = put(p, " x ");
    put_num(p, (uint32_t)info->screen_h);
    info_row(3, "Screen", s);
    info_row(4, "Touch", info->touch ? info->touch : "none");
    pill(WHITE);
}

static void draw_clock(void)
{
    struct rtc_time t;
    char s[40];
    gfx_fill(0, 0, lw, lh, 0x000000, 255);
    status_bar(WHITE);
    title("Clock", WHITE);
    rtc_split(rtc_seconds(), &t);
    clock_text(s, &t, 1);
    centred(UI_BOLD, 64, lw / 2, 330, s, WHITE);
    date_text(s, &t);
    centred(UI_SANS, 18, lw / 2, 366, s, MUTED);
    pill(WHITE);
}

static void clear_canvas(void)
{
    gfx_fill_s(&canvas, 0, 0, lw, lh, WHITE, 255);
}

static void draw_draw(void)
{
    memcpy(video_prow32(0), canvas.px, frame_bytes);
    status_bar(0x1C1C1E);
    title("Draw", 0x1C1C1E);
    ui_font_text(UI_SANS, 16, lw - 22 - ui_font_width(UI_SANS, 16, "Clear"),
                 MOB_TITLE_Y, "Clear", ACCENT);
    pill(0x1C1C1E);
}

static void draw(void)
{
    switch (screen) {
    case LOCK:      draw_lock(); break;
    case HOME:      draw_home(); break;
    case APP_ABOUT: draw_about(); break;
    case APP_CLOCK: draw_clock(); break;
    case APP_DRAW:  draw_draw(); break;
    }
    video_present();
}

/* --- the finger ---------------------------------------------------------- */

static const char *screen_name(int s)
{
    switch (s) {
    case LOCK:      return "lock screen";
    case HOME:      return "home";
    case APP_ABOUT: return "About";
    case APP_CLOCK: return "Clock";
    default:        return "Draw";
    }
}

static void go(int s, const char *why)
{
    screen = s;
    dirty = 1;
    printk("[mobile] %s (%s)\n", screen_name(s), why);
}

static void tap(int x, int y)
{
    if (screen == HOME) {
        for (int k = 0; k < APPS; k++) {
            int tx, ty;
            tile_xy(k, &tx, &ty);
            if (x >= tx && x < tx + MOB_TILE && y >= ty && y < ty + MOB_TILE) {
                if (apps[k].screen == APP_DRAW)
                    clear_canvas();
                go(apps[k].screen, apps[k].name);
                return;
            }
        }
    } else if (screen == APP_DRAW && y < MOB_TITLE_Y + 12 &&
               x > lw - 100) {
        clear_canvas();
        dirty = 1;
        printk("[mobile] Draw cleared\n");
    }
}

static struct { int on, x0, y0; } finger;

static void touch(const struct touch_event *e)
{
    if (e->kind == TOUCH_DOWN) {
        finger.on = 1;
        finger.x0 = e->x;
        finger.y0 = e->y;
        last_x = e->x;
        last_y = e->y;
        printk("[touch] down %d,%d\n", e->x, e->y);
        return;
    }
    if (!finger.on)
        return;
    if (screen == APP_DRAW && finger.y0 > MOB_TITLE_Y + 12 &&
        finger.y0 < lh - MOB_EDGE) {
        gfx_line16_s(&canvas, last_x * 16, last_y * 16, e->x * 16,
                     e->y * 16, 6 * 16, ACCENT, 255);
        dirty = 1;
    }
    last_x = e->x;
    last_y = e->y;
    if (e->kind != TOUCH_UP)
        return;
    finger.on = 0;
    int dx = e->x - finger.x0, dy = e->y - finger.y0;
    printk("[touch] up %d,%d\n", e->x, e->y);
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    if (dy < -80 && adx < ady) {
        /* Up: on the lock screen from anywhere in its lower half, in an
           app only from the bottom edge, as a phone's home gesture */
        if (screen == LOCK && finger.y0 > lh / 2)
            go(HOME, "swipe up");
        else if (screen >= APP_ABOUT && finger.y0 >= lh - MOB_EDGE)
            go(HOME, "swipe up from the edge");
    } else if (adx <= 12 && ady <= 12) {
        tap(finger.x0, finger.y0);
    }
}

void mobile_main(void *arg)
{
    info = arg;
    lw = video_w();
    lh = video_h();
    frame_bytes = (uint32_t)video_pw() * (uint32_t)video_ph() * 4;
    uint32_t pages = (frame_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    bg = (uint32_t *)(uintptr_t)pmm_alloc_contiguous(pages);
    canvas.px = (uint32_t *)(uintptr_t)pmm_alloc_contiguous(pages);
    if (!bg || !canvas.px) {
        printk("[mobile] no memory for the screens\n");
        return;
    }
    canvas.w = video_pw();
    canvas.h = video_ph();
    canvas.stride = video_pw();
    canvas.k = video_k();
    clear_canvas();
    aurora_draw(bg, video_pw(), video_ph(), video_pw(), 1);
    printk("[mobile] %dx%d logical, %dx%d on the panel\n", lw, lh,
           video_pw(), video_ph());
    go(LOCK, "start");

    uint32_t shown = 0;
    for (;;) {
        struct touch_event e;
        while (touch_next(&e))
            touch(&e);
        /* The clock: every second where seconds show, every minute
           everywhere else */
        uint32_t now = rtc_seconds();
        uint32_t step = screen == APP_CLOCK ? now : now / 60;
        if (step != shown) {
            shown = step;
            dirty = 1;
        }
        if (dirty) {
            dirty = 0;
            draw();
        }
        thread_sleep_ms(10);
    }
}
