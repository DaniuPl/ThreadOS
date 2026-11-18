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
 *   programs' windows (winmob.c): a program that makes one is shown in
 *   it at once - once the phone is open - under the status bar, and
 *   the finger in it is the program's; home, its window is a tile
 *   after the apps until the program ends
 *
 *   applications from packages (.tapp, kernel/initrd.c): a tile each
 *   after the phone's own apps, and a tap starts the package's program
 *
 *   the dock at the bottom of home: Notes, Calculator, Settings
 *   (mobapps.c) and the first package's application - a browser, as a
 *   phone's dock has one
 *
 *   the status bar: the time, the network's bars (a card with an
 *   address, one without, none, or airplane mode) and the battery -
 *   what fw_cfg's opt/threados/battery says ("73 discharging"), or
 *   mains power where there is no battery, as on QEMU's virt
 *
 *   the shade, pulled down from the status bar (from anywhere on home):
 *   the time, quick settings - the network, airplane mode, the dark
 *   look, do not disturb - the brightness, and the notifications: the
 *   kernel's (the network up, a package taken, a program stopped by a
 *   fault) and programs' (SYS64_NOTIFY, as on the PC); a new one is a
 *   banner for a few seconds unless do not disturb is on
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
#include "winmob.h"
#include "proc.h"
#include "mobapps.h"
#include "fwcfg.h"
#include "netglue.h"
#include "video64.h"
#include "vsnd.h"
#include "../kernel/include/initrd.h"
#include "../kernel/include/look.h"
#include "../kernel/include/net.h"
#include "../kernel/include/netdev.h"
#include "../kernel/include/spinlock.h"
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
#define MOB_HINT_UP    44       /* from the bottom */
#define MOB_PILL_W     120
#define MOB_PILL_UP    18       /* the home indicator, from the bottom */
#define MOB_EDGE       56       /* a swipe home starts this near the bottom;
                                   a tap there is the program's */
#define MOB_SAFE_UP    20       /* below programs' windows: the home
                                   indicator's own strip */
#define MOB_TITLE_PX   22
#define MOB_TITLE_Y    68
#define MOB_ROW_PX     15
#define MOB_INFO_Y     260      /* About's first line */
#define MOB_INFO_STEP  30
#define MOB_WIN_TILE   0x5E5CE6 /* a program's tile */
#define MOB_PKG_TILE   0xFF9F0A /* an application package's */
#define MOB_STATUS_PX  12       /* the battery's per cent */
#define MOB_BATT_W     24       /* the battery, 12 high, its right edge */
#define MOB_BATT_RIGHT 20       /* this far from the screen's */
#define MOB_BAR_PITCH  5        /* the network's four bars, 3 wide */
#define MOB_DOCK_PCT   85       /* the dock's panel: this much of the
                                   width, centred */
#define MOB_DOCK_UP    112      /* the dock's top, from the bottom */
#define MOB_DOCK_H     80
#define MOB_DOCK_R     30
#define MOB_DOCK_TILE  56
#define MOB_SHADE_TIME_Y 70     /* the shade: the time's baseline */
#define MOB_SHADE_TILE_Y 116    /* its quick settings: two rows of two */
#define MOB_SHADE_TILE_H 64
#define MOB_SHADE_TILE_STEP 76  /* across and down */
#define MOB_SHADE_TILES 6       /* in rows of two */
#define MOB_SHADE_BRIGHT_Y 360  /* the brightness slider's middle */
#define MOB_SHADE_BRIGHT_X 24
#define MOB_SHADE_NOTE_Y 406    /* "Notifications" */
#define MOB_SHADE_CARD_Y 420    /* the first card's top */
#define MOB_SHADE_CARD_H 58
#define MOB_SHADE_CARD_STEP 66
#define MOB_SHADE_CARDS 3
#define MOB_BANNER_Y   34
#define MOB_BANNER_H   64
#define MOB_BANNER_S   4        /* seconds a banner stays */
#define MOB_NOTIFS     8        /* notifications kept */
#define MOB_NAV_UP     36       /* Android's three buttons: their bar,
                                   from the bottom; back, home and recent
                                   apps at a quarter, half and three
                                   quarters of the width */
#define MOB_RECENT_Y   96       /* recent apps: the first card's top */
#define MOB_RECENT_H   64
#define MOB_RECENT_STEP 76
#define MOB_RECENTS    6
#define MOB_LOCK_NOTE_Y 236     /* the lock screen's first notification */

#define WHITE   0xFFFFFF
#define MUTED   0xAEB6CC
#define APP_BG  0x101218
#define ACCENT  0x0A84FF        /* the accent until Settings choose another
                                   (mobapps.c's first) */

enum { LOCK, HOME, APP_ABOUT, APP_CLOCK, APP_DRAW, APP_WINDOW, APP_NOTES,
       APP_CALC, APP_SETTINGS, APP_RECENTS, APP_RECORDER };

static const struct app {
    const char *name;
    int screen;
    uint32_t colour;
} apps[] = {
    { "About", APP_ABOUT, 0x2F6BFF },
    { "Clock", APP_CLOCK, 0x1C1C1E },
    { "Draw",  APP_DRAW,  0x30B45A },
    { "Recorder", APP_RECORDER, 0xFF3B30 },
};
#define APPS ((int)(sizeof(apps) / sizeof(apps[0])))

/* The dock's own apps; the first package's application after them */
static const struct app dock[] = {
    { "Notes",      APP_NOTES,    0xFFD60A },
    { "Calculator", APP_CALC,     0x3A3A3C },
    { "Settings",   APP_SETTINGS, 0x8E8E93 },
};
#define DOCK ((int)(sizeof(dock) / sizeof(dock[0])))

/* Places that follow the screen's logical size - the display size
   (Settings) changes it */
#define MOB_HINT_Y     (lh - MOB_HINT_UP)
#define MOB_PILL_Y     (lh - MOB_PILL_UP)
#define MOB_DOCK_Y     (lh - MOB_DOCK_UP)
#define MOB_DOCK_W     (lw * MOB_DOCK_PCT / 100)
#define MOB_DOCK_X     ((lw - MOB_DOCK_W) / 2)
#define MOB_SHADE_TILE_W ((lw - 36) / 2)
#define MOB_SHADE_BRIGHT_W (lw - 2 * MOB_SHADE_BRIGHT_X)
#define MOB_NAV_Y      (lh - MOB_NAV_UP)
#define MOB_NAV_BACK_X (lw / 4)
#define MOB_NAV_HOME_X (lw / 2)
#define MOB_NAV_RECENT_X (lw * 3 / 4)

static const struct mobile_info *info;
static int lw, lh, screen = LOCK, dirty = 1;
static int shown = -1;          /* the program's window on the screen */
static int pending = -1;        /* one made while the phone was locked */
static uint32_t *bg;                       /* the aurora, drawn once */
static struct gfx_surface canvas;          /* Draw's page */
static uint32_t frame_bytes;
static int last_x, last_y;
static int shade;               /* pulled down over the screen */
static int win_top;             /* programs' windows' top, panel pixels */
static int recent[MOB_RECENTS], recent_count;  /* screens, newest first */
static int about_from_settings;
static int battery = -1, charging;  /* -1: mains power, no battery */

/* Notifications, newest first: any thread may add one */
static struct notif {
    char title[40], body[80];
    uint32_t at;                /* rtc_seconds() */
} notifs[MOB_NOTIFS], banner;
static int notif_count;
static volatile uint32_t notif_seq;
static uint32_t banner_until;
static struct spinlock notif_lock = SPINLOCK_FREE;

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
    char *p = out;
    if (!mob_prefs.clock24) {
        /* 1:05, and with the seconds 1:05:09 PM */
        int h = t->hour % 12 ? t->hour % 12 : 12;
        p = put_num(p, (uint32_t)h);
        p = put(p, ":");
        p = put2(p, t->min);
        if (seconds) {
            p = put(p, ":");
            p = put2(p, t->sec);
            put(p, t->hour < 12 ? " AM" : " PM");
        }
        return;
    }
    p = put2(out, t->hour);
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

/* The bottom of every screen: the iPhone's home indicator, or
   Android's back, home and recent apps */
static void pill(uint32_t c)
{
    if (!mob_prefs.nav_buttons) {
        gfx_rrect((lw - MOB_PILL_W) / 2, MOB_PILL_Y, MOB_PILL_W, 5, 2, c,
                  230);
        return;
    }
    int cy = MOB_NAV_Y + (lh - MOB_NAV_Y) / 2;
    int bx = MOB_NAV_BACK_X, rx = MOB_NAV_RECENT_X;
    gfx_line16((bx + 6) * 16, (cy - 8) * 16, (bx - 7) * 16, cy * 16, 32, c,
               255);
    gfx_line16((bx - 7) * 16, cy * 16, (bx + 6) * 16, (cy + 8) * 16, 32, c,
               255);
    gfx_line16((bx + 6) * 16, (cy - 8) * 16, (bx + 6) * 16, (cy + 8) * 16,
               32, c, 255);
    gfx_circle16(MOB_NAV_HOME_X * 16, cy * 16, 9 * 16, 32, c, 255);
    gfx_rring16((rx - 8) * 16, (cy - 8) * 16, 16 * 16, 16 * 16, 3 * 16, 32,
                c, 255);
}

void mob_pill(uint32_t c) { pill(c); }

void mob_centred(int font, int px, int cx, int baseline, const char *s,
                 uint32_t c)
{
    centred(font, px, cx, baseline, s, c);
}

void mob_set_airplane(int on)
{
    mob_prefs.airplane = on;
    vnet_airplane(on);
    printk("[net] airplane mode %s\n", on ? "on" : "off");
}

/* Battery saver holds the screen to 60%, whatever the slider says */
#define MOB_SAVER_BRIGHT 60

void mob_apply_saver(void)
{
    int pct = mob_prefs.brightness;
    if (mob_prefs.saver && pct > MOB_SAVER_BRIGHT)
        pct = MOB_SAVER_BRIGHT;
    video_set_brightness(pct * 256 / 100);
}

void mobile_sound(int volume, int mute)
{
    if (volume >= 0)
        mob_prefs.volume = volume > 100 ? 100 : volume;
    vsnd_volume(volume, mute);
    printk("[mobile] sound: volume %d%s\n", mob_prefs.volume,
           mute > 0 ? ", muted" : "");
    dirty = 1;
}

void mob_set_brightness(int pct)
{
    if (pct < 10)
        pct = 10;
    if (pct > 100)
        pct = 100;
    mob_prefs.brightness = pct;
    mob_apply_saver();
    printk("[mobile] brightness %d%%\n", pct);
}

/* The wallpapers after the aurora: a slope from the top's colour to
   the bottom's */
static const uint32_t wall_grad[MOB_WALLPAPERS][2] = {
    { 0, 0 },
    { 0x0B3D91, 0x00C6FF },     /* Ocean */
    { 0xFF5E62, 0xFF9966 },     /* Sunset */
    { 0x3A3A3C, 0x000000 },     /* Graphite */
};

static void paint_wallpaper(void)
{
    int w = video_pw(), h = video_ph();
    int i = mob_prefs.wallpaper;
    if (i <= 0 || i >= MOB_WALLPAPERS) {
        aurora_draw(bg, w, h, w, 1);
        return;
    }
    uint32_t a = wall_grad[i][0], b = wall_grad[i][1];
    for (int y = 0; y < h; y++) {
        uint32_t c = 0;
        for (int sh = 0; sh < 24; sh += 8) {
            int ca = (int)((a >> sh) & 0xFF), cb = (int)((b >> sh) & 0xFF);
            c |= (uint32_t)(ca + (cb - ca) * y / (h - 1)) << sh;
        }
        for (int x = 0; x < w; x++)
            bg[y * w + x] = c;
    }
}

void mob_wallpaper_changed(void)
{
    paint_wallpaper();
    dirty = 1;
}

static void clear_canvas(void);

/* The display sizes: panel pixels to sixteen logical ones */
static const int size_k16[MOB_SIZES] = { 32, 28, 24 };

/* Programs' windows end above the home indicator's strip, or above
   Android's buttons */
void mob_windows_fit(void)
{
    int up = mob_prefs.nav_buttons ? MOB_NAV_UP : MOB_SAFE_UP;
    winmob_set_bottom(video_ph() - up * video_k() / 16);
}

/* Turned on its side or upright: the screen's width and height swap,
   and everything laid out on them is laid out again - home, the
   wallpaper, Draw's page, programs' windows (each told its new size) */
void mob_set_orientation(int landscape)
{
    if (video_set_landscape(landscape) < 0)
        return;
    mob_prefs.landscape = video_landscape();
    canvas.w = video_pw();
    canvas.h = video_ph();
    canvas.stride = video_pw();
    winmob_reshape(video_pw(), win_top, video_ph());
    paint_wallpaper();
    mob_set_size(mob_prefs.size);
    printk("[mobile] %s, %dx%d\n", mob_prefs.landscape ? "landscape" :
           "portrait", lw, lh);
}

void mob_set_size(int which)
{
    if (which < 0 || which >= MOB_SIZES)
        which = 0;
    mob_prefs.size = which;
    video_set_scale(size_k16[which]);
    gfx_screen_changed();
    lw = video_w();
    lh = video_h();
    touch_screen(lw, lh);
    mob_windows_fit();
    winmob_rescale();
    /* Draw's page is panel pixels: drawn on at the new size from now */
    canvas.k = video_k();
    clear_canvas();
    dirty = 1;
    printk("[mobile] display size %d%% (%dx%d)\n",
           size_k16[which] * 100 / 16, lw, lh);
}

/* --- notifications --------------------------------------------------------- */

static void copy_text(char *to, const char *from, int room)
{
    int n = 0;
    while (from[n] && n < room - 1)
        n++;
    /* not in the middle of a character */
    while (n > 0 && from[n] && ((unsigned char)from[n] & 0xC0) == 0x80)
        n--;
    memcpy(to, from, (size_t)n);
    to[n] = 0;
}

void mobile_notify(const char *title, const char *body)
{
    irqflags_t f = spin_lock_irq(&notif_lock);
    if (notif_count < MOB_NOTIFS)
        notif_count++;
    for (int i = notif_count - 1; i > 0; i--)
        notifs[i] = notifs[i - 1];
    copy_text(notifs[0].title, title, sizeof(notifs[0].title));
    copy_text(notifs[0].body, body, sizeof(notifs[0].body));
    notifs[0].at = rtc_seconds();
    notif_seq++;
    spin_unlock_irq(&notif_lock, f);
    printk("[notify] %s: %s\n", title, body);
    mob_chime();
}

static void notif_drop(int i)
{
    irqflags_t f = spin_lock_irq(&notif_lock);
    if (i >= 0 && i < notif_count) {
        for (int k = i; k < notif_count - 1; k++)
            notifs[k] = notifs[k + 1];
        notif_count--;
    }
    spin_unlock_irq(&notif_lock, f);
}

int mob_battery(int *ch)
{
    *ch = charging;
    return battery;
}

int mob_network(char *addr, int len)
{
    if (len > 0)
        addr[0] = 0;
    if (mob_prefs.airplane)
        return 3;
    if (!netdev_present())
        return 0;
    if (!net_configured())
        return 1;
    uint32_t ip = net_ip();
    char s[20], *p = s;
    for (int i = 3; i >= 0; i--) {
        p = put_num(p, (ip >> (i * 8)) & 0xFF);
        if (i)
            p = put(p, ".");
    }
    if ((int)(p - s) < len)
        memcpy(addr, s, (size_t)(p - s) + 1);
    return 2;
}

/* The network's bars, or a plane in airplane mode: x is their left */
static void network_icon(int x, uint32_t c)
{
    char addr[20];
    int st = mob_network(addr, sizeof(addr));
    if (st == 3) {
        /* the fuselage, the wings, the tail */
        gfx_line16(x * 16, 14 * 16, (x + 16) * 16, 14 * 16, 40, c, 255);
        gfx_line16((x + 9) * 16, 14 * 16, (x + 5) * 16, 6 * 16, 40, c, 255);
        gfx_line16((x + 9) * 16, 14 * 16, (x + 5) * 16, 22 * 16, 40, c, 255);
        gfx_line16((x + 2) * 16, 14 * 16, x * 16, 10 * 16, 32, c, 255);
        gfx_line16((x + 2) * 16, 14 * 16, x * 16, 18 * 16, 32, c, 255);
        return;
    }
    int lit = st == 2 ? 4 : st == 1 ? 1 : 0;
    for (int i = 0; i < 4; i++) {
        int h = 4 + i * 2;
        gfx_fill(x + i * MOB_BAR_PITCH, 19 - h, 3, h, c, i < lit ? 255 : 80);
    }
}

static void status_bar(uint32_t c)
{
    struct rtc_time t;
    char s[16];
    rtc_split(rtc_seconds(), &t);
    clock_text(s, &t, 0);
    ui_font_text(UI_BOLD, 14, 22, 19, s, c);

    /* The battery: its outline, its nub, as full as it is */
    int bx = lw - MOB_BATT_RIGHT - MOB_BATT_W, by = 8;
    int pct = battery < 0 ? 100 : battery;
    gfx_fill(bx, by, MOB_BATT_W, 1, c, 255);
    gfx_fill(bx, by + 11, MOB_BATT_W, 1, c, 255);
    gfx_fill(bx, by, 1, 12, c, 255);
    gfx_fill(bx + MOB_BATT_W - 1, by, 1, 12, c, 255);
    gfx_fill(bx + MOB_BATT_W + 1, by + 4, 2, 4, c, 255);
    uint32_t fill = mob_prefs.saver ? 0xFFD60A :
                    battery >= 0 && battery <= 20 && !charging ? 0xFF453A :
                    (battery < 0 || charging) ? 0x30D158 : c;
    gfx_fill(bx + 2, by + 2, (MOB_BATT_W - 4) * pct / 100, 8, fill, 255);
    char *p = put_num(s, (uint32_t)pct);
    put(p, "%");
    if (!mob_prefs.show_pct)
        s[0] = 0;
    int tw = s[0] ? ui_font_width(UI_SANS, MOB_STATUS_PX, s) : 0;
    int tx = bx - 5 - tw;
    if (battery < 0 || charging) {
        /* a bolt: on mains power, or charging */
        int zx = tx - 9;
        gfx_line16((zx + 5) * 16, 7 * 16, (zx + 1) * 16, 14 * 16, 28, c, 255);
        gfx_line16((zx + 1) * 16, 14 * 16, (zx + 6) * 16, 14 * 16, 28, c,
                   255);
        gfx_line16((zx + 6) * 16, 14 * 16, (zx + 2) * 16, 21 * 16, 28, c,
                   255);
        ui_font_text(UI_SANS, MOB_STATUS_PX, tx, 19, s, c);
        tx = zx;
    } else {
        ui_font_text(UI_SANS, MOB_STATUS_PX, tx, 19, s, c);
    }
    int nx = tx - 8 - 4 * MOB_BAR_PITCH;
    network_icon(nx, c);
    if (vsnd_mic_who() >= 0) {
        /* the microphone records: an orange dot, as the PC's bar has */
        nx -= 12;
        gfx_disc16((nx + 3) * 16, 14 * 16, 4 * 16, MIC_DOT_RGB, 255);
    }
    if (mob_prefs.silent) {
        /* a bell, crossed out */
        int x = nx - 20;
        gfx_rrect(x + 2, 8, 10, 10, 5, c, 255);
        gfx_fill(x, 15, 14, 3, c, 255);
        gfx_disc16((x + 7) * 16, 20 * 16, 2 * 16, c, 255);
        gfx_line16(x * 16, 6 * 16, (x + 15) * 16, 21 * 16, 28, 0xFF453A,
                   255);
    }
}

void mob_status_bar(uint32_t c) { status_bar(c); }

/* An app's square's corners: rounded, or round whole (Settings) */
static int tile_r(int side)
{
    return mob_prefs.round_icons ? side / 2 : MOB_TILE_R * side / MOB_TILE;
}

/* As many columns as the width takes: four at the largest size */
static int grid_cols(void)
{
    return (lw - MOB_GRID_X + (MOB_GRID_STEP - MOB_TILE)) / MOB_GRID_STEP;
}

static void tile_xy(int k, int *x, int *y)
{
    *x = MOB_GRID_X + (k % grid_cols()) * MOB_GRID_STEP;
    *y = MOB_GRID_Y + (k / grid_cols()) * MOB_ROW_STEP;
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
    case APP_RECORDER: {
        /* a sound's wave, as bars */
        static const int bar[7] = { 8, 18, 30, 22, 34, 16, 10 };
        for (int i = 0; i < 7; i++)
            gfx_rrect(cx - 19 + i * 6, cy - bar[i] / 2, 3, bar[i], 1, WHITE,
                      255);
        break;
    }
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

/* --- glass ------------------------------------------------------------------
 *
 * With the iPhone's gestures the panels - the dock, banners, the shade's
 * tiles, the lock screen's notifications - are matte (a tint over what
 * is under them) or Liquid Glass, as an iPhone's since iOS 26: what is
 * under the panel shows through clear, a little brighter, and bent at
 * its rim as through the edge of a lens; a bright rim lit from above
 * and a soft shadow below make it a thing in front.
 */

#define GLASS_LENS     14       /* the rim that bends, logical pixels */
#define GLASS_LIFT     16       /* brighter by 16/256 of what is left */
#define GLASS_MAX      (720 * 260)      /* panel pixels copied at most */

static uint32_t *glass_px;

static int glassy(void)
{
    return mob_prefs.theme == MOB_THEME_GLASS;
}

static int isqrt(int v)
{
    int r = 0;
    while ((r + 1) * (r + 1) <= v)
        r++;
    return r;
}

static uint32_t lift(uint32_t c)
{
    uint32_t out = 0;
    for (int sh = 0; sh < 24; sh += 8) {
        uint32_t v = (c >> sh) & 0xFF;
        out |= (v + ((255 - v) * GLASS_LIFT >> 8)) << sh;
    }
    return out;
}

static void liquid_glass(int x, int y, int w, int h, int r)
{
    int k = video_k();
    int px = x * k / 16, py = y * k / 16, pw = w * k / 16, ph = h * k / 16;
    int pr = r * k / 16, lens = GLASS_LENS * k / 16;
    if (!glass_px)
        glass_px = (uint32_t *)(uintptr_t)pmm_alloc_contiguous(
            (GLASS_MAX * 4 + PAGE_SIZE - 1) / PAGE_SIZE);
    if (!glass_px || pw * ph > GLASS_MAX || px < 0 || py < 0 ||
        px + pw > video_pw() || py + ph > video_ph()) {
        gfx_rrect(x, y, w, h, r, WHITE, 60);
        return;
    }
    /* what is under it, before the shadow - which darkens the bands
       inside the shape too, counting on the shape to cover them */
    for (int j = 0; j < ph; j++)
        memcpy(glass_px + j * pw, video_prow32(py + j) + px,
               (size_t)pw * 4);
    gfx_shadow(x, y, w, h, r, 10, 60, 3);
    for (int j = 0; j < ph; j++) {
        uint32_t *row = video_prow32(py + j) + px;
        for (int i = 0; i < pw; i++) {
            /* how far inside the rounded rectangle, and which way in */
            int cx = i < pr ? pr - i : i > pw - 1 - pr ? i - (pw - 1 - pr)
                                                       : 0;
            int cy = j < pr ? pr - j : j > ph - 1 - pr ? j - (ph - 1 - pr)
                                                       : 0;
            int d, nx = 0, ny = 0;
            if (cx && cy) {
                int len = isqrt(cx * cx + cy * cy);
                d = pr - len;
                if (d < 0 || !len)
                    continue;               /* outside the corner */
                nx = (i < pr ? 1 : -1) * cx * 256 / len;
                ny = (j < pr ? 1 : -1) * cy * 256 / len;
            } else {
                int left = i, right = pw - 1 - i, top = j, bottom = ph - 1 - j;
                d = left;
                nx = 256;
                if (right < d) { d = right; nx = -256; }
                if (top < d) { d = top; nx = 0; ny = 256; }
                if (bottom < d) { d = bottom; nx = 0; ny = -256; }
            }
            int sx = i, sy = j;
            if (d < lens) {
                /* the lens: nearer the rim, what is shown comes from
                   further in - the picture bends there */
                int t = lens - d, shift = t * t / lens;
                sx += shift * nx / 256;
                sy += shift * ny / 256;
                if (sx < 0) sx = 0;
                if (sx >= pw) sx = pw - 1;
                if (sy < 0) sy = 0;
                if (sy >= ph) sy = ph - 1;
            }
            row[i] = lift(glass_px[sy * pw + sx]);
        }
    }
    /* the rim, and the light on it: brighter along the top's left, and
       the bottom's right */
    gfx_rring16(x * 16, y * 16, w * 16, h * 16, r * 16, 20, WHITE, 90);
    gfx_line16((x + r) * 16, (y + 1) * 16 + 8, (x + w * 45 / 100) * 16,
               (y + 1) * 16 + 8, 22, WHITE, 150);
    gfx_line16((x + w * 60 / 100) * 16, (y + h - 2) * 16 + 8,
               (x + w - r) * 16, (y + h - 2) * 16 + 8, 20, WHITE, 70);
}

/* A panel in the theme Settings choose: a tint, glass, or a surface */
static void panel(int x, int y, int w, int h, int r, uint32_t c, int a)
{
    if (glassy())
        liquid_glass(x, y, w, h, r);
    else if (mob_prefs.theme == MOB_THEME_SOLID)
        gfx_rrect(x, y, w, h, r, 0x2C2C2E, 255);
    else
        gfx_rrect(x, y, w, h, r, c, a);
}

/* What lies under the pulled-down shade, blurred - Liquid Glass's
   backdrop: the screen averaged in blocks of BLUR_BLOCK panel pixels,
   those smoothed, and drawn back between them; darkened a little so
   that white reads on it */
#define BLUR_BLOCK 8
static uint32_t *blur_small, *blur_tmp;

static void backdrop_blur(void)
{
    int w = video_pw(), h = video_ph();
    int sw = (w + BLUR_BLOCK - 1) / BLUR_BLOCK, sh = (h + BLUR_BLOCK - 1) /
             BLUR_BLOCK;
    uint32_t n = (uint32_t)(sw * sh * 4);
    if (!blur_small) {
        blur_small = (uint32_t *)(uintptr_t)pmm_alloc_contiguous(
            (n + PAGE_SIZE - 1) / PAGE_SIZE);
        blur_tmp = (uint32_t *)(uintptr_t)pmm_alloc_contiguous(
            (n + PAGE_SIZE - 1) / PAGE_SIZE);
    }
    if (!blur_small || !blur_tmp) {
        gfx_fill(0, 0, lw, lh, 0x000000, 190);
        return;
    }
    for (int by = 0; by < sh; by++)
        for (int bx = 0; bx < sw; bx++) {
            uint32_t r = 0, g = 0, b = 0, c = 0;
            for (int y = by * BLUR_BLOCK; y < (by + 1) * BLUR_BLOCK && y < h;
                 y++) {
                const uint32_t *row = video_prow32(y);
                for (int x = bx * BLUR_BLOCK;
                     x < (bx + 1) * BLUR_BLOCK && x < w; x++) {
                    r += (row[x] >> 16) & 0xFF;
                    g += (row[x] >> 8) & 0xFF;
                    b += row[x] & 0xFF;
                    c++;
                }
            }
            blur_small[by * sw + bx] = ((r / c) << 16) | ((g / c) << 8) |
                                       (b / c);
        }
    /* two passes of a box two blocks wide, across and down */
    for (int pass = 0; pass < 2; pass++) {
        uint32_t *in = pass ? blur_tmp : blur_small;
        uint32_t *out = pass ? blur_small : blur_tmp;
        for (int y = 0; y < sh; y++)
            for (int x = 0; x < sw; x++) {
                uint32_t r = 0, g = 0, b = 0, c = 0;
                for (int d = -2; d <= 2; d++) {
                    int xx = pass ? x : x + d, yy = pass ? y + d : y;
                    if (xx < 0 || xx >= sw || yy < 0 || yy >= sh)
                        continue;
                    uint32_t v = in[yy * sw + xx];
                    r += (v >> 16) & 0xFF;
                    g += (v >> 8) & 0xFF;
                    b += v & 0xFF;
                    c++;
                }
                out[y * sw + x] = ((r / c) << 16) | ((g / c) << 8) | (b / c);
            }
    }
    for (int y = 0; y < h; y++) {
        uint32_t *row = video_prow32(y);
        int fy = (y * 256 + 128) / BLUR_BLOCK - 128;
        int y0 = fy < 0 ? 0 : fy >> 8, ty = fy < 0 ? 0 : fy & 0xFF;
        int y1 = y0 + 1 < sh ? y0 + 1 : y0;
        for (int x = 0; x < w; x++) {
            int fx = (x * 256 + 128) / BLUR_BLOCK - 128;
            int x0 = fx < 0 ? 0 : fx >> 8, tx = fx < 0 ? 0 : fx & 0xFF;
            int x1 = x0 + 1 < sw ? x0 + 1 : x0;
            uint32_t a = blur_small[y0 * sw + x0], b = blur_small[y0 * sw + x1];
            uint32_t c = blur_small[y1 * sw + x0], d = blur_small[y1 * sw + x1];
            uint32_t out = 0;
            for (int sh8 = 0; sh8 < 24; sh8 += 8) {
                int top = (int)((a >> sh8) & 0xFF) * (256 - tx) +
                          (int)((b >> sh8) & 0xFF) * tx;
                int bot = (int)((c >> sh8) & 0xFF) * (256 - tx) +
                          (int)((d >> sh8) & 0xFF) * tx;
                int v = (top * (256 - ty) + bot * ty) >> 16;
                out |= (uint32_t)(v * 3 / 4) << sh8;   /* darkened */
            }
            row[x] = out;
        }
    }
}

/* The dock's places: its own apps, then the first package's - centred
   when there are fewer than four */
static int dock_count(void)
{
    return DOCK + (package_count() > 0 ? 1 : 0);
}

/* The icons evenly apart, the ends' room the same as between them */
static void dock_xy(int i, int *x, int *y)
{
    int n = dock_count();
    int gap = (MOB_DOCK_W - n * MOB_DOCK_TILE) / (n + 1);
    *x = MOB_DOCK_X + gap + i * (MOB_DOCK_TILE + gap);
    *y = MOB_DOCK_Y + (MOB_DOCK_H - MOB_DOCK_TILE) / 2;
}

static void dock_icon(int i, int x, int y)
{
    int t = MOB_DOCK_TILE, cx = x + t / 2, cy = y + t / 2;
    if (i >= DOCK) {
        const struct package *p = package_at(0);
        char letter[2] = { p->name[0] ? p->name[0] : '?', 0 };
        gfx_rrect(x, y, t, t, tile_r(t), MOB_PKG_TILE, 255);
        centred(UI_BOLD, 28, cx, y + 38, letter, WHITE);
        return;
    }
    gfx_rrect(x, y, t, t, tile_r(t), dock[i].colour, 255);
    switch (dock[i].screen) {
    case APP_NOTES:
        /* a page's lines */
        for (int k = 0; k < 3; k++)
            gfx_fill(x + 12, y + 18 + k * 9, k == 2 ? 20 : 32, 3, WHITE, 255);
        break;
    case APP_CALC:
        gfx_disc16((x + 39) * 16, (y + 17) * 16, 8 * 16, 0xFF9F0A, 255);
        centred(UI_BOLD, 22, x + 18, y + 25, "+", WHITE);
        gfx_fill(x + 12, y + 39, 12, 3, WHITE, 255);   /* minus */
        centred(UI_BOLD, 22, x + 39, y + 46, "=", WHITE);
        break;
    case APP_SETTINGS:
        /* a gear: its teeth, its ring, its hole */
        for (int k = 0; k < 4; k++) {
            int dx = (k == 0 ? 16 : k == 2 ? -16 : k == 1 ? 11 : -11);
            int dy = (k == 0 ? 0 : k == 2 ? 0 : k == 1 ? 11 : 11);
            gfx_line16((cx - dx) * 16, (cy - dy) * 16, (cx + dx) * 16,
                       (cy + dy) * 16, 7 * 16, 0x3A3A3C, 255);
            gfx_line16((cx - dy) * 16, (cy + dx) * 16, (cx + dy) * 16,
                       (cy - dx) * 16, 7 * 16, 0x3A3A3C, 255);
        }
        gfx_disc16(cx * 16, cy * 16, 14 * 16, 0x3A3A3C, 255);
        gfx_disc16(cx * 16, cy * 16, 6 * 16, dock[i].colour, 255);
        break;
    }
}

static void draw_dock(void)
{
    panel(MOB_DOCK_X, MOB_DOCK_Y, MOB_DOCK_W, MOB_DOCK_H, MOB_DOCK_R, WHITE,
          60);
    for (int i = 0; i < dock_count(); i++) {
        int x, y;
        dock_xy(i, &x, &y);
        dock_icon(i, x, y);
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
    /* The latest notifications, unless Settings keep them off it */
    if (mob_prefs.lock_notes) {
        irqflags_t f = spin_lock_irq(&notif_lock);
        int n = notif_count < 2 ? notif_count : 2;
        struct notif copy[2];
        for (int i = 0; i < n; i++)
            copy[i] = notifs[i];
        spin_unlock_irq(&notif_lock, f);
        for (int i = 0; i < n; i++) {
            int y = MOB_LOCK_NOTE_Y + i * MOB_SHADE_CARD_STEP;
            if (glassy())
                liquid_glass(12, y, lw - 24, MOB_SHADE_CARD_H, 14);
            else
                gfx_rrect(12, y, lw - 24, MOB_SHADE_CARD_H, 14, 0x1C1C24,
                          255);
            ui_font_text(UI_BOLD, 15, 28, y + 24, copy[i].title, WHITE);
            ui_font_text(UI_SANS, 13, 28, y + 44, copy[i].body, MUTED);
        }
    }
    centred(UI_SANS, MOB_HINT_PX, lw / 2, MOB_HINT_Y, "Swipe up to open",
            MUTED);
    if (mob_prefs.nav_buttons)
        return;                 /* no bar of buttons on a locked phone */
    pill(WHITE);
}

static void draw_home(void)
{
    with_background();
    status_bar(WHITE);
    for (int k = 0; k < APPS; k++) {
        int x, y;
        tile_xy(k, &x, &y);
        gfx_rrect(x, y, MOB_TILE, MOB_TILE, tile_r(MOB_TILE), apps[k].colour,
                  255);
        icon(k, x, y);
        centred(UI_SANS, MOB_LABEL_PX, x + MOB_TILE / 2,
                y + MOB_TILE + 18, apps[k].name, WHITE);
    }
    /* The packages' applications, then the programs' windows: each a
       tile with its name's first letter */
    for (int i = 0; i < package_count(); i++) {
        const struct package *p = package_at(i);
        char letter[2] = { p->name[0] ? p->name[0] : '?', 0 };
        int x, y;
        tile_xy(APPS + i, &x, &y);
        gfx_rrect(x, y, MOB_TILE, MOB_TILE, tile_r(MOB_TILE), MOB_PKG_TILE, 255);
        centred(UI_BOLD, 30, x + MOB_TILE / 2, y + 41, letter, WHITE);
        centred(UI_SANS, MOB_LABEL_PX, x + MOB_TILE / 2,
                y + MOB_TILE + 18, p->name, WHITE);
    }
    for (int i = 0; i < winmob_count(); i++) {
        int x, y, win = winmob_nth(i);
        char name[32], letter[2] = { 0, 0 };
        if (win < 0)
            break;
        winmob_name(win, name, sizeof(name));
        letter[0] = name[0] ? name[0] : '?';
        tile_xy(APPS + package_count() + i, &x, &y);
        gfx_rrect(x, y, MOB_TILE, MOB_TILE, tile_r(MOB_TILE), MOB_WIN_TILE, 255);
        centred(UI_BOLD, 30, x + MOB_TILE / 2, y + 41, letter, WHITE);
        centred(UI_SANS, MOB_LABEL_PX, x + MOB_TILE / 2,
                y + MOB_TILE + 18, name, WHITE);
    }
    draw_dock();
    pill(WHITE);
}

static void title(const char *s, uint32_t c)
{
    ui_font_text(UI_BOLD, MOB_TITLE_PX, 22, MOB_TITLE_Y, s, c);
}

void mob_title(const char *s, uint32_t c) { title(s, c); }

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
                 MOB_TITLE_Y, "Clear", mob_accent());
    pill(0x1C1C1E);
}

/* The program's pixels below, the phone's status bar above */
static void draw_window(void)
{
    gfx_fill(0, 0, lw, (win_top * 16 + video_k() - 1) / video_k(), 0x000000,
             255);
    status_bar(WHITE);
    winmob_draw(shown);
    /* the program waits for typing: the keyboard over its bottom */
    if (winmob_keyboard(shown))
        mob_kb_draw();
    pill(0x8E8E93);
}

static void type_into_window(const char *s)
{
    winmob_type(shown, s);
}

static const char *screen_name(int s);

/* Android's recent apps: a card each, newest first */
static uint32_t app_colour(int scr)
{
    for (int k = 0; k < APPS; k++)
        if (apps[k].screen == scr)
            return apps[k].colour;
    for (int k = 0; k < DOCK; k++)
        if (dock[k].screen == scr)
            return dock[k].colour;
    return MOB_WIN_TILE;
}

static void draw_recents(void)
{
    gfx_fill(0, 0, lw, lh, 0x1C1C1E, 255);
    status_bar(WHITE);
    title("Recent apps", WHITE);
    if (!recent_count)
        centred(UI_SANS, 15, lw / 2, MOB_RECENT_Y + 40, "Nothing yet", MUTED);
    for (int i = 0; i < recent_count; i++) {
        int y = MOB_RECENT_Y + i * MOB_RECENT_STEP;
        gfx_rrect(12, y, lw - 24, MOB_RECENT_H, 16, 0x2C2C2E, 255);
        gfx_rrect(24, y + 12, 40, 40, tile_r(40), app_colour(recent[i]), 255);
        ui_font_text(UI_BOLD, 17, 80, y + 39, screen_name(recent[i]), WHITE);
    }
    pill(WHITE);
}

/* --- the shade and banners ------------------------------------------------ */

static const char *const shade_tiles[MOB_SHADE_TILES] = {
    "Network", "Airplane mode", "Dark mode", "Do not disturb", "Flashlight",
    "Battery saver"
};

static void shade_tile_xy(int i, int *x, int *y)
{
    *x = 12 + (i % 2) * (MOB_SHADE_TILE_W + 12);
    *y = MOB_SHADE_TILE_Y + (i / 2) * MOB_SHADE_TILE_STEP;
}

static void when_text(char *out, uint32_t at)
{
    uint32_t now = rtc_seconds();
    if (now - at < 60) {
        put(out, "now");
        return;
    }
    struct rtc_time t;
    rtc_split(at, &t);
    clock_text(out, &t, 0);
}

static void draw_shade(void)
{
    if (glassy())
        backdrop_blur();
    else if (mob_prefs.theme == MOB_THEME_SOLID)
        gfx_fill(0, 0, lw, lh, 0x101218, 255);
    else
        gfx_fill(0, 0, lw, lh, 0x000000, 190);
    status_bar(WHITE);
    struct rtc_time t;
    char s[80];
    rtc_split(rtc_seconds(), &t);
    clock_text(s, &t, 0);
    ui_font_text(UI_BOLD, 40, 16, MOB_SHADE_TIME_Y, s, WHITE);
    date_text(s, &t);
    ui_font_text(UI_SANS, 15, 16, MOB_SHADE_TIME_Y + 26, s, MUTED);

    for (int i = 0; i < MOB_SHADE_TILES; i++) {
        int x, y, on = 0;
        char addr[20];
        const char *state = 0;
        shade_tile_xy(i, &x, &y);
        switch (i) {
        case 0: {
            int st = mob_network(addr, sizeof(addr));
            on = st == 2;
            state = st == 2 ? addr : st == 3 ? "Off" : st == 1 ?
                    "Connecting" : "No network";
            break;
        }
        case 1: on = mob_prefs.airplane; break;
        case 2: on = mob_prefs.dark; break;
        case 3: on = mob_prefs.dnd; break;
        case 4: on = mob_prefs.flashlight; break;
        case 5: on = mob_prefs.saver; break;
        }
        if (!state)
            state = on ? "On" : "Off";
        if (on)
            gfx_rrect(x, y, MOB_SHADE_TILE_W, MOB_SHADE_TILE_H, 18,
                      mob_accent(), 240);
        else
            panel(x, y, MOB_SHADE_TILE_W, MOB_SHADE_TILE_H, 18, 0x2C2C2E,
                  240);
        ui_font_text(UI_BOLD, 15, x + 16, y + 28, shade_tiles[i], WHITE);
        ui_font_text(UI_SANS, 13, x + 16, y + 48, state,
                     on ? WHITE : MUTED);
    }

    int bx = MOB_SHADE_BRIGHT_X, bw = MOB_SHADE_BRIGHT_W;
    int fill = bw * mob_prefs.brightness / 100;
    panel(bx, MOB_SHADE_BRIGHT_Y - 16, bw, 32, 16, 0x2C2C2E, 240);
    gfx_rrect(bx, MOB_SHADE_BRIGHT_Y - 16, fill, 32, 16, WHITE, 230);
    /* a sun at the slider's start */
    gfx_disc16((bx + 18) * 16, MOB_SHADE_BRIGHT_Y * 16, 5 * 16, 0x8E8E93,
               255);

    ui_font_text(UI_BOLD, 15, 16, MOB_SHADE_NOTE_Y, "Notifications", WHITE);
    irqflags_t f = spin_lock_irq(&notif_lock);
    int n = notif_count;
    struct notif copy[MOB_SHADE_CARDS];
    for (int i = 0; i < n && i < MOB_SHADE_CARDS; i++)
        copy[i] = notifs[i];
    spin_unlock_irq(&notif_lock, f);
    if (n) {
        ui_font_text(UI_SANS, 15, lw - 16 - ui_font_width(UI_SANS, 15,
                     "Clear"), MOB_SHADE_NOTE_Y, "Clear", mob_accent());
    } else {
        centred(UI_SANS, 15, lw / 2, MOB_SHADE_CARD_Y + 34,
                "No notifications", MUTED);
    }
    for (int i = 0; i < n && i < MOB_SHADE_CARDS; i++) {
        int y = MOB_SHADE_CARD_Y + i * MOB_SHADE_CARD_STEP;
        panel(12, y, lw - 24, MOB_SHADE_CARD_H, 14, 0x2C2C2E, 240);
        ui_font_text(UI_BOLD, 15, 28, y + 24, copy[i].title, WHITE);
        ui_font_text(UI_SANS, 13, 28, y + 44, copy[i].body, MUTED);
        when_text(s, copy[i].at);
        ui_font_text(UI_SANS, 12, lw - 28 - ui_font_width(UI_SANS, 12, s),
                     y + 24, s, MUTED);
    }
    pill(WHITE);
}

static void draw_banner(void)
{
    panel(10, MOB_BANNER_Y, lw - 20, MOB_BANNER_H, 16, 0x2C2C2E, 245);
    ui_font_text(UI_BOLD, 15, 26, MOB_BANNER_Y + 26, banner.title, WHITE);
    ui_font_text(UI_SANS, 13, 26, MOB_BANNER_Y + 48, banner.body, MUTED);
}

static void draw(void)
{
    switch (screen) {
    case LOCK:         draw_lock(); break;
    case HOME:         draw_home(); break;
    case APP_ABOUT:    draw_about(); break;
    case APP_CLOCK:    draw_clock(); break;
    case APP_DRAW:     draw_draw(); break;
    case APP_WINDOW:   draw_window(); break;
    case APP_NOTES:    notes_draw(); break;
    case APP_CALC:     calc_draw(); break;
    case APP_SETTINGS: settings_draw(); break;
    case APP_RECENTS:  draw_recents(); break;
    case APP_RECORDER: recorder_draw(); break;
    }
    if (shade)
        draw_shade();
    else if (banner_until && rtc_seconds() < banner_until)
        draw_banner();
    video_present();
}

/* --- the finger ---------------------------------------------------------- */

static const char *screen_name(int s)
{
    switch (s) {
    case LOCK:       return "lock screen";
    case HOME:       return "home";
    case APP_ABOUT:  return "About";
    case APP_CLOCK:  return "Clock";
    case APP_WINDOW: return "window";
    case APP_NOTES:  return "Notes";
    case APP_CALC:   return "Calculator";
    case APP_SETTINGS: return "Settings";
    case APP_RECENTS: return "recent apps";
    case APP_RECORDER: return "Recorder";
    default:         return "Draw";
    }
}

static void go(int s, const char *why)
{
    screen = s;
    dirty = 1;
    if (s == APP_SETTINGS)
        settings_reset();
    /* The phone's own apps, newest first, for Android's recent apps */
    if (s >= APP_ABOUT && s != APP_WINDOW && s != APP_RECENTS) {
        int k = 0;
        while (k < recent_count && recent[k] != s)
            k++;
        if (k == recent_count && recent_count < MOB_RECENTS)
            recent_count++;
        if (k == MOB_RECENTS)
            k = MOB_RECENTS - 1;
        for (; k > 0; k--)
            recent[k] = recent[k - 1];
        recent[0] = s;
    }
    if (s == APP_WINDOW) {
        char name[32];
        winmob_name(shown, name, sizeof(name));
        printk("[mobile] window \"%s\" (%s)\n", name, why);
    } else {
        printk("[mobile] %s (%s)\n", screen_name(s), why);
    }
}

static void show(int win, const char *why)
{
    shown = win;
    go(APP_WINDOW, why);
}

/* A tap on the pulled-down shade */
static void shade_tap(int x, int y)
{
    for (int i = 0; i < MOB_SHADE_TILES; i++) {
        int tx, ty;
        shade_tile_xy(i, &tx, &ty);
        if (x < tx || x >= tx + MOB_SHADE_TILE_W || y < ty ||
            y >= ty + MOB_SHADE_TILE_H)
            continue;
        switch (i) {
        case 0:
            shade = 0;
            go(APP_SETTINGS, "the shade's network");
            return;
        case 1:
            mob_set_airplane(!mob_prefs.airplane);
            break;
        case 2:
            mob_prefs.dark = !mob_prefs.dark;
            printk("[mobile] dark mode %s\n", mob_prefs.dark ? "on" : "off");
            break;
        case 3:
            mob_prefs.dnd = !mob_prefs.dnd;
            printk("[mobile] do not disturb %s\n",
                   mob_prefs.dnd ? "on" : "off");
            break;
        case 4:
            mob_prefs.flashlight = !mob_prefs.flashlight;
            printk("[mobile] flashlight %s\n",
                   mob_prefs.flashlight ? "on" : "off");
            break;
        case 5:
            mob_prefs.saver = !mob_prefs.saver;
            mob_apply_saver();
            printk("[mobile] battery saver %s\n",
                   mob_prefs.saver ? "on" : "off");
            break;
        }
        dirty = 1;
        return;
    }
    if (y >= MOB_SHADE_BRIGHT_Y - 16 && y < MOB_SHADE_BRIGHT_Y + 16) {
        mob_set_brightness((x - MOB_SHADE_BRIGHT_X) * 100 /
                           MOB_SHADE_BRIGHT_W);
        dirty = 1;
        return;
    }
    if (y >= MOB_SHADE_NOTE_Y - 20 && y < MOB_SHADE_NOTE_Y + 8 &&
        x > lw - 90) {
        irqflags_t f = spin_lock_irq(&notif_lock);
        notif_count = 0;
        spin_unlock_irq(&notif_lock, f);
        printk("[mobile] notifications cleared\n");
        dirty = 1;
        return;
    }
    for (int i = 0; i < MOB_SHADE_CARDS; i++) {
        int cy = MOB_SHADE_CARD_Y + i * MOB_SHADE_CARD_STEP;
        if (y >= cy && y < cy + MOB_SHADE_CARD_H && i < notif_count) {
            notif_drop(i);
            printk("[mobile] notification %d dismissed\n", i + 1);
            dirty = 1;
            return;
        }
    }
}

static void open_app(int s, const char *name)
{
    if (s == APP_DRAW)
        clear_canvas();
    about_from_settings = 0;
    go(s, name);
}

/* Android's three buttons, at the bottom of every screen but the lock
   screen's */
static void nav_press(int x)
{
    if (x < (MOB_NAV_BACK_X + MOB_NAV_HOME_X) / 2) {
        printk("[mobile] back\n");
        if (shade) {
            shade = 0;
            dirty = 1;
        } else if (screen == APP_SETTINGS && settings_back()) {
            dirty = 1;
        } else if (screen == APP_ABOUT && about_from_settings) {
            about_from_settings = 0;
            go(APP_SETTINGS, "back");
        } else if (screen >= APP_ABOUT) {
            go(HOME, "back");
        }
        return;
    }
    shade = 0;
    if (x < (MOB_NAV_HOME_X + MOB_NAV_RECENT_X) / 2)
        go(HOME, "the home button");
    else
        go(APP_RECENTS, "the recent apps button");
}

static void tap(int x, int y)
{
    if (shade) {
        shade_tap(x, y);
        return;
    }
    if (screen == HOME && y >= MOB_DOCK_Y && y < MOB_DOCK_Y + MOB_DOCK_H) {
        for (int i = 0; i < dock_count(); i++) {
            int tx, ty;
            dock_xy(i, &tx, &ty);
            if (x < tx || x >= tx + MOB_DOCK_TILE || y < ty ||
                y >= ty + MOB_DOCK_TILE)
                continue;
            if (i < DOCK) {
                open_app(dock[i].screen, "dock");
            } else {
                const struct package *p = package_at(0);
                int pid = proc_spawn(p->exec, p->exec);
                printk("[mobile] %s started from the dock (pid %d)\n",
                       p->name, pid);
            }
            return;
        }
        return;
    }
    if (screen == APP_NOTES) {
        dirty |= notes_tap(x, y);
        return;
    }
    if (screen == APP_CALC) {
        dirty |= calc_tap(x, y);
        return;
    }
    if (screen == APP_RECORDER) {
        dirty |= recorder_tap(x, y);
        return;
    }
    if (screen == APP_SETTINGS) {
        int r = settings_tap(x, y);
        if (r == SETTINGS_ABOUT) {
            about_from_settings = 1;
            go(APP_ABOUT, "Settings");
        } else {
            dirty |= r;
        }
        return;
    }
    if (screen == APP_RECENTS) {
        int i = (y - MOB_RECENT_Y) / MOB_RECENT_STEP;
        if (y >= MOB_RECENT_Y && i < recent_count &&
            y < MOB_RECENT_Y + i * MOB_RECENT_STEP + MOB_RECENT_H)
            open_app(recent[i], "recent apps");
        return;
    }
    if (screen == HOME) {
        for (int k = 0; k < APPS; k++) {
            int tx, ty;
            tile_xy(k, &tx, &ty);
            if (x >= tx && x < tx + MOB_TILE && y >= ty && y < ty + MOB_TILE) {
                open_app(apps[k].screen, apps[k].name);
                return;
            }
        }
        for (int i = 0; i < package_count(); i++) {
            int tx, ty;
            tile_xy(APPS + i, &tx, &ty);
            if (x >= tx && x < tx + MOB_TILE && y >= ty && y < ty + MOB_TILE) {
                const struct package *p = package_at(i);
                int pid = proc_spawn(p->exec, p->exec);
                if (pid > 0)
                    printk("[mobile] %s started from its package (pid %d)\n",
                           p->name, pid);
                else
                    printk("[mobile] %s did not start (%d)\n", p->name, pid);
                return;
            }
        }
        for (int i = 0; i < winmob_count(); i++) {
            int tx, ty;
            tile_xy(APPS + package_count() + i, &tx, &ty);
            if (x >= tx && x < tx + MOB_TILE && y >= ty && y < ty + MOB_TILE &&
                winmob_nth(i) >= 0) {
                show(winmob_nth(i), "its tile");
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

static struct { int on, x0, y0, program, edge; } finger;

/* Where the phone's own bottom begins: the home gesture's edge, or
   the bar of Android's buttons */
static int bottom_edge(void)
{
    return mob_prefs.nav_buttons ? MOB_NAV_Y : lh - MOB_EDGE;
}

/* A finger in a program's window, in the window's own pixels: the
   panel's, from below the status bar (video_k is in sixteenths) */
static void to_program(int kind, int x, int y)
{
    int k = video_k();
    winmob_touch(shown, kind, x * k / 16, y * k / 16 - win_top);
}

static void touch(const struct touch_event *e)
{
    if (e->kind == TOUCH_DOWN) {
        finger.on = 1;
        finger.x0 = e->x;
        finger.y0 = e->y;
        last_x = e->x;
        last_y = e->y;
        printk("[touch] down %d,%d\n", e->x, e->y);
        /* The bottom edge is the home gesture's, the status bar the
           phone's; the rest of a window is its program's */
        finger.program = !shade && screen == APP_WINDOW &&
                         e->y * video_k() / 16 >= win_top &&
                         e->y < bottom_edge() &&
                         !(winmob_keyboard(shown) && e->y >= mob_kb_top());
        /* In the home gesture's edge a swipe up is the phone's, but a
           tap is still the program's: it is told once the finger lifts
           where it went down */
        finger.edge = !shade && screen == APP_WINDOW && !finger.program &&
                      !mob_prefs.nav_buttons && e->y >= bottom_edge() &&
                      e->y < lh - MOB_SAFE_UP &&
                      !(winmob_keyboard(shown) && e->y >= mob_kb_top());
        if (finger.program)
            to_program(WEV_PRESS, e->x, e->y);
        return;
    }
    if (!finger.on)
        return;
    if (finger.program)
        to_program(e->kind == TOUCH_UP ? WEV_RELEASE : WEV_DRAG, e->x, e->y);
    if (!shade && screen == APP_DRAW && finger.y0 > MOB_TITLE_Y + 12 &&
        finger.y0 < bottom_edge()) {
        gfx_line16_s(&canvas, last_x * 16, last_y * 16, e->x * 16,
                     e->y * 16, 6 * 16, mob_accent(), 255);
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
    if (mob_prefs.nav_buttons && screen != LOCK && finger.y0 >= MOB_NAV_Y &&
        adx <= 12 && ady <= 12) {
        nav_press(finger.x0);
        return;
    }
    if (shade) {
        if (dy < -80 && adx < ady) {
            shade = 0;
            dirty = 1;
            printk("[mobile] shade (closed)\n");
        } else if (adx <= 12 && ady <= 12) {
            tap(finger.x0, finger.y0);
        } else if (finger.y0 >= MOB_SHADE_BRIGHT_Y - 16 &&
                   finger.y0 < MOB_SHADE_BRIGHT_Y + 16) {
            shade_tap(e->x, MOB_SHADE_BRIGHT_Y);  /* the slider, dragged */
        }
        return;
    }
    if (dy > 80 && adx < ady && screen != LOCK &&
        (finger.y0 < MOB_BAR_H ||
         (screen == HOME && finger.y0 < MOB_DOCK_Y))) {
        /* Down from the status bar - from anywhere on home */
        shade = 1;
        dirty = 1;
        printk("[mobile] shade (opened)\n");
        return;
    }
    if (dy < -80 && adx < ady) {
        /* Up: on the lock screen from anywhere in its lower half, in an
           app only from the bottom edge, as a phone's home gesture */
        if (screen == LOCK && finger.y0 > lh / 2) {
            if (winmob_exists(pending))
                show(pending, "opened while locked");
            else
                go(HOME, "swipe up");
            pending = -1;
        }
        else if (screen >= APP_ABOUT && !mob_prefs.nav_buttons &&
                 finger.y0 >= lh - MOB_EDGE)
            go(HOME, "swipe up from the edge");
    } else if (adx <= 12 && ady <= 12 && finger.edge) {
        to_program(WEV_PRESS, finger.x0, finger.y0);
        to_program(WEV_RELEASE, finger.x0, finger.y0);
    } else if (adx <= 12 && ady <= 12 && !finger.program) {
        if (!shade && screen == APP_WINDOW && winmob_keyboard(shown) &&
            finger.y0 >= mob_kb_top()) {
            if (mob_kb_tap(finger.x0, finger.y0, type_into_window))
                dirty = 1;
            return;
        }
        tap(finger.x0, finger.y0);
    }
}

int mobile_init(const struct mobile_info *arg)
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
        return -1;
    }
    canvas.w = video_pw();
    canvas.h = video_ph();
    canvas.stride = video_pw();
    canvas.k = video_k();
    clear_canvas();
    /* Programs' windows stay where they were made in panel pixels,
       whatever the display size later: below the status bar as tall as
       it is at the largest size */
    win_top = MOB_BAR_H * video_k() / 16;
    winmob_init(video_pw(), win_top, video_ph());
    mob_windows_fit();
    mob_prefs.landscape = video_landscape();
    /* The battery, as the machine reports it: QEMU's virt has none, and
       a test or an emulator gives one as a fw_cfg file of text */
    uint16_t item;
    uint32_t size;
    char text[32];
    if (fwcfg_find("opt/threados/battery", &item, &size) == 0 && size &&
        size < sizeof(text) && fwcfg_read(item, text, size) == 0) {
        text[size] = 0;
        int v = 0, k = 0;
        while (text[k] >= '0' && text[k] <= '9')
            v = v * 10 + (text[k++] - '0');
        if (k && v <= 100) {
            battery = v;
            charging = text[k] == ' ' && text[k + 1] == 'c';
            printk("[mobile] battery %d%%%s\n", battery,
                   charging ? ", charging" : "");
        }
    }
    paint_wallpaper();
    printk("[mobile] %dx%d logical, %dx%d on the panel\n", lw, lh,
           video_pw(), video_ph());
    vsnd_volume(mob_prefs.volume, 0);
    mobsound_init();
    /* The display size the command line asks for (scale=150, 175) */
    for (int i = 1; i < MOB_SIZES; i++)
        if (info->scale == size_k16[i] * 100 / 16)
            mob_set_size(i);
    return 0;
}

void mobile_main(void *arg)
{
    (void)arg;
    go(LOCK, "start");

    uint32_t ticked = 0, changes = winmob_changes(), seq = notif_seq;
    int net = -1, sound = rec_changes(), mic = 0;
    for (;;) {
        struct touch_event e;
        while (touch_next(&e))
            touch(&e);
        /* Programs: a window made is shown (once the phone is open), one
           drawn into is drawn again, one whose program ended is left */
        int fresh = winmob_take_new();
        if (fresh >= 0) {
            if (screen == LOCK)
                pending = fresh;
            else
                show(fresh, "opened");
        }
        if (screen == APP_WINDOW && !winmob_exists(shown))
            go(HOME, "its program ended");
        if (winmob_changes() != changes) {
            changes = winmob_changes();
            if (screen == APP_WINDOW || screen == HOME)
                dirty = 1;
        }
        /* A notification: a banner, unless do not disturb; the shade's
           list is drawn again either way */
        if (notif_seq != seq) {
            seq = notif_seq;
            if (!mob_prefs.dnd && mob_prefs.banners) {
                irqflags_t f = spin_lock_irq(&notif_lock);
                banner = notifs[0];
                spin_unlock_irq(&notif_lock, f);
                banner_until = rtc_seconds() + MOB_BANNER_S;
            }
            dirty = 1;
        }
        if (banner_until && rtc_seconds() >= banner_until) {
            banner_until = 0;
            dirty = 1;
        }
        /* The Recorder's time and list; the microphone's dot */
        if (rec_changes() != sound) {
            sound = rec_changes();
            if (screen == APP_RECORDER)
                dirty = 1;
        }
        if ((vsnd_mic_who() >= 0) != mic) {
            mic = vsnd_mic_who() >= 0;
            dirty = 1;
        }
        /* The status bar's network */
        int now_net = mob_network((char *)0, 0);
        if (now_net != net) {
            net = now_net;
            dirty = 1;
        }
        /* The clock: every second where seconds show, every minute
           everywhere else */
        uint32_t now = rtc_seconds();
        uint32_t step = screen == APP_CLOCK ? now : now / 60;
        if (step != ticked) {
            ticked = step;
            dirty = 1;
        }
        if (dirty) {
            dirty = 0;
            draw();
        }
        /* Battery saver: the shell wakes a third as often */
        thread_sleep_ms(mob_prefs.saver ? 30 : 10);
    }
}
