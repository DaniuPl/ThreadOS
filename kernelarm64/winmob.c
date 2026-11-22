/* Programs' windows on the phone
 *
 * On the PC a window is a frame among others on the desktop; on a phone
 * a program has the whole screen. So a window here is the screen below
 * the status bar, whatever size the program asked for, and the first
 * thing it hears (WEV_SIZE) is the size it has - Weft's WPE platform,
 * and any program written for the PC's desktop, already redraws itself
 * at the size the desktop tells it. Its pixels are its own buffer,
 * 0x00RRGGBB as the panel's (win_blit32); the shell (mobile.c) puts
 * the window in front on the screen and draws its status bar over it.
 *
 * A finger in the window is the mouse the program knows: a touch is a
 * click and a press, moving is dragging, lifting is the release - in
 * the window's own pixels. The home gesture stays the shell's.
 *
 * Those pixels are logical ones, as the shell's are: the display size
 * (Settings) makes one of them k/16 of the panel's - two at 200% - and
 * WEV_SIZE says the window's logical size and k (its val, from bit 8).
 * A program that only blits (WIN_BLIT32) has each of its pixels drawn
 * as the block of the panel's it covers, as the PC's desktop does at
 * its larger sizes; shapes and text (WIN_DRAW) are drawn at the panel's
 * own resolution; and a program that renders for the panel itself - a
 * web page at the screen's scale - hands over panel pixels
 * (WIN_BLITHI), which go as they are.
 *
 * A window belongs to a program, not to the thread that made it, and
 * goes when the program ends.
 */

#include <stdint.h>
#include "arm64.h"
#include "winmob.h"
#include "mobapps.h"
#include "thread.h"
#include "../kernel/include/desktop.h"
#include "../kernel/include/gfx.h"
#include "../kernel/include/locale.h"
#include "../kernel/include/pmm.h"
#include "../kernel/include/printk.h"
#include "../kernel/include/spinlock.h"
#include "../kernel/include/string.h"
#include "../kernel/include/uitext.h"
#include "../kernel/include/video.h"

#define EVENTS 128

struct window {
    int used;
    int owner;
    uint32_t order;              /* when it was made */
    char title[32];
    uint32_t *px;                /* the panel's pixels: ww x wh */
    uint32_t pages;
    int clip, clx, cly, clw, clh;    /* WIN_DRAW's box, logical */
    int keyboard;                /* the program waits for typing */
    int drawing;                 /* WIN_DRAWs drawing into px now */
    int held;                    /* kept from the screen (WD_HOLD) */
    struct win_event ev[EVENTS];
    uint32_t head, tail;
};

static struct window wins[WINMOB_MAX];
static struct spinlock lock = SPINLOCK_FREE;
static int ww, wtop, wh;         /* where windows go, in the panel's
                                    pixels; 0 = no screen */
static int wmax;                 /* the most wh can be: each window's
                                    pixels are made this tall */
static volatile uint32_t changes;
static volatile int reshaping;
static int fresh = -1;

static void push(struct window *w, int type, int x, int y, int val);
static int L(int v);
static uint32_t made;

void winmob_init(int width, int top, int bottom)
{
    ww = width;
    wtop = top;
    wh = wmax = bottom - top;
}

/* The screen turned: windows of the new shape, each window's pixels
   made again for it (white, as a new window's), and each told its new
   size - a program draws itself again on WEV_SIZE */
void winmob_reshape(int width, int top, int bottom)
{
    reshaping = 1;
    uint32_t pages = ((uint32_t)width * (uint32_t)(bottom - top) * 4 +
                      PAGE_SIZE - 1) / PAGE_SIZE;
    uint32_t *fresh_px[WINMOB_MAX] = { 0 };
    for (int i = 0; i < WINMOB_MAX; i++) {
        if (!wins[i].used)
            continue;
        fresh_px[i] = (uint32_t *)(uintptr_t)pmm_alloc_contiguous(pages);
        if (fresh_px[i])
            for (uint32_t k = 0; k < pages * PAGE_SIZE / 4; k++)
                fresh_px[i][k] = 0xFFFFFF;
    }
    uint32_t *old_px[WINMOB_MAX] = { 0 };
    uint32_t old_pages[WINMOB_MAX] = { 0 };
    for (;;) {
        irqflags_t f = spin_lock_irq(&lock);
        int busy = 0;
        for (int i = 0; i < WINMOB_MAX; i++)
            busy |= wins[i].used && wins[i].drawing;
        if (!busy) {
            ww = width;
            wtop = top;
            wh = wmax = bottom - top;
            for (int i = 0; i < WINMOB_MAX; i++) {
                if (!wins[i].used)
                    continue;
                if (!fresh_px[i]) {
                    /* no room for its new pixels: the window goes, as
                       its program's would */
                    old_px[i] = wins[i].px;
                    old_pages[i] = wins[i].pages;
                    wins[i].used = 0;
                    continue;
                }
                old_px[i] = wins[i].px;
                old_pages[i] = wins[i].pages;
                wins[i].px = fresh_px[i];
                wins[i].pages = pages;
                fresh_px[i] = 0;
                push(&wins[i], WEV_SIZE, L(ww), L(wh),
                     WEV_SIZE_TOUCH | video_k() << 8);
            }
            changes++;
            spin_unlock_irq(&lock, f);
            break;
        }
        spin_unlock_irq(&lock, f);
        thread_sleep_ms(1);
    }
    for (int i = 0; i < WINMOB_MAX; i++) {
        for (uint32_t k = 0; old_px[i] && k < old_pages[i]; k++)
            pmm_free_frame((uint32_t)(uintptr_t)old_px[i] + k * PAGE_SIZE);
        for (uint32_t k = 0; fresh_px[i] && k < pages; k++)
            pmm_free_frame((uint32_t)(uintptr_t)fresh_px[i] + k * PAGE_SIZE);
    }
    reshaping = 0;
}

void winmob_set_bottom(int bottom)
{
    int h = bottom - wtop;
    if (h < 1 || h > wmax || h == wh)
        return;
    wh = h;
    winmob_rescale();
}

/* The panel's pixel where a logical one begins, and back */
static int P(int v)
{
    return (int)((int64_t)v * video_k() / 16);
}

static int L(int v)
{
    return (int)((int64_t)v * 16 / video_k());
}

static void push(struct window *w, int type, int x, int y, int val)
{
    if (w->head - w->tail >= EVENTS)
        return;                  /* a program that never looks loses some */
    struct win_event *e = &w->ev[w->head % EVENTS];
    e->type = type;
    e->x = x;
    e->y = y;
    e->val = val;
    w->head++;
}

static void set_title(struct window *w, const char *title)
{
    int i = 0;
    for (; title[i] && i < (int)sizeof(w->title) - 1; i++)
        w->title[i] = title[i];
    w->title[i] = 0;
}

int64_t winmob_create(int owner, const char *title)
{
    while (reshaping)
        thread_sleep_ms(5);          /* the screen is turning */
    if (!ww)
        return -1;                   /* no screen */
    uint32_t pages = ((uint32_t)ww * (uint32_t)wmax * 4 + PAGE_SIZE - 1) /
                     PAGE_SIZE;
    uint32_t *px = (uint32_t *)(uintptr_t)pmm_alloc_contiguous(pages);
    if (!px)
        return -1;
    /* White, as a page is before anything is drawn on it */
    for (uint32_t i = 0; i < (uint32_t)ww * (uint32_t)wmax; i++)
        px[i] = 0xFFFFFF;
    irqflags_t f = spin_lock_irq(&lock);
    int id = -1;
    for (int i = 0; i < WINMOB_MAX && id < 0; i++)
        if (!wins[i].used)
            id = i;
    if (id >= 0) {
        struct window *w = &wins[id];
        memset(w, 0, sizeof(*w));
        w->used = 1;
        w->owner = owner;
        w->order = made++;
        w->px = px;
        w->pages = pages;
        set_title(w, title);
        push(w, WEV_SIZE, L(ww), L(wh), WEV_SIZE_TOUCH | video_k() << 8);
        fresh = id;
        changes++;
    }
    spin_unlock_irq(&lock, f);
    if (id < 0) {
        for (uint32_t i = 0; i < pages; i++)
            pmm_free_frame((uint32_t)(uintptr_t)px + i * PAGE_SIZE);
        return -1;
    }
    printk("[winmob] window %d for program %d: \"%s\", %dx%d at %d/16\n",
           id, owner, wins[id].title, L(ww), L(wh), video_k());
    return id;
}

static struct window *mine(int win, int owner)
{
    if (win < 0 || win >= WINMOB_MAX || !wins[win].used ||
        wins[win].owner != owner)
        return 0;
    return &wins[win];
}

int winmob_mine(int win, int owner)
{
    return mine(win, owner) != 0;
}

/* The program's memory was checked by the caller; a window cannot go
   while its own program is in a call. Panel pixels, as they are. */
int64_t winmob_blit_hi(int win, int owner, int x, int y, int w, int h,
                       const uint32_t *px)
{
    irqflags_t f = spin_lock_irq(&lock);
    struct window *o = mine(win, owner);
    if (o) {
        for (int r = 0; r < h; r++) {
            int ty = y + r;
            if (ty < 0 || ty >= wh)
                continue;
            int x0 = x < 0 ? 0 : x, x1 = x + w > ww ? ww : x + w;
            if (x1 > x0)
                memcpy(o->px + (long)ty * ww + x0, px + (long)r * w + (x0 - x),
                       (size_t)(x1 - x0) * 4);
        }
        changes++;
    }
    spin_unlock_irq(&lock, f);
    return o ? 0 : -1;
}

/* Logical pixels: each the block of the panel's it covers */
int64_t winmob_blit32(int win, int owner, int x, int y, int w, int h,
                      const uint32_t *px)
{
    if (video_k() == 16)
        return winmob_blit_hi(win, owner, x, y, w, h, px);
    irqflags_t f = spin_lock_irq(&lock);
    struct window *o = mine(win, owner);
    if (o) {
        for (int r = 0; r < h; r++) {
            int y0 = P(y + r), y1 = P(y + r + 1);
            if (y0 < 0)
                y0 = 0;
            if (y1 > wh)
                y1 = wh;
            for (int c = 0; c < w && y1 > y0; c++) {
                int x0 = P(x + c), x1 = P(x + c + 1);
                if (x0 < 0)
                    x0 = 0;
                if (x1 > ww)
                    x1 = ww;
                uint32_t v = px[(long)r * w + c];
                for (int py = y0; py < y1; py++)
                    for (int qx = x0; qx < x1; qx++)
                        o->px[(long)py * ww + qx] = v;
            }
        }
        changes++;
    }
    spin_unlock_irq(&lock, f);
    return o ? 0 : -1;
}

static int64_t draw_into(const struct window *w, const struct win_draw *d,
                         const char *s, int font, int px);

/* Shapes and text, as the PC's desktop draws them (desktop_win_draw):
   in logical pixels, onto the panel's at its scale. The lock is held
   only to find the window: a program cannot lose its window while in a
   call, and the shell copying the pixels meanwhile sees at worst half a
   shape, drawn whole a moment later. */
int64_t winmob_draw_cmd(int win, int owner, const struct win_draw *d,
                        const char *s)
{
    int px = d->px > 0 && d->px <= 64 ? d->px : UI_PX;
    int font = d->font >= UI_BOLD && d->font <= UI_SERIF_BOLD ? d->font
                                                                : UI_SANS;
    if (d->op == WD_MEASURE || d->op == WD_LMEASURE)
        return s ? ui_font_width(font, px,
                                 d->op == WD_LMEASURE ? locale_text(s) : s)
                 : 0;
    irqflags_t f = spin_lock_irq(&lock);
    struct window *o = mine(win, owner);
    if (o && d->op == WD_HOLD) {
        o->held = d->w != 0;
        if (!o->held)
            changes++;               /* the picture whole, at last */
        spin_unlock_irq(&lock, f);
        return 0;
    }
    if (o && d->op == WD_CLIP) {         /* w or h 0: no clip */
        o->clip = d->w > 0 && d->h > 0;
        o->clx = d->x;
        o->cly = d->y;
        o->clw = d->w;
        o->clh = d->h;
    }
    struct window snap = o ? *o : (struct window){ 0 };
    if (o && d->op != WD_CLIP)
        o->drawing++;            /* its pixels stay while drawn into */
    spin_unlock_irq(&lock, f);
    if (!o)
        return -1;
    if (d->op == WD_CLIP)
        return 0;
    int64_t ret = draw_into(&snap, d, s, font, px);
    f = spin_lock_irq(&lock);
    o->drawing--;
    changes++;
    spin_unlock_irq(&lock, f);
    return ret;
}

static int64_t draw_into(const struct window *w, const struct win_draw *d,
                         const char *s, int font, int px)
{
    struct window snap = *w;

    struct gfx_surface t = { snap.px, ww, wh, ww, video_k() };
    struct win_draw moved = *d;
    int ox = 0, oy = 0;                  /* where t starts in the window */
    if (snap.clip) {
        /* the box's part of the pixels, and what is drawn moved by as
           much: nothing drawn can leave it */
        int x0 = P(snap.clx), y0 = P(snap.cly);
        int x1 = P(snap.clx + snap.clw), y1 = P(snap.cly + snap.clh);
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > ww) x1 = ww;
        if (y1 > wh) y1 = wh;
        if (x1 <= x0 || y1 <= y0) {
            if ((d->op == WD_TEXT || d->op == WD_LTEXT) && s)
                return d->x + ui_font_width(font, px, d->op == WD_LTEXT
                                                      ? locale_text(s) : s);
            return d->op == WD_TEXT || d->op == WD_LTEXT ||
                   d->op == WD_CELLS ? d->x : 0;
        }
        t.px += (long)y0 * ww + x0;
        t.w = x1 - x0;
        t.h = y1 - y0;
        ox = x0;
        oy = y0;
        moved.x -= snap.clx;
        moved.y -= snap.cly;
        if (d->op == WD_LINE || d->op == WD_THREAD) {
            moved.w -= snap.clx;         /* (w and h are its other end) */
            moved.h -= snap.cly;
        }
        if (d->op == WD_THREAD) {        /* ... and its two pulls */
            moved.font = WD_PACK(WD_PACKED_X(d->font) - snap.clx,
                                 WD_PACKED_Y(d->font) - snap.cly);
            moved.px = WD_PACK(WD_PACKED_X(d->px) - snap.clx,
                               WD_PACKED_Y(d->px) - snap.cly);
        }
        d = &moved;
    }
    int a = d->a < 0 ? 0 : d->a > 255 ? 255 : d->a;
    int r = d->r < 0 ? 0 : d->r;
    int back = snap.clip ? snap.clx : 0;
    int64_t ret = 0;
    switch (d->op) {
    case WD_GLASS:
        ret = mob_panel_on(&t, d->x, d->y, d->w, d->h, r);
        break;
    case WD_FILL:
        gfx_fill_s(&t, d->x, d->y, d->w, d->h, d->rgb, a);
        break;
    case WD_RRECT:
        gfx_rrect16_s(&t, d->x * 16, d->y * 16, d->w * 16, d->h * 16, r * 16,
                      d->rgb, a);
        break;
    case WD_RING:
        gfx_rring16_s(&t, d->x * 16, d->y * 16, d->w * 16, d->h * 16, r * 16,
                      16, d->rgb, a);
        break;
    case WD_GRAD:
        gfx_rrect_grad_s(&t, d->x, d->y, d->w, d->h, r, d->rgb, d->rgb2, a);
        break;
    case WD_DISC:
        gfx_disc16_s(&t, d->x * 16 + d->w * 8, d->y * 16 + d->w * 8,
                     d->w * 8, d->rgb, a);
        break;
    case WD_LINE:
        gfx_line16_s(&t, d->x * 16 + 8, d->y * 16 + 8, d->w * 16 + 8,
                     d->h * 16 + 8, r ? r : 16, d->rgb, a);
        break;
    case WD_GRAIN:
        gfx_grain_s(&t, d->x, d->y, d->w, d->h, ox, oy, d->rgb, r);
        break;
    case WD_STITCH:
        gfx_stitch_s(&t, d->x, d->y, d->w, d->h, r, d->rgb, a);
        break;
    case WD_KNOT:
        gfx_knot_s(&t, d->x, d->y, d->w, r, d->rgb, a);
        break;
    case WD_THREAD:
        gfx_thread_s(&t, d->x * 16 + 8, d->y * 16 + 8,
                     WD_PACKED_X(d->font) * 16 + 8,
                     WD_PACKED_Y(d->font) * 16 + 8,
                     WD_PACKED_X(d->px) * 16 + 8, WD_PACKED_Y(d->px) * 16 + 8,
                     d->w * 16 + 8, d->h * 16 + 8, r ? r : 16, d->rgb, a);
        break;
    case WD_TEXT:
    case WD_LTEXT:
        ret = (s ? ui_font_text_s(&t, font, px, d->x, d->y,
                                  d->op == WD_LTEXT ? locale_text(s) : s,
                                  d->rgb) : d->x) + back;
        break;
    case WD_CELLS: {
        /* a cell is a character: a Polish letter is two bytes of UTF-8
           and one column */
        int col = 0;
        for (int i = 0; s && s[i]; col++) {
            char one[5];
            int n = 0;
            one[n++] = s[i++];
            while (s[i] && ((uint8_t)s[i] & 0xC0) == 0x80 && n < 4)
                one[n++] = s[i++];
            one[n] = 0;
            if (one[0] != ' ')
                ui_font_text_s(&t, font, px, d->x + col * d->w, d->y, one,
                               d->rgb);
        }
        ret = d->x + col * d->w + back;
        break;
    }
    default:
        return -1;
    }
    return ret;
}

int64_t winmob_set_keyboard(int win, int owner, int on)
{
    irqflags_t f = spin_lock_irq(&lock);
    struct window *o = mine(win, owner);
    int said = o && o->keyboard != !!on;
    if (said) {
        o->keyboard = !!on;
        changes++;
    }
    spin_unlock_irq(&lock, f);
    if (said)
        printk("[winmob] window %d: keyboard %s\n", win, on ? "up" : "down");
    return o ? 0 : -1;
}

int winmob_keyboard(int win)
{
    return winmob_exists(win) && wins[win].keyboard;
}

/* What the keyboard typed: a byte at a time, as the PC's keys come */
void winmob_type(int win, const char *s)
{
    irqflags_t f = spin_lock_irq(&lock);
    if (winmob_exists(win))
        for (; *s; s++)
            push(&wins[win], WEV_KEY, 0, 0, (uint8_t)*s);
    spin_unlock_irq(&lock, f);
}

/* The display size changed: every window hears its new logical size */
void winmob_rescale(void)
{
    irqflags_t f = spin_lock_irq(&lock);
    for (int i = 0; i < WINMOB_MAX; i++)
        if (wins[i].used)
            push(&wins[i], WEV_SIZE, L(ww), L(wh),
                 WEV_SIZE_TOUCH | video_k() << 8);
    spin_unlock_irq(&lock, f);
}

/* The look changed: every window hears it, to draw itself again */
void winmob_look_changed(void)
{
    irqflags_t f = spin_lock_irq(&lock);
    for (int i = 0; i < WINMOB_MAX; i++)
        if (wins[i].used)
            push(&wins[i], WEV_LOOK, 0, 0, 0);
    spin_unlock_irq(&lock, f);
}

void winmob_media(int win, int what)
{
    irqflags_t f = spin_lock_irq(&lock);
    if (win >= 0 && win < WINMOB_MAX && wins[win].used)
        push(&wins[win], WEV_MEDIA, 0, 0, what);
    spin_unlock_irq(&lock, f);
}

int64_t winmob_poll(int win, int owner, struct win_event *ev)
{
    irqflags_t f = spin_lock_irq(&lock);
    struct window *o = mine(win, owner);
    int r = -1;
    if (o) {
        r = o->tail != o->head;
        if (r)
            *ev = o->ev[o->tail++ % EVENTS];
    }
    spin_unlock_irq(&lock, f);
    return r;
}

int64_t winmob_title(int win, int owner, const char *title)
{
    irqflags_t f = spin_lock_irq(&lock);
    struct window *o = mine(win, owner);
    if (o) {
        set_title(o, title);
        changes++;
    }
    spin_unlock_irq(&lock, f);
    return o ? 0 : -1;
}

void winmob_drop(int owner)
{
    for (int i = 0; i < WINMOB_MAX; i++) {
        irqflags_t f = spin_lock_irq(&lock);
        uint32_t *px = 0, pages = 0;
        if (wins[i].used && wins[i].owner == owner) {
            px = wins[i].px;
            pages = wins[i].pages;
            wins[i].used = 0;
            if (fresh == i)
                fresh = -1;
            changes++;
        }
        spin_unlock_irq(&lock, f);
        for (uint32_t k = 0; k < pages; k++)
            pmm_free_frame((uint32_t)(uintptr_t)px + k * PAGE_SIZE);
    }
}

/* --- the shell's side ----------------------------------------------------- */

uint32_t winmob_changes(void)
{
    return changes;
}

int winmob_take_new(void)
{
    irqflags_t f = spin_lock_irq(&lock);
    int id = fresh;
    fresh = -1;
    spin_unlock_irq(&lock, f);
    return id;
}

int winmob_exists(int win)
{
    return win >= 0 && win < WINMOB_MAX && wins[win].used;
}

int winmob_nth(int k)
{
    /* By the order they were made in: a window's tile keeps its place
       while others come and go before it */
    int ids[WINMOB_MAX], n = 0;
    irqflags_t f = spin_lock_irq(&lock);
    for (int i = 0; i < WINMOB_MAX; i++)
        if (wins[i].used) {
            int at = n++;
            while (at > 0 && wins[ids[at - 1]].order > wins[i].order) {
                ids[at] = ids[at - 1];
                at--;
            }
            ids[at] = i;
        }
    spin_unlock_irq(&lock, f);
    return k >= 0 && k < n ? ids[k] : -1;
}

int winmob_count(void)
{
    int n = 0;
    for (int i = 0; i < WINMOB_MAX; i++)
        n += wins[i].used;
    return n;
}

void winmob_name(int win, char *out, int max)
{
    irqflags_t f = spin_lock_irq(&lock);
    int i = 0;
    if (winmob_exists(win))
        for (; wins[win].title[i] && i < max - 1; i++)
            out[i] = wins[win].title[i];
    out[i] = 0;
    spin_unlock_irq(&lock, f);
}

/* The colour a row of a window's pixels has on the whole */
static uint32_t row_colour(const uint32_t *row)
{
    uint32_t sr = 0, sg = 0, sb = 0;
    for (int x = 0; x < ww; x++) {
        sr += (row[x] >> 16) & 0xFF;
        sg += (row[x] >> 8) & 0xFF;
        sb += row[x] & 0xFF;
    }
    return ww ? (sr / (uint32_t)ww) << 16 | (sg / (uint32_t)ww) << 8 |
                sb / (uint32_t)ww : 0;
}

/* The colour of a window's top row, or its bottom one - what the
   phone's status bar above it and the home indicator's strip below it
   take, so that a program's own colours go on to the screen's edges */
uint32_t winmob_edge_colour(int win, int bottom)
{
    irqflags_t f = spin_lock_irq(&lock);
    uint32_t c = 0;
    if (winmob_exists(win))
        c = row_colour(wins[win].px + (bottom ? (long)(wh - 1) * ww : 0));
    spin_unlock_irq(&lock, f);
    return c;
}

void winmob_draw(int win)
{
    irqflags_t f = spin_lock_irq(&lock);
    /* a window being drawn whole (WD_HOLD) keeps what the screen shows
       of it until it is done */
    if (winmob_exists(win) && !wins[win].held) {
        for (int y = 0; y < wh; y++) {
            uint32_t *row = video_prow32(wtop + y);
            if (row)
                memcpy(row, wins[win].px + (long)y * ww, (size_t)ww * 4);
        }
        /* Under it, to the screen's bottom - the home indicator's strip
           - the colour its last row has on the whole: a program's own
           bar goes on under the indicator, and a picture there does not
           smear down into streaks */
        uint32_t flat = row_colour(wins[win].px + (long)(wh - 1) * ww);
        for (int y = wtop + wh; y < video_ph(); y++) {
            uint32_t *row = video_prow32(y);
            for (int x = 0; row && x < ww; x++)
                row[x] = flat;
        }
    }
    spin_unlock_irq(&lock, f);
}

void winmob_touch(int win, int kind, int x, int y)
{
    x = L(x);
    y = L(y);
    irqflags_t f = spin_lock_irq(&lock);
    if (winmob_exists(win)) {
        struct window *w = &wins[win];
        if (kind == WEV_PRESS)
            push(w, WEV_CLICK, x, y, 1);     /* the left button, no keys */
        push(w, kind, x, y, kind == WEV_RELEASE ? 0 : 1);
    }
    spin_unlock_irq(&lock, f);
}
