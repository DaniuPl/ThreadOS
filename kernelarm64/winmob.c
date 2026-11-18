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
 * A window belongs to a program, not to the thread that made it, and
 * goes when the program ends.
 */

#include <stdint.h>
#include "arm64.h"
#include "winmob.h"
#include "../kernel/include/pmm.h"
#include "../kernel/include/printk.h"
#include "../kernel/include/spinlock.h"
#include "../kernel/include/string.h"
#include "../kernel/include/video.h"

#define EVENTS 128

struct window {
    int used;
    int owner;
    uint32_t order;              /* when it was made */
    char title[32];
    uint32_t *px;                /* w x h */
    uint32_t pages;
    struct win_event ev[EVENTS];
    uint32_t head, tail;
};

static struct window wins[WINMOB_MAX];
static struct spinlock lock = SPINLOCK_FREE;
static int ww, wtop, wh;         /* where windows go; 0 = no screen */
static volatile uint32_t changes;
static int fresh = -1;
static uint32_t made;

void winmob_init(int width, int top, int bottom)
{
    ww = width;
    wtop = top;
    wh = bottom - top;
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
    if (!ww)
        return -1;                   /* no screen */
    uint32_t pages = ((uint32_t)ww * (uint32_t)wh * 4 + PAGE_SIZE - 1) /
                     PAGE_SIZE;
    uint32_t *px = (uint32_t *)(uintptr_t)pmm_alloc_contiguous(pages);
    if (!px)
        return -1;
    /* White, as a page is before anything is drawn on it */
    for (uint32_t i = 0; i < (uint32_t)ww * (uint32_t)wh; i++)
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
        push(w, WEV_SIZE, ww, wh, WEV_SIZE_TOUCH);
        fresh = id;
        changes++;
    }
    spin_unlock_irq(&lock, f);
    if (id < 0) {
        for (uint32_t i = 0; i < pages; i++)
            pmm_free_frame((uint32_t)(uintptr_t)px + i * PAGE_SIZE);
        return -1;
    }
    printk("[winmob] window %d for program %d: \"%s\", %dx%d\n", id, owner,
           wins[id].title, ww, wh);
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
   while its own program is in a call */
int64_t winmob_blit32(int win, int owner, int x, int y, int w, int h,
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

void winmob_draw(int win)
{
    irqflags_t f = spin_lock_irq(&lock);
    if (winmob_exists(win))
        for (int y = 0; y < wh; y++) {
            uint32_t *row = video_prow32(wtop + y);
            if (row)
                memcpy(row, wins[win].px + (long)y * ww, (size_t)ww * 4);
        }
    spin_unlock_irq(&lock, f);
}

void winmob_touch(int win, int kind, int x, int y)
{
    irqflags_t f = spin_lock_irq(&lock);
    if (winmob_exists(win)) {
        struct window *w = &wins[win];
        if (kind == WEV_PRESS)
            push(w, WEV_CLICK, x, y, 1);     /* the left button, no keys */
        push(w, kind, x, y, kind == WEV_RELEASE ? 0 : 1);
    }
    spin_unlock_irq(&lock, f);
}
