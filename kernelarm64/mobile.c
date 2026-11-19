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
#define MOB_BATT_FAINT 110      /* its outline and nub, of 255 */
#define MOB_BAR_PITCH  5        /* the network's four bars, 3 wide */
#define MOB_DOCK_PCT   85       /* the dock's panel: this much of the
                                   width, centred */
#define MOB_DOCK_UP    112      /* the dock's top, from the bottom */
#define MOB_DOCK_H     80
#define MOB_DOCK_R     40       /* a pill, as Weft's toolbar */
#define MOB_DOCK_TILE  56
/* The shade's controls, as an iPhone's Control Center: a grid of four
   columns of squares CC_GAP apart, CC_SIDE from the screen's sides (no
   wider than CC_MAXW, on a tablet), from CC_TOP down - a square of
   four round buttons, two tall sliders, a pill and round buttons */
#define CC_TOP          48
#define CC_SIDE         22
#define CC_GAP          14
#define CC_MAXW         380
#define CC_PAD          10      /* the square's buttons, from its edge */
#define CC_FOOT         30      /* room kept below the grid, at least */
#define CC_NOTES_DOWN   38      /* "Notifications", below the grid */
#define CC_CARDS_DOWN   14      /* ... and its first card, below that */
#define MOB_SHADE_CARD_H 58
#define MOB_NP_H        64      /* the Now Playing card, above the
                                   notifications while something plays */
#define MOB_NP_BTN      40      /* its play or pause, a circle at its right */
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
/* The window a program has put on the whole screen (a page's video):
   over the status bar and the home indicator's strip, which are not
   drawn over it, its top the screen's. -1 = none. Asked for by the
   program's call (full_ask), done here, on the shell's thread. */
static int full_win = -1;
static volatile int full_ask_win = -1, full_ask_on;
/* What a program said it plays (SYS64_NOW_PLAYING): the shade's Now
   Playing card. -1 = nothing. Set from the program's call, read here. */
static struct spinlock np_lock = SPINLOCK_FREE;
static int np_win = -1, np_state;
static char np_title[64];
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
    if (full_win >= 0)
        return;                      /* the whole screen, its foot too */
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
    winmob_reshape(video_pw(), full_win >= 0 ? 0 : win_top, video_ph());
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
    /* rounded, as an iPhone's, those not lit faint */
    int lit = st == 2 ? 4 : st == 1 ? 1 : 0;
    for (int i = 0; i < 4; i++) {
        int h = 4 + i * 2;
        gfx_rrect16((x + i * MOB_BAR_PITCH) * 16, (19 - h) * 16, 3 * 16,
                    h * 16, 24, c, i < lit ? 255 : 70);
    }
}

static void status_bar(uint32_t c)
{
    struct rtc_time t;
    char s[16];
    rtc_split(rtc_seconds(), &t);
    clock_text(s, &t, 0);
    ui_font_text(UI_BOLD, 14, 22, 19, s, c);

    /* The battery, as an iPhone's: a pill with a faint outline and nub,
       filled as far as it is full */
    int bx = lw - MOB_BATT_RIGHT - MOB_BATT_W, by = 8;
    int pct = battery < 0 ? 100 : battery;
    gfx_rring16(bx * 16, by * 16, MOB_BATT_W * 16, 12 * 16, 4 * 16, 20, c,
                MOB_BATT_FAINT);
    gfx_rrect16((bx + MOB_BATT_W + 1) * 16, (by + 4) * 16, 24, 4 * 16, 12, c,
                MOB_BATT_FAINT);
    uint32_t fill = mob_prefs.saver ? 0xFFD60A :
                    battery >= 0 && battery <= 20 && !charging ? 0xFF453A :
                    (battery < 0 || charging) ? 0x30D158 : c;
    gfx_rrect16((bx + 2) * 16, (by + 2) * 16, (MOB_BATT_W - 4) * pct / 100 * 16,
                8 * 16, 2 * 16 + 8, fill, 255);
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
 * its rim as through the edge of a lens - the colours parting a little
 * there, as through real glass. The rounded edge is lit from the top
 * left: lighter on the sides that face the light and darker on those
 * that turn from it, and its rim catches the light where it curves
 * round the corners that face it (and, weaker, the opposite ones, as
 * light thrown back through the glass). A soft shadow below makes it a
 * thing in front.
 */

#define GLASS_LENS     14       /* the rim that bends, logical pixels */
#define GLASS_BEND     2        /* ... showing what is up to twice that
                                   further in, at the very rim */
#define GLASS_SPREAD   3        /* red bends 3/16 less, blue 3/16 more */
#define GLASS_BEVEL    24       /* the edge lit or shaded by this much */
#define GLASS_GLINT    150      /* the rim's light where it faces the light */
#define GLASS_BACK     80       /* ... and where it faces away */
#define GLASS_RING     36       /* the rim all round, of 255: a hairline, not
                                   a frame drawn round the glass */
#define MOB_MATTE_DARK  0x1C1C1E    /* a program's matte panel: this tint */
#define MOB_MATTE_LIGHT 0xF8F8FA
#define MOB_MATTE_A     200         /* ... this much of it */
#define GLASS_LIFT     16       /* brighter by 16/256 of what is left */

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

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* Each channel moved by 'by' (lighter if positive), held in 0..255 */
static uint32_t bevel(uint32_t c, int by)
{
    uint32_t out = 0;
    for (int sh = 0; sh < 24; sh += 8)
        out |= (uint32_t)clampi((int)((c >> sh) & 0xFF) + by, 0, 255) << sh;
    return out;
}

/* a over c, a of 256 */
static uint32_t mix(uint32_t c, uint32_t to, int a)
{
    uint32_t out = 0;
    for (int sh = 0; sh < 24; sh += 8) {
        uint32_t u = (c >> sh) & 0xFF, v = (to >> sh) & 0xFF;
        out |= ((u * (uint32_t)(256 - a) + v * (uint32_t)a) >> 8) << sh;
    }
    return out;
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

/* The glass on a surface - the screen's, or a program's window's: what
   is under the rounded rectangle, bent at its rim and a little brighter,
   and the rim lit. 'shadow' (the screen only) lays the panel's shadow
   under it once what it covers has been taken. */
static struct spinlock glass_lock = SPINLOCK_FREE;
static uint32_t *glass_tmp, glass_cap;

/* A program's glass is lightly frosted, over an app's content: what
   is under it blurred - a box blur each way, twice, which is close to a
   Gaussian - so that text under the toolbar does not fight what is on
   it. pw x ph pixels in glass_px, glass_tmp the room to work in */
#define FROST_R 6               /* the blur's reach, panel pixels: the
                                   page shows through, its colours and
                                   shapes, as through Apple's, but not
                                   its text, which would fight what is on
                                   the glass */
static void frost(int pw, int ph)
{
    for (int pass = 0; pass < 2; pass++) {
        for (int j = 0; j < ph; j++) {          /* across, into tmp */
            const uint32_t *src = glass_px + j * pw;
            uint32_t *dst = glass_tmp + j * pw;
            int sr = 0, sg = 0, sb = 0, n = 0;
            for (int i = -FROST_R; i < pw + FROST_R; i++) {
                int add = i + FROST_R, drop = i - FROST_R - 1;
                if (add >= 0 && add < pw) {
                    uint32_t c = src[add];
                    sr += (c >> 16) & 0xFF; sg += (c >> 8) & 0xFF;
                    sb += c & 0xFF; n++;
                }
                if (drop >= 0 && drop < pw) {
                    uint32_t c = src[drop];
                    sr -= (c >> 16) & 0xFF; sg -= (c >> 8) & 0xFF;
                    sb -= c & 0xFF; n--;
                }
                if (i >= 0 && i < pw && n)
                    dst[i] = (uint32_t)(sr / n) << 16 |
                             (uint32_t)(sg / n) << 8 | (uint32_t)(sb / n);
            }
        }
        for (int i = 0; i < pw; i++) {          /* down, back */
            int sr = 0, sg = 0, sb = 0, n = 0;
            for (int j = -FROST_R; j < ph + FROST_R; j++) {
                int add = j + FROST_R, drop = j - FROST_R - 1;
                if (add >= 0 && add < ph) {
                    uint32_t c = glass_tmp[add * pw + i];
                    sr += (c >> 16) & 0xFF; sg += (c >> 8) & 0xFF;
                    sb += c & 0xFF; n++;
                }
                if (drop >= 0 && drop < ph) {
                    uint32_t c = glass_tmp[drop * pw + i];
                    sr -= (c >> 16) & 0xFF; sg -= (c >> 8) & 0xFF;
                    sb -= c & 0xFF; n--;
                }
                if (j >= 0 && j < ph && n)
                    glass_px[j * pw + i] = (uint32_t)(sr / n) << 16 |
                                           (uint32_t)(sg / n) << 8 |
                                           (uint32_t)(sb / n);
            }
        }
    }
}

/* ... and tinted towards the look's own surface, an eighth of the way */
#define FROST_TINT 32
static uint32_t tint(uint32_t c, uint32_t to)
{
    uint32_t out = 0;
    for (int sh = 0; sh < 24; sh += 8) {
        uint32_t a = (c >> sh) & 0xFF, b = (to >> sh) & 0xFF;
        out |= ((a * (256 - FROST_TINT) + b * FROST_TINT) >> 8) << sh;
    }
    return out;
}

static void glass_on(const struct gfx_surface *t, int x, int y, int w, int h,
                     int r, int shadow, int frosted)
{
    int k = t->k ? t->k : 16;
    int px = x * k / 16, py = y * k / 16, pw = w * k / 16, ph = h * k / 16;
    int pr = r * k / 16, lens = GLASS_LENS * k / 16, rim = 3 * k / 32;
    /* Room for what is under a panel the size of the screen - a card
       the height of a page - and as much again to blur it in */
    if (!glass_px) {
        glass_cap = (uint32_t)video_pw() * (uint32_t)video_ph();
        uint32_t pages = (glass_cap * 4 + PAGE_SIZE - 1) / PAGE_SIZE;
        glass_px = (uint32_t *)(uintptr_t)pmm_alloc_contiguous(pages);
        glass_tmp = (uint32_t *)(uintptr_t)pmm_alloc_contiguous(pages);
    }
    /* Without that room the panel is lit, shaded and glinting all the
       same, only neither bent nor blurred: what is under it is read in
       place */
    int whole = glass_px && (uint32_t)(pw * ph) <= glass_cap;
    if (px < 0 || py < 0 || px + pw > t->w || py + ph > t->h) {
        gfx_rrect16_s(t, x * 16, y * 16, w * 16, h * 16, r * 16, WHITE, 60);
        return;
    }
    /* what is under it, before the shadow - which darkens the bands
       inside the shape too, counting on the shape to cover them */
    for (int j = 0; whole && j < ph; j++)
        memcpy(glass_px + j * pw, t->px + (long)(py + j) * t->stride + px,
               (size_t)pw * 4);
    if (shadow && whole)
        gfx_shadow(x, y, w, h, r, 10, 60, 3);
    if (frosted && whole && glass_tmp)
        frost(pw, ph);
    uint32_t surface = mob_prefs.dark ? 0x1C1C1E : 0xFFFFFF;
    for (int j = 0; j < ph; j++) {
        uint32_t *row = t->px + (long)(py + j) * t->stride + px;
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
            uint32_t c;
            if (d < lens) {
                /* the lens: nearer the rim, what is shown comes from
                   further in - the picture bends there, each colour by
                   its own amount */
                int depth = lens - d;
                int shift = GLASS_BEND * depth * depth / lens;
                c = whole ? 0 : row[i];
                for (int ch = 0; whole && ch < 3; ch++) {
                    int sh = shift * (16 + (ch - 1) * GLASS_SPREAD) / 16;
                    int sx = clampi(i + sh * nx / 256, 0, pw - 1);
                    int sy = clampi(j + sh * ny / 256, 0, ph - 1);
                    c |= glass_px[sy * pw + sx] & (0xFF0000u >> (ch * 8));
                }
                c = frosted ? tint(lift(c), surface) : lift(c);
                /* which way this part of the edge faces: towards the
                   light at the top left (+256), or away from it (-256) */
                int face = (nx + ny) * 181 / 256;
                c = bevel(c, face * GLASS_BEVEL * depth * depth /
                             (256 * lens * lens));
                if (d < rim) {
                    /* the rim's glint, only where it turns round to face
                       the light (or straight away from it) */
                    int p = face < 0 ? -face : face;
                    int p2 = p * p >> 8, p6 = (p2 * p2 >> 8) * p2 >> 8;
                    c = mix(c, WHITE, p6 * (face > 0 ? GLASS_GLINT
                                                     : GLASS_BACK) >> 8);
                }
            } else {
                c = whole ? glass_px[j * pw + i] : row[i];
                c = frosted ? tint(lift(c), surface) : lift(c);
            }
            row[i] = c;
        }
    }
    /* the rim itself, lit all round alike and smooth - the glint above
       follows its curve; a streak of light drawn along one edge read as
       a line across the glass rather than as its edge */
    gfx_rring16_s(t, x * 16, y * 16, w * 16, h * 16, r * 16, 16, WHITE,
                  GLASS_RING);
}

/* The shell's panels are the same glass as programs' (Weft's toolbar):
   what is under them blurred a little, as a frosted pane over the
   wallpaper or the home screen, which shows through sharp around it */
static void liquid_glass(int x, int y, int w, int h, int r)
{
    struct gfx_surface scr = { video_prow32(0), video_pw(), video_ph(),
                               video_pw(), video_k() };
    irqflags_t f = spin_lock_irq(&glass_lock);
    glass_on(&scr, x, y, w, h, r, 1, 1);
    spin_unlock_irq(&glass_lock, f);
}

/* How light a part of a surface is, 0..255: what goes on it is dark
   above the middle, light below */
static int lightness(const struct gfx_surface *t, int x, int y, int w, int h)
{
    int k = t->k ? t->k : 16;
    int px = x * k / 16, py = y * k / 16, pw = w * k / 16, ph = h * k / 16;
    uint32_t sum = 0, n = 0;
    for (int j = py; j < py + ph && j < t->h; j += 3)
        for (int i = px; i < px + pw && i < t->w; i += 3) {
            if (i < 0 || j < 0)
                continue;
            uint32_t c = t->px[(long)j * t->stride + i];
            sum += (((c >> 16) & 0xFF) * 77 + ((c >> 8) & 0xFF) * 150 +
                    (c & 0xFF) * 29) >> 8;
            n++;
        }
    return n ? (int)(sum / n) : 0;
}

/* A program's panel (WIN_DRAW's WD_GLASS) in the look Settings choose:
   Liquid Glass, a tint, or a surface - and how light it came out */
int mob_panel_on(const struct gfx_surface *t, int x, int y, int w, int h,
                 int r)
{
    if (glassy()) {
        irqflags_t f = spin_lock_irq(&glass_lock);
        glass_on(t, x, y, w, h, r, 0, 1);
        spin_unlock_irq(&glass_lock, f);
    } else if (mob_prefs.theme == MOB_THEME_SOLID) {
        gfx_rrect16_s(t, x * 16, y * 16, w * 16, h * 16, r * 16,
                      mob_prefs.dark ? 0x2C2C2E : 0xFFFFFF, 255);
    } else {
        gfx_rrect16_s(t, x * 16, y * 16, w * 16, h * 16, r * 16,
                      mob_prefs.dark ? MOB_MATTE_DARK : MOB_MATTE_LIGHT,
                      MOB_MATTE_A);
    }
    return lightness(t, x + r / 2, y + 4, w - r, h - 8);
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
   that white reads on it. Blurred enough that what was written there
   does not fight the shade's own words, and no more: its shapes and
   colours still show, for the glass over them to bend */
#define BLUR_BLOCK 6
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

/* What lies under Settings' cards with Liquid Glass: the wallpaper,
   dimmed so that white reads on it - not blurred, so that the glass
   over it is seen as glass, its own blur and the bend at its rim
   against the sharp picture around it, as Weft's toolbar is over the
   page */
#define GLASS_DIM 140           /* of 256 */
static void backdrop_dim(void)
{
    int w = video_pw(), h = video_ph();
    for (int y = 0; y < h; y++) {
        uint32_t *row = video_prow32(y);
        for (int x = 0; x < w; x++) {
            uint32_t c = row[x];
            row[x] = (((c >> 16 & 0xFF) * GLASS_DIM >> 8) << 16) |
                     (((c >> 8 & 0xFF) * GLASS_DIM >> 8) << 8) |
                     ((c & 0xFF) * GLASS_DIM >> 8);
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

/* An app's backdrop under Liquid Glass (Settings'): the wallpaper,
   blurred and darkened as under the pulled-down shade, for its cards of
   glass to lie on. 0, and nothing drawn, in the other looks. */
int mob_glass_backdrop(void)
{
    if (!glassy())
        return 0;
    with_background();
    backdrop_dim();
    return 1;
}

/* ... and how light the card came out, for what is written on it */
int mob_glass_card(int x, int y, int w, int h, int r)
{
    liquid_glass(x, y, w, h, r);
    struct gfx_surface scr = { video_prow32(0), video_pw(), video_ph(),
                               video_pw(), video_k() };
    return lightness(&scr, x + r / 2, y + 4, w - r, h - 8);
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

/* Light enough that what goes on it is dark */
static int light(uint32_t c)
{
    return ((((c >> 16) & 0xFF) * 77 + ((c >> 8) & 0xFF) * 150 +
             (c & 0xFF) * 29) >> 8) > 150;
}

/* The program's pixels below, the phone's status bar above - in the
   colour of the program's top, as an iPhone's app goes on under it,
   what it says dark or light to read on that */
static void draw_window(void)
{
    if (shown == full_win) {         /* the program's, edge to edge */
        winmob_draw(shown);
        if (winmob_keyboard(shown))
            mob_kb_draw();
        return;
    }
    uint32_t top = winmob_edge_colour(shown, 0);
    gfx_fill(0, 0, lw, (win_top * 16 + video_k() - 1) / video_k(), top, 255);
    status_bar(light(top) ? 0x1C1C1E : WHITE);
    winmob_draw(shown);
    /* the program waits for typing: the keyboard over its bottom */
    if (winmob_keyboard(shown))
        mob_kb_draw();
    pill(light(winmob_edge_colour(shown, 1)) ? 0x1C1C1E : 0xF2F2F7);
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


/* The controls, and where each is: the square's four buttons, the two
   sliders, the pill, two buttons beside it, and a row of four that
   open apps */
enum { CC_AIRPLANE, CC_NETWORK, CC_DARK, CC_SAVER, CC_BRIGHT, CC_VOLUME,
       CC_DND, CC_TURN, CC_TORCH, CC_CALC, CC_REC, CC_NOTES, CC_SETTINGS,
       CC_COUNT };

static int cc_unit(void)
{
    int w = lw - 2 * CC_SIDE < CC_MAXW ? lw - 2 * CC_SIDE : CC_MAXW;
    int across = (w - 3 * CC_GAP) / 4;
    /* on its side the height is what limits it */
    int down = (lh - CC_TOP - CC_FOOT - 3 * CC_GAP) / 4;
    return across < down ? across : down;
}

/* Wider than high: the notifications beside the controls, not below */
static int cc_wide(void)
{
    return lw > lh;
}

static int cc_width(void)
{
    return 4 * cc_unit() + 3 * CC_GAP;
}

static int cc_left(void)
{
    return cc_wide() ? CC_SIDE : (lw - cc_width()) / 2;
}

/* Where the notifications go: their heading's baseline, their cards'
   left, width and first top */
/* A title cut to the room it has, "..." where it is cut - never inside
   a character's bytes */
static const char *np_fit(const char *t, int room, char *out, int cap)
{
    if (ui_font_width(UI_BOLD, 15, t) <= room)
        return t;
    int n = 0;
    while (t[n] && n < cap - 4)
        n++;
    while (n > 0) {
        while (n > 0 && ((unsigned char)t[n] & 0xC0) == 0x80)
            n--;
        memcpy(out, t, (size_t)n);
        memcpy(out + n, "...", 4);
        if (ui_font_width(UI_BOLD, 15, out) <= room)
            return out;
        n--;
    }
    return "...";
}

/* Where the Now Playing card goes, while there is one: at the head of
   the notifications' column, which moves down for it */
static int np_card(int *x, int *y, int *w)
{
    if (np_win < 0)
        return 0;
    if (cc_wide()) {
        *x = cc_left() + cc_width() + 2 * CC_GAP;
        *w = lw - *x - CC_SIDE;
        *y = CC_TOP;
    } else {
        *w = lw - 24 < cc_width() + 24 ? lw - 24 : cc_width() + 24;
        *x = (lw - *w) / 2;
        *y = CC_TOP + cc_width() + CC_GAP;
    }
    return 1;
}

static void notes_area(int *head, int *x, int *w, int *top)
{
    if (cc_wide()) {
        *x = cc_left() + cc_width() + 2 * CC_GAP;
        *w = lw - *x - CC_SIDE;
        *head = CC_TOP + 16;
    } else {
        *w = lw - 24 < cc_width() + 24 ? lw - 24 : cc_width() + 24;
        *x = (lw - *w) / 2;
        *head = CC_TOP + cc_width() + CC_NOTES_DOWN;
    }
    if (np_win >= 0)
        *head += MOB_NP_H + CC_GAP;
    *top = *head + CC_CARDS_DOWN;
}

/* The square's corner, and its side */
static void cc_square(int *x, int *y, int *side)
{
    *x = cc_left();
    *y = CC_TOP;
    *side = 2 * cc_unit() + CC_GAP;
}

static void cc_box(int i, int *x, int *y, int *w, int *h)
{
    int u = cc_unit(), step = u + CC_GAP, l = cc_left();
    if (i <= CC_SAVER) {
        int sx, sy, side;
        cc_square(&sx, &sy, &side);
        int d = (side - 3 * CC_PAD) / 2;
        *x = sx + CC_PAD + (i % 2) * (d + CC_PAD);
        *y = sy + CC_PAD + (i / 2) * (d + CC_PAD);
        *w = *h = d;
        return;
    }
    static const signed char cell[][4] = {   /* column, row, columns, rows */
        { 2, 0, 1, 2 }, { 3, 0, 1, 2 },      /* the sliders */
        { 0, 2, 2, 1 }, { 2, 2, 1, 1 }, { 3, 2, 1, 1 },
        { 0, 3, 1, 1 }, { 1, 3, 1, 1 }, { 2, 3, 1, 1 }, { 3, 3, 1, 1 },
    };
    const signed char *c = cell[i - CC_BRIGHT];
    *x = l + c[0] * step;
    *y = CC_TOP + c[1] * step;
    *w = c[2] * u + (c[2] - 1) * CC_GAP;
    *h = c[3] * u + (c[3] - 1) * CC_GAP;
}

static int cc_at(int x, int y)
{
    for (int i = 0; i < CC_COUNT; i++) {
        int bx, by, bw, bh;
        cc_box(i, &bx, &by, &bw, &bh);
        if (x >= bx && x < bx + bw && y >= by && y < by + bh)
            return i;
    }
    return -1;
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

/* --- the Control Center's controls --------------------------------- */

static struct gfx_surface screen_surface(void)
{
    struct gfx_surface scr = { video_prow32(0), video_pw(), video_ph(),
                               video_pw(), video_k() };
    return scr;
}

/* The left half of a disc, and its outline: light and dark at once */
static void half_disc(int cx, int cy, int r, uint32_t c)
{
    struct gfx_surface scr = screen_surface();
    int k = video_k();
    gfx_circle16(cx * 16, cy * 16, r * 16, 24, c, 255);
    for (int py = (cy - r) * k / 16; py <= (cy + r) * k / 16; py++)
        for (int px = (cx - r) * k / 16; px < cx * k / 16; px++) {
            int a = gfx_cov_rrect16(px, py, (cx - r) * k, (cy - r) * k,
                                    2 * r * k, 2 * r * k, r * k);
            if (a)
                gfx_pblend_s(&scr, px, py, c, a);
        }
}

/* A crescent: a disc, and the same disc moved up and right in the
   colour behind it */
static void moon(int cx, int cy, int r, uint32_t c, uint32_t behind)
{
    gfx_disc16(cx * 16, cy * 16, r * 16, c, 255);
    gfx_disc16((cx + r / 2) * 16 + 8, (cy - r / 2) * 16, r * 13, behind,
               255);
}

static void plane(int cx, int cy, uint32_t c)
{
    int x = cx - 11, y = cy;
    gfx_line16(x * 16, y * 16, (x + 22) * 16, y * 16, 48, c, 255);
    gfx_line16((x + 12) * 16, y * 16, (x + 6) * 16, (y - 11) * 16, 48, c,
               255);
    gfx_line16((x + 12) * 16, y * 16, (x + 6) * 16, (y + 11) * 16, 48, c,
               255);
    gfx_line16((x + 3) * 16, y * 16, x * 16, (y - 5) * 16, 40, c, 255);
    gfx_line16((x + 3) * 16, y * 16, x * 16, (y + 5) * 16, 40, c, 255);
}

/* Wi-Fi's fan: three arcs over a dot, each a few straight pieces */
static void fan(int cx, int cy, uint32_t c)
{
    static const short sn[7] = { -181, -128, -66, 0, 66, 128, 181 };
    static const short cs[7] = { 181, 222, 247, 256, 247, 222, 181 };
    int oy = cy + 8;
    for (int r = 6; r <= 18; r += 6)
        for (int i = 0; i < 6; i++)
            gfx_line16(cx * 16 + r * sn[i] / 16, oy * 16 - r * cs[i] / 16,
                       cx * 16 + r * sn[i + 1] / 16,
                       oy * 16 - r * cs[i + 1] / 16, 40, c, 255);
    gfx_disc16(cx * 16, oy * 16, 40, c, 255);
}

static void small_battery(int cx, int cy, uint32_t c)
{
    gfx_rring16((cx - 11) * 16, (cy - 6) * 16, 20 * 16, 12 * 16, 4 * 16, 24,
                c, 255);
    gfx_rrect16((cx + 10) * 16, (cy - 2) * 16, 24, 4 * 16, 12, c, 255);
    gfx_rrect16((cx - 9) * 16, (cy - 4) * 16, 7 * 16, 8 * 16, 2 * 16, c, 255);
}

/* The phone, upright or on its side */
static void phone_shape(int cx, int cy, int side, uint32_t c)
{
    int w = side ? 22 : 13, h = side ? 13 : 22;
    gfx_rring16((cx - w / 2) * 16, (cy - h / 2) * 16, w * 16, h * 16, 3 * 16,
                28, c, 255);
}

static void torch(int cx, int cy, uint32_t c)
{
    gfx_rrect16((cx - 4) * 16, (cy - 3) * 16, 8 * 16, 15 * 16, 2 * 16, c, 255);
    gfx_line16((cx - 6) * 16, (cy - 9) * 16, (cx + 6) * 16, (cy - 9) * 16,
               40, c, 255);
    gfx_line16((cx - 6) * 16, (cy - 9) * 16, (cx - 4) * 16, (cy - 3) * 16,
               32, c, 255);
    gfx_line16((cx + 6) * 16, (cy - 9) * 16, (cx + 4) * 16, (cy - 3) * 16,
               32, c, 255);
}

static void glyph(int i, int cx, int cy, uint32_t c, uint32_t behind)
{
    switch (i) {
    case CC_AIRPLANE: plane(cx, cy, c); break;
    case CC_NETWORK:  fan(cx, cy, c); break;
    case CC_DARK:     half_disc(cx, cy, 11, c); break;
    case CC_SAVER:    small_battery(cx, cy, c); break;
    case CC_TURN:     phone_shape(cx, cy, mob_prefs.landscape, c); break;
    case CC_TORCH:    torch(cx, cy, c); break;
    case CC_CALC:                                /* + - x = */
        gfx_line16((cx - 11) * 16, (cy - 6) * 16, (cx - 3) * 16,
                   (cy - 6) * 16, 32, c, 255);
        gfx_line16((cx - 7) * 16, (cy - 10) * 16, (cx - 7) * 16,
                   (cy - 2) * 16, 32, c, 255);
        gfx_line16((cx + 3) * 16, (cy - 6) * 16, (cx + 11) * 16,
                   (cy - 6) * 16, 32, c, 255);
        gfx_line16((cx - 10) * 16, (cy + 3) * 16, (cx - 4) * 16,
                   (cy + 9) * 16, 32, c, 255);
        gfx_line16((cx - 4) * 16, (cy + 3) * 16, (cx - 10) * 16,
                   (cy + 9) * 16, 32, c, 255);
        gfx_line16((cx + 3) * 16, (cy + 4) * 16, (cx + 11) * 16,
                   (cy + 4) * 16, 32, c, 255);
        gfx_line16((cx + 3) * 16, (cy + 8) * 16, (cx + 11) * 16,
                   (cy + 8) * 16, 32, c, 255);
        break;
    case CC_REC:                                 /* a waveform */
        for (int k = -2; k <= 2; k++) {
            int h = k == 0 ? 11 : (k == -1 || k == 1) ? 7 : 4;
            gfx_line16((cx + k * 5) * 16, (cy - h) * 16, (cx + k * 5) * 16,
                       (cy + h) * 16, 40, c, 255);
        }
        break;
    case CC_NOTES:                               /* a page's lines */
        gfx_rring16((cx - 9) * 16, (cy - 11) * 16, 18 * 16, 22 * 16, 3 * 16,
                    24, c, 255);
        for (int k = 0; k < 3; k++)
            gfx_line16((cx - 5) * 16, (cy - 5 + k * 5) * 16,
                       (cx + (k == 2 ? 1 : 5)) * 16, (cy - 5 + k * 5) * 16,
                       28, c, 255);
        break;
    case CC_SETTINGS: {                          /* a gear */
        static const short sn[8] = { 0, 181, 256, 181, 0, -181, -256, -181 };
        for (int k = 0; k < 8; k++)
            gfx_disc16(cx * 16 + 10 * sn[k] / 16,
                       cy * 16 - 10 * sn[(k + 2) % 8] / 16, 40, c, 255);
        gfx_circle16(cx * 16, cy * 16, 8 * 16, 48, c, 255);
        break;
    }
    }
    (void)behind;
}

/* Is the control on, and the colour it is filled with then */
static int cc_on(int i, uint32_t *fill, uint32_t *ink)
{
    char addr[20];
    *ink = WHITE;
    *fill = mob_accent();
    switch (i) {
    case CC_AIRPLANE: *fill = 0xFF9F0A; return mob_prefs.airplane;
    case CC_NETWORK:  return mob_network(addr, sizeof(addr)) == 2;
    case CC_DARK:     return mob_prefs.dark;
    case CC_SAVER:    *fill = 0xFFD60A; *ink = 0x1C1C1E;
                      return mob_prefs.saver;
    case CC_TURN:     *fill = WHITE; *ink = 0xFF453A;
                      return mob_prefs.landscape;
    case CC_TORCH:    *fill = WHITE; *ink = 0x1C1C1E;
                      return mob_prefs.flashlight;
    }
    return 0;
}

/* A slider standing up: glass, filled from its foot as far as its value,
   the fill held inside the pill's round ends */
static void cc_slider(int i)
{
    int x, y, w, h;
    cc_box(i, &x, &y, &w, &h);
    panel(x, y, w, h, w / 2, 0x2C2C2E, 240);
    int v = i == CC_BRIGHT ? mob_prefs.brightness : mob_prefs.volume;
    int top = y + h - h * v / 100;
    struct gfx_surface scr = screen_surface();
    int k = video_k();
    for (int py = top * k / 16; py < (y + h) * k / 16; py++)
        for (int px = x * k / 16; px < (x + w) * k / 16; px++) {
            int a = gfx_cov_rrect16(px, py, x * k, y * k, w * k, h * k,
                                    w / 2 * k);
            if (a)
                gfx_pblend_s(&scr, px, py, WHITE, a * 235 / 255);
        }
    int cx = x + w / 2, cy = y + h - w / 2;
    uint32_t c = v >= 100 * (y + h - (cy - 12)) / h ? 0x8E8E93 : WHITE;
    if (i == CC_BRIGHT) {                        /* a sun */
        uint32_t sun = 0xFFB800;
        static const short sn[8] = { 0, 181, 256, 181, 0, -181, -256, -181 };
        gfx_disc16(cx * 16, cy * 16, 5 * 16, sun, 255);
        for (int r = 0; r < 8; r++)
            gfx_line16(cx * 16 + 8 * sn[r] / 16,
                       cy * 16 - 8 * sn[(r + 2) % 8] / 16,
                       cx * 16 + 11 * sn[r] / 16,
                       cy * 16 - 11 * sn[(r + 2) % 8] / 16, 28, sun, 255);
    } else {                                     /* a speaker */
        gfx_rrect16((cx - 8) * 16, (cy - 4) * 16, 5 * 16, 8 * 16, 16, c, 255);
        gfx_line16((cx - 4) * 16, (cy - 4) * 16, (cx + 2) * 16, (cy - 9) * 16,
                   40, c, 255);
        gfx_line16((cx - 4) * 16, (cy + 4) * 16, (cx + 2) * 16, (cy + 9) * 16,
                   40, c, 255);
        gfx_line16((cx + 2) * 16, (cy - 9) * 16, (cx + 2) * 16, (cy + 9) * 16,
                   40, c, 255);
        gfx_line16((cx + 6) * 16, (cy - 4) * 16, (cx + 7) * 16, (cy + 4) * 16,
                   32, c, 255);
    }
}

static void cc_draw(int i)
{
    int x, y, w, h;
    if (i == CC_AIRPLANE) {
        /* the square the first four stand in */
        int sx, sy, side;
        cc_square(&sx, &sy, &side);
        panel(sx, sy, side, side, cc_unit() / 2, 0x2C2C2E, 240);
    }
    if (i == CC_BRIGHT || i == CC_VOLUME) {
        cc_slider(i);
        return;
    }
    cc_box(i, &x, &y, &w, &h);
    if (i == CC_DND) {
        /* a pill: a moon in a disc of its own, and what it is */
        panel(x, y, w, h, h / 2, 0x2C2C2E, 240);
        int d = h - 16, mx = x + 8 + d / 2, my = y + h / 2;
        uint32_t disc = mob_prefs.dnd ? 0x5E5CE6 : 0x48484E;
        gfx_disc16(mx * 16, my * 16, d * 8, disc, 255);
        moon(mx - 1, my + 1, 9, WHITE, disc);
        /* Focus, as an iPhone names it: the pill is not wide enough for
           "Do not disturb" beside its moon */
        ui_font_text(UI_BOLD, 15, x + d + 18, my - 2, "Focus", WHITE);
        ui_font_text(UI_SANS, 13, x + d + 18, my + 16,
                     mob_prefs.dnd ? "Do not disturb" : "Off", MUTED);
        return;
    }
    uint32_t fill, ink;
    int on = cc_on(i, &fill, &ink);
    if (on)
        gfx_rrect(x, y, w, h, w / 2, fill, 255);
    else
        panel(x, y, w, h, w / 2, 0x2C2C2E, 240);
    glyph(i, x + w / 2, y + h / 2, on ? ink : WHITE, on ? fill : 0x2C2C2E);
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
    char s[80];
    for (int i = 0; i < CC_COUNT; i++)
        cc_draw(i);

    int head, cx, cw, top;
    int px, py, pw;
    if (np_card(&px, &py, &pw)) {
        /* Now Playing: what, and play or pause for it */
        char t[64];
        irqflags_t g = spin_lock_irq(&np_lock);
        int state = np_state;
        memcpy(t, np_title, sizeof(t));
        spin_unlock_irq(&np_lock, g);
        panel(px, py, pw, MOB_NP_H, 20, 0x2C2C2E, 240);
        ui_font_text(UI_SANS, 12, px + 16, py + 22, "Now Playing", MUTED);
        char cut[72];
        ui_font_text(UI_BOLD, 15, px + 16, py + 44,
                     np_fit(t[0] ? t : "Playing", pw - 32 - MOB_NP_BTN, cut,
                            sizeof(cut)), WHITE);
        int bx = px + pw - 12 - MOB_NP_BTN, by = py + (MOB_NP_H - MOB_NP_BTN) / 2;
        gfx_rrect(bx, by, MOB_NP_BTN, MOB_NP_BTN, MOB_NP_BTN / 2, WHITE, 255);
        int cx0 = bx + MOB_NP_BTN / 2, cy0 = by + MOB_NP_BTN / 2;
        if (state == 1) {
            gfx_rrect(cx0 - 7, cy0 - 8, 5, 16, 1, 0x1C1C1E, 255);
            gfx_rrect(cx0 + 2, cy0 - 8, 5, 16, 1, 0x1C1C1E, 255);
        } else {
            for (int k = 0; k < 16; k++)    /* a triangle, row by row */
                gfx_fill(cx0 - 5, cy0 - 8 + k, 1 + (k < 8 ? k : 15 - k) * 12 / 8,
                         1, 0x1C1C1E, 255);
        }
    }
    notes_area(&head, &cx, &cw, &top);
    ui_font_text(UI_BOLD, 15, cx + 4, head, "Notifications", WHITE);
    irqflags_t f = spin_lock_irq(&notif_lock);
    int n = notif_count;
    struct notif copy[MOB_SHADE_CARDS];
    for (int i = 0; i < n && i < MOB_SHADE_CARDS; i++)
        copy[i] = notifs[i];
    spin_unlock_irq(&notif_lock, f);
    if (n) {
        ui_font_text(UI_SANS, 15, cx + cw - 4 - ui_font_width(UI_SANS, 15,
                     "Clear"), head, "Clear", mob_accent());
    } else {
        centred(UI_SANS, 15, cx + cw / 2, top + 34, "No notifications",
                MUTED);
    }
    for (int i = 0; i < n && i < MOB_SHADE_CARDS; i++) {
        int y = top + i * MOB_SHADE_CARD_STEP;
        panel(cx, y, cw, MOB_SHADE_CARD_H, 20, 0x2C2C2E, 240);
        ui_font_text(UI_BOLD, 15, cx + 16, y + 24, copy[i].title, WHITE);
        ui_font_text(UI_SANS, 13, cx + 16, y + 44, copy[i].body, MUTED);
        when_text(s, copy[i].at);
        ui_font_text(UI_SANS, 12, cx + cw - 16 - ui_font_width(UI_SANS, 12,
                     s), y + 24, s, MUTED);
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
/* A slider set where the finger is: its foot 0, its top 100 */
static void cc_slide(int i, int fy)
{
    int x, y, w, h;
    cc_box(i, &x, &y, &w, &h);
    int v = (y + h - fy) * 100 / h;
    v = v < 0 ? 0 : v > 100 ? 100 : v;
    if (i == CC_BRIGHT) {
        mob_set_brightness(v);
    } else {
        mob_prefs.volume = v;
        vsnd_volume(v, -1);
        printk("[mobile] volume %d%%\n", v);
    }
    dirty = 1;
}

static void shade_tap(int x, int y)
{
    int i = cc_at(x, y);
    switch (i) {
    case CC_NETWORK:
        shade = 0;
        go(APP_SETTINGS, "the shade's network");
        return;
    case CC_AIRPLANE:
        mob_set_airplane(!mob_prefs.airplane);
        break;
    case CC_DARK:
        mob_prefs.dark = !mob_prefs.dark;
        printk("[mobile] dark mode %s\n", mob_prefs.dark ? "on" : "off");
        break;
    case CC_SAVER:
        mob_prefs.saver = !mob_prefs.saver;
        mob_apply_saver();
        printk("[mobile] battery saver %s\n", mob_prefs.saver ? "on" : "off");
        break;
    case CC_BRIGHT:
    case CC_VOLUME:
        cc_slide(i, y);
        return;
    case CC_DND:
        mob_prefs.dnd = !mob_prefs.dnd;
        printk("[mobile] do not disturb %s\n", mob_prefs.dnd ? "on" : "off");
        break;
    case CC_TURN:
        mob_set_orientation(!mob_prefs.landscape);
        break;
    case CC_TORCH:
        mob_prefs.flashlight = !mob_prefs.flashlight;
        printk("[mobile] flashlight %s\n",
               mob_prefs.flashlight ? "on" : "off");
        break;
    case CC_CALC:
    case CC_REC:
    case CC_NOTES:
    case CC_SETTINGS: {
        static const int app[4] = { APP_CALC, APP_RECORDER, APP_NOTES,
                                    APP_SETTINGS };
        shade = 0;
        go(app[i - CC_CALC], "the shade");
        return;
    }
    }
    if (i >= 0) {
        dirty = 1;
        return;
    }
    int px, py, pw;
    if (np_card(&px, &py, &pw) && x >= px && x < px + pw && y >= py &&
        y < py + MOB_NP_H) {
        int win = np_win;
        if (x >= px + pw - 12 - MOB_NP_BTN - 8) {
            /* play or pause: to the program, which says back what it
               does now (SYS64_NOW_PLAYING) */
            winmob_media(win, np_state == 1 ? 2 : 1);
            printk("[mobile] now playing: %s\n",
                   np_state == 1 ? "pause" : "play");
        } else {
            shade = 0;               /* the rest of it: to the program */
            show(win, "now playing");
        }
        dirty = 1;
        return;
    }
    int head, nx, nw, top;
    notes_area(&head, &nx, &nw, &top);
    if (y >= head - 20 && y < head + 8 && x > nx + nw - 90 && x < nx + nw) {
        irqflags_t f = spin_lock_irq(&notif_lock);
        notif_count = 0;
        spin_unlock_irq(&notif_lock, f);
        printk("[mobile] notifications cleared\n");
        dirty = 1;
        return;
    }
    for (int n = 0; n < MOB_SHADE_CARDS; n++) {
        int cy = top + n * MOB_SHADE_CARD_STEP;
        if (y >= cy && y < cy + MOB_SHADE_CARD_H && n < notif_count &&
            x >= nx && x < nx + nw) {
            notif_drop(n);
            printk("[mobile] notification %d dismissed\n", n + 1);
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
    winmob_touch(shown, kind, x * k / 16,
                 y * k / 16 - (full_win >= 0 ? 0 : win_top));
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
                         e->y * video_k() / 16 >=
                             (shown == full_win ? 0 : win_top) &&
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
    if (shade && e->kind != TOUCH_UP) {
        /* a slider follows the finger */
        int s = cc_at(finger.x0, finger.y0);
        if (s == CC_BRIGHT || s == CC_VOLUME)
            cc_slide(s, e->y);
    }
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
        int s = cc_at(finger.x0, finger.y0);
        if (adx <= 12 && ady <= 12) {
            tap(finger.x0, finger.y0);
        } else if (s == CC_BRIGHT || s == CC_VOLUME) {
            cc_slide(s, e->y);       /* a slider, dragged: not a close */
        } else if (dy < -80 && adx < ady) {
            shade = 0;
            dirty = 1;
            printk("[mobile] shade (closed)\n");
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

void mobile_now_playing(int win, int state, const char *title)
{
    irqflags_t f = spin_lock_irq(&np_lock);
    if (state == 0) {
        if (np_win == win)
            np_win = -1;
    } else {
        np_win = win;
        np_state = state;
        int i = 0;
        for (; title[i] && i < (int)sizeof(np_title) - 1; i++)
            np_title[i] = title[i];
        np_title[i] = 0;
    }
    spin_unlock_irq(&np_lock, f);
    printk("[mobile] now playing %s\n", state == 1 ? "playing" :
           state == 2 ? "paused" : "nothing");
    dirty = 1;
}

void mobile_window_full(int win, int on)
{
    full_ask_on = on;
    full_ask_win = win;
}

/* The whole screen given, or given back: every window is made again in
   the new shape (they share one) and told its size */
static void full_set(int win)
{
    if (win == full_win)
        return;
    full_win = win;
    winmob_reshape(video_pw(), win >= 0 ? 0 : win_top, video_ph());
    if (win < 0)
        mob_windows_fit();
    printk("[mobile] window %d %s\n", win >= 0 ? win : 0,
           win >= 0 ? "full screen" : "back from full screen");
    dirty = 1;
}

void mobile_main(void *arg)
{
    (void)arg;
    go(LOCK, "start");

    uint32_t ticked = 0, changes = winmob_changes(), seq = notif_seq;
    int net = -1, sound = rec_changes(), mic = 0, look = -1;
    for (;;) {
        /* The look - which theme, light or dark, the accent - changed,
           from Settings or the shade: programs' windows draw themselves
           in it again, those in the background too, so that one comes
           back to the screen as the rest of the phone looks */
        int now_look = mob_prefs.theme | mob_prefs.dark << 4 |
                       mob_prefs.accent << 8;
        if (now_look != look) {
            if (look >= 0) {
                winmob_look_changed();
                printk("[mobile] look changed\n");
            }
            look = now_look;
        }
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
        if (full_ask_win >= 0) {
            int w = full_ask_win, on = full_ask_on;
            full_ask_win = -1;
            if (on)
                full_set(w);
            else if (w == full_win)
                full_set(-1);
        }
        if (full_win >= 0 && !winmob_exists(full_win))
            full_set(-1);            /* its program ended full */
        if (np_win >= 0 && !winmob_exists(np_win)) {
            np_win = -1;             /* what played ended with its program */
            dirty = 1;
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
