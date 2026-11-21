/* Kontrolki: the part of a window that every program was writing again
 *
 * Look at any graphical program in this system before this file and
 * the same thing is in all of them: a rectangle drawn where a button
 * should be, a caption drawn on top of it, and then, in the event
 * loop, arithmetic - is the click inside those coordinates, is it the
 * first button or the second, which row of the list is (y - 20) / 12.
 * Six programs, six copies of the same arithmetic, and every one of
 * them a place where a button can end up one pixel from where the
 * click is expected.
 *
 * So the window becomes a LIST OF THINGS instead of a sequence of
 * drawing calls. A program says what it has - a label here, a button
 * there, a slider from zero to a hundred - and this layer draws them,
 * decides what a click landed on, moves the keyboard focus between
 * them and reports what happened as "the third one was pressed".
 *
 * It does not own the loop or keep a program's data. Controls accept
 * pixel rectangles for compatibility; ug_layout and ug_workspace_layout
 * can compute those rectangles so applications do not repeat layout
 * arithmetic in their draw and event paths.
 *
 * It draws the way the desktop draws (udraw.h): TrueType, round
 * corners, the theme's colours - asked for every time it draws, so a
 * window follows a switch between dark and light as the desktop does.
 * A program that says a thing's colour says it by name: UG_BLUE is the
 * accent, UG_RED is "careful" - whatever those are in the theme.
 */

#ifndef THREADOS_UGUI_H
#define THREADOS_UGUI_H

#include "udraw.h"

/* Colours by the names the programs already use - meanings rather than
   colours: the theme says what they look like */
#define UG_BLACK 0
#define UG_RED   1               /* danger */
#define UG_BLUE  3               /* the accent */
#define UG_WHITE 4
#define UG_BG    8
#define UG_DIM   15

#define UG_MAX   24              /* things in one window */
#define UG_ROW   18              /* one row of text */
#define UG_R     6               /* the corners of a button, a field */

/* What a thing is */
#define UG_LABEL  0
#define UG_BUTTON 1
#define UG_SLIDER 2
#define UG_CHECK  3
#define UG_FIELD  4
#define UG_LIST   5
#define UG_LINE   6
#define UG_TABS   7
/* ProtonUI v2's (docs/protonui-plan.md stage 4) */
#define UG_SWITCH   8            /* on or off, as a switch */
#define UG_SEGMENTS 9            /* one of a few, side by side */
#define UG_PROGRESS 10           /* how far: value of max, or -1 unknown */
#define UG_KNOTS    11           /* one of a few, one under the other */

/* What happened */
#define UG_NOTHING 0
#define UG_PRESSED 1             /* a button */
#define UG_CHANGED 2             /* a slider, a box, a row of a list */
#define UG_TYPED   3             /* a field's text changed */
#define UG_ENTER   4             /* Enter in a field */
#define UG_CLOSED  5             /* the window is to go */
#define UG_RESIZED 6
#define UG_FOCUSED 7             /* Tab gave a thing the keys (pu_start) */

struct ug_item {
    int kind;
    int x, y, w, h;
    const char *text;            /* a caption, or a label's text */
    int value;                   /* slider position, tick, chosen row */
    int max;                     /* slider maximum, rows in a list */
    int colour;                  /* the accent; 0 = this layer decides */
    int dim;                     /* a quieter label */
    int enabled;
    char *buf;                   /* a field's text, owned by the program */
    int buflen;
    const char *(*row)(void *ctx, int i);   /* a list's rows */
    void *ctx;
    const char *name;            /* what it is called, for a screen reader:
                                    its text unless pu_name says otherwise */
    int kind2;                   /* a button's kind (PU_PRIMARY..) */
    int phase;                   /* an unknown progress's place, of 1000 */
};

struct ug {
    int win;
    int w, h;
    struct ud_look look;         /* the theme's colours, as of the last draw */
    struct ug_item own[UG_MAX];  /* the things, unless pu_start was given
                                    room of the program's own */
    struct ug_item *it;
    int cap;
    int n;
    int focus;                   /* which thing has the keyboard, -1 = none */
    int top;                     /* the first visible row of a list */
    int v2;                      /* started by pu_start: the keys move the
                                    focus, and it is drawn */
};

/* --- rectangles and reusable layout -------------------------------- */

#define UG_HORIZONTAL 0
#define UG_VERTICAL   1
#define UG_LAYOUT_MAX 16

struct ug_rect {
    int x, y, w, h;
};

struct ug_layout_slot {
    struct ug_rect rect;
    int size;
    int weight;
    int minimum;
};

struct ug_layout {
    struct ug_rect bounds;
    struct ug_layout_slot slots[UG_LAYOUT_MAX];
    int axis, padding, gap, n;
};

static inline int ug_layout_start(struct ug_layout *l, struct ug_rect bounds,
                                  int axis, int padding, int gap)
{
    if (!l || (axis != UG_HORIZONTAL && axis != UG_VERTICAL) ||
        bounds.w < 0 || bounds.h < 0 || padding < 0 || gap < 0)
        return -1;
    l->bounds = bounds;
    l->axis = axis;
    l->padding = padding;
    l->gap = gap;
    l->n = 0;
    return 0;
}

static inline int ug_layout_push(struct ug_layout *l, int size, int weight,
                                 int minimum)
{
    if (!l || l->n >= UG_LAYOUT_MAX || size < 0 || weight < 0 ||
        minimum < 0 || (weight && size))
        return -1;
    int id = l->n++;
    l->slots[id].size = size;
    l->slots[id].weight = weight;
    l->slots[id].minimum = minimum;
    l->slots[id].rect = (struct ug_rect){ 0, 0, 0, 0 };
    return id;
}

static inline int ug_layout_fixed(struct ug_layout *l, int size)
{
    return ug_layout_push(l, size, 0, 0);
}

static inline int ug_layout_flex(struct ug_layout *l, int weight, int minimum)
{
    if (weight <= 0)
        return -1;
    return ug_layout_push(l, 0, weight, minimum);
}

static inline int ug_layout_run(struct ug_layout *l)
{
    if (!l)
        return -1;
    int main = l->axis == UG_HORIZONTAL ? l->bounds.w : l->bounds.h;
    int cross = l->axis == UG_HORIZONTAL ? l->bounds.h : l->bounds.w;
    if (l->n < 1 || l->padding > main / 2 || l->padding > cross / 2)
        return -1;

    int available = main - 2 * l->padding;
    if (l->n > 1) {
        if (l->gap > available / (l->n - 1))
            return -1;
        available -= l->gap * (l->n - 1);
    }
    int cross_size = cross - 2 * l->padding;
    int fixed = 0, minimum = 0, total_weight = 0, last_flex = -1;
    for (int i = 0; i < l->n; i++) {
        struct ug_layout_slot *s = &l->slots[i];
        if (!s->weight) {
            if (s->size > available - fixed)
                return -1;
            fixed += s->size;
        } else {
            if (s->minimum > available - fixed - minimum ||
                s->weight > 1000000 - total_weight)
                return -1;
            minimum += s->minimum;
            total_weight += s->weight;
            last_flex = i;
        }
    }
    if (fixed + minimum > available)
        return -1;
    int extra = available - fixed - minimum;
    int assigned = 0, cursor = l->padding;
    for (int i = 0; i < l->n; i++) {
        struct ug_layout_slot *s = &l->slots[i];
        int size = s->size;
        if (s->weight) {
            int share = i == last_flex
                            ? extra - assigned
                            /* pixels times a weight: far inside 32
                               bits */
                            : (int)((unsigned int)extra *
                                    (unsigned int)s->weight /
                                    (unsigned int)total_weight);
            assigned += share;
            size = s->minimum + share;
        }
        if (l->axis == UG_HORIZONTAL) {
            s->rect = (struct ug_rect){
                l->bounds.x + cursor, l->bounds.y + l->padding,
                size, cross_size
            };
        } else {
            s->rect = (struct ug_rect){
                l->bounds.x + l->padding, l->bounds.y + cursor,
                cross_size, size
            };
        }
        cursor += size + l->gap;
    }
    return 0;
}

static inline int ug_layout_rect(const struct ug_layout *l, int id,
                                 struct ug_rect *rect)
{
    if (!l || !rect || id < 0 || id >= l->n)
        return -1;
    *rect = l->slots[id].rect;
    return 0;
}

/* A reusable editor-style frame; content areas are returned, not owned. */
struct ug_workspace {
    struct ug_rect toolbar, activity, sidebar, tabs, breadcrumbs;
    struct ug_rect editor, panel, status;
};

static inline int ug_workspace_layout(struct ug_workspace *w, int width,
                                      int height, int toolbar_h,
                                      int activity_w, int sidebar_w,
                                      int tabs_h, int breadcrumb_h,
                                      int panel_h, int status_h)
{
    if (!w || width < 0 || height < 0 || toolbar_h < 0 || activity_w < 0 ||
        sidebar_w < 0 || tabs_h < 0 || breadcrumb_h < 0 || panel_h < 0 ||
        status_h < 0)
        return -1;

    struct ug_layout vertical, columns, content;
    struct ug_rect full = { 0, 0, width, height }, work, center;
    int toolbar, body, status, activity, sidebar, editor;
    int tabs, breadcrumbs, panel;

    if (ug_layout_start(&vertical, full, UG_VERTICAL, 0, 0) < 0)
        return -1;
    toolbar = ug_layout_fixed(&vertical, toolbar_h);
    body = ug_layout_flex(&vertical, 1, 0);
    status = ug_layout_fixed(&vertical, status_h);
    if (toolbar < 0 || body < 0 || status < 0 ||
        ug_layout_run(&vertical) < 0 ||
        ug_layout_rect(&vertical, toolbar, &w->toolbar) < 0 ||
        ug_layout_rect(&vertical, body, &work) < 0 ||
        ug_layout_rect(&vertical, status, &w->status) < 0)
        return -1;

    if (ug_layout_start(&columns, work, UG_HORIZONTAL, 0, 0) < 0)
        return -1;
    activity = ug_layout_fixed(&columns, activity_w);
    sidebar = ug_layout_fixed(&columns, sidebar_w);
    editor = ug_layout_flex(&columns, 1, 1);
    if (activity < 0 || sidebar < 0 || editor < 0 ||
        ug_layout_run(&columns) < 0 ||
        ug_layout_rect(&columns, activity, &w->activity) < 0 ||
        ug_layout_rect(&columns, sidebar, &w->sidebar) < 0 ||
        ug_layout_rect(&columns, editor, &center) < 0)
        return -1;

    if (ug_layout_start(&content, center, UG_VERTICAL, 0, 0) < 0)
        return -1;
    tabs = ug_layout_fixed(&content, tabs_h);
    breadcrumbs = ug_layout_fixed(&content, breadcrumb_h);
    editor = ug_layout_flex(&content, 1, 1);
    panel = ug_layout_fixed(&content, panel_h);
    if (tabs < 0 || breadcrumbs < 0 || editor < 0 || panel < 0 ||
        ug_layout_run(&content) < 0 ||
        ug_layout_rect(&content, tabs, &w->tabs) < 0 ||
        ug_layout_rect(&content, breadcrumbs, &w->breadcrumbs) < 0 ||
        ug_layout_rect(&content, editor, &w->editor) < 0 ||
        ug_layout_rect(&content, panel, &w->panel) < 0)
        return -1;
    return 0;
}

static inline void ug_fill_rect(int win, struct ug_rect rect, uint32_t colour)
{
    if (rect.w > 0 && rect.h > 0)
        ud_fill(win, rect.x, rect.y, rect.w, rect.h, colour);
}

static inline void ug_workspace_draw(int win, const struct ug_workspace *w,
                                     const struct ud_look *look)
{
    if (!w || !look)
        return;
    ug_fill_rect(win, w->toolbar, look->bg);
    ug_fill_rect(win, w->activity, look->row);
    ug_fill_rect(win, w->sidebar, look->bg);
    ug_fill_rect(win, w->tabs, look->bg);
    ug_fill_rect(win, w->breadcrumbs, look->field);
    ug_fill_rect(win, w->editor, look->field);
    ug_fill_rect(win, w->panel, look->field);
    ug_fill_rect(win, w->status, look->accent);
    if (w->sidebar.w > 0)
        ud_fill(win, w->sidebar.x + w->sidebar.w - 1, w->sidebar.y, 1,
                w->sidebar.h, look->line);
    if (w->tabs.h > 0)
        ud_fill(win, w->tabs.x, w->tabs.y + w->tabs.h - 1,
                w->tabs.w, 1, look->line);
    if (w->breadcrumbs.h > 0)
        ud_fill(win, w->breadcrumbs.x,
                w->breadcrumbs.y + w->breadcrumbs.h - 1,
                w->breadcrumbs.w, 1, look->line);
    if (w->panel.h > 0)
        ud_fill(win, w->panel.x, w->panel.y, w->panel.w, 1, look->line);
}

/* --- putting a window together -------------------------------------- */

static inline void ug_start(struct ug *u, int win, int w, int h)
{
    u->win = win;
    u->w = w;
    u->h = h;
    u->it = u->own;
    u->cap = UG_MAX;
    u->n = 0;
    u->focus = -1;
    u->top = 0;
    u->v2 = 0;
}

/* ProtonUI v2's start: the things in an array the program hands in (as
   many as it likes; NULL for the 24 of its own), and Tab, Space and
   Enter work the controls, the one with the keys wrapped in the thread */
static inline void pu_start(struct ug *u, int win, int w, int h,
                            struct ug_item *items, int cap)
{
    ug_start(u, win, w, h);
    if (items && cap > 0) {
        u->it = items;
        u->cap = cap;
    }
    u->v2 = 1;
}

static inline int ug_add(struct ug *u, int kind, int x, int y, int w, int h)
{
    if (u->n >= u->cap)
        return -1;

    struct ug_item *t = &u->it[u->n];
    t->kind = kind;
    t->x = x; t->y = y; t->w = w; t->h = h;
    t->text = "";
    t->value = 0;
    t->max = 100;
    t->colour = 0;
    t->dim = 0;
    t->enabled = 1;
    t->buf = (char *)0;
    t->buflen = 0;
    t->row = (const char *(*)(void *, int))0;
    t->ctx = (void *)0;
    t->name = (const char *)0;
    t->kind2 = 0;
    t->phase = 0;
    return u->n++;
}

static inline int ug_label(struct ug *u, int x, int y, const char *text)
{
    int i = ug_add(u, UG_LABEL, x, y, 0, UG_ROW);
    if (i >= 0)
        u->it[i].text = text;
    return i;
}

static inline int ug_note(struct ug *u, int x, int y, const char *text)
{
    int i = ug_label(u, x, y, text);
    if (i >= 0)
        u->it[i].dim = 1;
    return i;
}

static inline int ug_button(struct ug *u, int x, int y, int w, int h,
                            const char *text)
{
    int i = ug_add(u, UG_BUTTON, x, y, w, h);
    if (i >= 0)
        u->it[i].text = text;
    return i;
}

static inline int ug_slider(struct ug *u, int x, int y, int w, int h,
                            int value, int max)
{
    int i = ug_add(u, UG_SLIDER, x, y, w, h);
    if (i >= 0) {
        u->it[i].value = value;
        u->it[i].max = max > 0 ? max : 100;
    }
    return i;
}

static inline int ug_check(struct ug *u, int x, int y, const char *text,
                           int on)
{
    int i = ug_add(u, UG_CHECK, x, y, 0, UG_ROW);
    if (i >= 0) {
        u->it[i].text = text;
        u->it[i].value = on ? 1 : 0;
    }
    return i;
}

static inline int ug_field(struct ug *u, int x, int y, int w, char *buf,
                           int buflen)
{
    int i = ug_add(u, UG_FIELD, x, y, w, UG_ROW + 4);
    if (i >= 0) {
        u->it[i].buf = buf;
        u->it[i].buflen = buflen;
    }
    return i;
}

static inline int ug_list(struct ug *u, int x, int y, int w, int h,
                          const char *(*row)(void *ctx, int i), void *ctx,
                          int rows)
{
    int i = ug_add(u, UG_LIST, x, y, w, h);
    if (i >= 0) {
        u->it[i].row = row;
        u->it[i].ctx = ctx;
        u->it[i].max = rows;
        u->it[i].value = -1;
    }
    return i;
}

static inline int ug_tabs(struct ug *u, int x, int y, int w, int h,
                          const char *(*row)(void *ctx, int i), void *ctx,
                          int count, int active)
{
    if (count < 0 || active < -1 || active >= count)
        return -1;
    int i = ug_add(u, UG_TABS, x, y, w, h);
    if (i >= 0) {
        u->it[i].row = row;
        u->it[i].ctx = ctx;
        u->it[i].max = count;
        u->it[i].value = active;
    }
    return i;
}

static inline int ug_line(struct ug *u, int x, int y, int w)
{
    return ug_add(u, UG_LINE, x, y, w, 1);
}

/* --- ProtonUI v2's things -------------------------------------------- */

#define PU_SWITCH_W 40
#define PU_SWITCH_H 22
#define PU_KNOT_ROW 24           /* one choice among knots */

/* A switch: on (1) or off (0), its words to the left of it at x */
static inline int pu_switch(struct ug *u, int x, int y, int w,
                            const char *text, int on)
{
    int i = ug_add(u, UG_SWITCH, x, y, w, PU_SWITCH_H + 4);
    if (i >= 0) {
        u->it[i].text = text;
        u->it[i].value = on ? 1 : 0;
    }
    return i;
}

/* Segments: n choices side by side, 'chosen' of them */
static inline int pu_segments(struct ug *u, int x, int y, int w, int h,
                              const char *(*row)(void *ctx, int i), void *ctx,
                              int n, int chosen)
{
    int i = ug_add(u, UG_SEGMENTS, x, y, w, h);
    if (i >= 0) {
        u->it[i].row = row;
        u->it[i].ctx = ctx;
        u->it[i].max = n;
        u->it[i].value = chosen;
    }
    return i;
}

/* Progress: value of max; a value of -1 is a length not known, whose
   piece of thread pu_set_phase moves along */
static inline int pu_progress(struct ug *u, int x, int y, int w, int value,
                              int max)
{
    int i = ug_add(u, UG_PROGRESS, x, y, w, 10);
    if (i >= 0) {
        u->it[i].value = value;
        u->it[i].max = max > 0 ? max : 100;
    }
    return i;
}

/* Knots: n choices one under the other, 'chosen' of them */
static inline int pu_knots(struct ug *u, int x, int y, int w,
                           const char *(*row)(void *ctx, int i), void *ctx,
                           int n, int chosen)
{
    int i = ug_add(u, UG_KNOTS, x, y, w, n * PU_KNOT_ROW);
    if (i >= 0) {
        u->it[i].row = row;
        u->it[i].ctx = ctx;
        u->it[i].max = n;
        u->it[i].value = chosen;
    }
    return i;
}

/* A button's kind: the one thing to do (the accent), another thing,
   a quiet one (words only) and one that destroys */
#define PU_PRIMARY   0
#define PU_SECONDARY 1
#define PU_GHOST     2
#define PU_DANGER    3

static inline void pu_kind(struct ug *u, int id, int kind)
{
    if (id >= 0 && id < u->n)
        u->it[id].kind2 = kind;
}

static inline void pu_set_phase(struct ug *u, int id, int phase)
{
    if (id >= 0 && id < u->n)
        u->it[id].phase = phase;
}

static inline void pu_name(struct ug *u, int id, const char *name)
{
    if (id >= 0 && id < u->n)
        u->it[id].name = name;
}

/* What a thing is called, for whoever reads the window aloud */
static inline const char *pu_name_of(const struct ug *u, int id)
{
    if (id < 0 || id >= u->n)
        return "";
    const struct ug_item *t = &u->it[id];
    return t->name ? t->name : t->text ? t->text : "";
}

static inline void pu_enable(struct ug *u, int id, int on)
{
    if (id >= 0 && id < u->n)
        u->it[id].enabled = on ? 1 : 0;
}

/* The v1 names under v2's, so a program can be written in either */
#define pu_label    ug_label
#define pu_note     ug_note
#define pu_button   ug_button
#define pu_slider   ug_slider
#define pu_check    ug_check
#define pu_field    ug_field
#define pu_list     ug_list
#define pu_tabs     ug_tabs
#define pu_line     ug_line
#define pu_value    ug_value
#define pu_set_value ug_set_value
#define pu_set_text ug_set_text

/* --- reading and setting ------------------------------------------- */

static inline int ug_value(struct ug *u, int id)
{
    return (id >= 0 && id < u->n) ? u->it[id].value : 0;
}

static inline void ug_set_value(struct ug *u, int id, int value)
{
    if (id >= 0 && id < u->n)
        u->it[id].value = value;
}

static inline void ug_set_text(struct ug *u, int id, const char *text)
{
    if (id >= 0 && id < u->n)
        u->it[id].text = text;
}

static inline void ug_set_colour(struct ug *u, int id, int colour)
{
    if (id >= 0 && id < u->n)
        u->it[id].colour = colour;
}

static inline void ug_set_rows(struct ug *u, int id, int rows)
{
    if (id >= 0 && id < u->n)
        u->it[id].max = rows;
}

/* --- drawing --------------------------------------------------------- */

static inline int ug_len(const char *s)
{
    int n = 0;
    while (s && s[n])
        n++;
    return n;
}

/* Backspace in a field: the last letter, all of it - a Polish letter
   is two bytes of UTF-8 */
static inline void ug_rub(char *b)
{
    int n = ug_len(b);
    while (n > 0) {
        char gone = b[--n];
        b[n] = '\0';
        if (((unsigned char)gone & 0xC0) != 0x80)
            break;
    }
}

/* A key that is typing: printable ASCII, or a byte of a letter beyond */
static inline int ug_typed(char c)
{
    return (unsigned char)c >= 0x20 && c != 0x7F;
}

/* A colour by its meaning */
static inline uint32_t ug_rgb(const struct ud_look *l, int colour)
{
    switch (colour) {
    case UG_RED:   return l->danger;
    case UG_BLUE:  return l->accent;
    case UG_DIM:   return l->dim;
    case UG_WHITE: return l->on_accent;
    case UG_BG:    return l->bg;
    default:       return l->text;
    }
}

/* Text placed in a row, as the desktop places it */
static inline int ug_say(int win, int x, int y, int h, const char *s,
                         uint32_t rgb, int font)
{
    return ud_text(win, x, ud_baseline(y, h, UD_PX), s, rgb, font, UD_PX);
}

static inline int ug_say_l(int win, int x, int y, int h, const char *s,
                           uint32_t rgb, int font)
{
    return ud_text_l(win, x, ud_baseline(y, h, UD_PX), s, rgb, font, UD_PX);
}

/* --- ProtonUI v2: Weave's controls ------------------------------------
 *
 * docs/weave/canvas/Components.dc.html. Under Weave (the look's style)
 * a control is cloth like everything else: a button a raised surface
 * with a line round it, or the accent for the one thing to do, or
 * madder for what destroys; a field a surface with a stitch under it
 * that the focus pulls tight into the thread; a switch a pill with its
 * knob; a choice among a few a knot, hollow or filled; the keyboard's
 * focus the thread wrapped round the control two pixels out. Under the
 * other styles everything is drawn as it always was. The colours the
 * look does not carry are the design's, here, for Night and Linen.
 */
#define PU_N_RAISED      0x20232B
#define PU_N_RAISED_LINE 0x2E323D
#define PU_N_FIELD       0x202229   /* (a shade lighter than the design's
                                       0x1B1D24, which sits on a darker
                                       card there than a window is) */
#define PU_N_FIELD_ON    0x262931
#define PU_N_STITCH      0x3A3F4C
#define PU_N_KNOT        0x4A4F5C
#define PU_N_SEG_ON      0x2A2D36
#define PU_N_SEG_INK     0xB4B9C6
#define PU_N_LINK        0x8FA5FF
#define PU_N_DANGER_INK  0xFF8A8D
#define PU_N_OFF_INK     0x6A7080
#define PU_N_PRESSED     0x4A68E0
#define PU_N_KNOB_OFF    0x8A90A0
#define PU_L_RAISED      0xFBF9F4
#define PU_L_RAISED_LINE 0xCFC8BA
#define PU_L_FIELD       0xEFEBE3
#define PU_L_FIELD_ON    0xFFFFFF
#define PU_L_STITCH      0xB4AEA2
#define PU_L_KNOT        0xA39D91
#define PU_L_SEG_ON      0xFFFFFF
#define PU_L_SEG_INK     0x5E5A52
#define PU_L_LINK        0x3F5EE8
#define PU_L_DANGER_INK  0xC9302C
#define PU_L_OFF_INK     0xA39D91
#define PU_L_PRESSED     0x3450CC
#define PU_L_KNOB_OFF    0x8E897F
#define PU_BTN_R    8            /* a button's corners */
#define PU_SEL_A    46           /* the chosen row: the accent at this, of 255 */
#define PU_DANGER_A 26           /* a destroying button's ground ... */
#define PU_DANGER_LINE_A 128     /* ... and its line */

struct pu_ink {
    uint32_t raised, raised_line, field, field_on, stitch, knot, seg_on,
             seg_ink, link, danger_ink, off_ink, pressed, knob_off;
};

static inline int pu_weave(const struct ud_look *l)
{
    return l->style == UD_STYLE_WEAVE;
}

static inline void pu_ink_of(const struct ud_look *l, struct pu_ink *p)
{
    int d = (int)l->dark;
    p->raised = d ? PU_N_RAISED : PU_L_RAISED;
    p->raised_line = d ? PU_N_RAISED_LINE : PU_L_RAISED_LINE;
    p->field = d ? PU_N_FIELD : PU_L_FIELD;
    p->field_on = d ? PU_N_FIELD_ON : PU_L_FIELD_ON;
    p->stitch = d ? PU_N_STITCH : PU_L_STITCH;
    p->knot = d ? PU_N_KNOT : PU_L_KNOT;
    p->seg_on = d ? PU_N_SEG_ON : PU_L_SEG_ON;
    p->seg_ink = d ? PU_N_SEG_INK : PU_L_SEG_INK;
    p->link = d ? PU_N_LINK : PU_L_LINK;
    p->danger_ink = d ? PU_N_DANGER_INK : PU_L_DANGER_INK;
    p->off_ink = d ? PU_N_OFF_INK : PU_L_OFF_INK;
    p->pressed = d ? PU_N_PRESSED : PU_L_PRESSED;
    p->knob_off = d ? PU_N_KNOB_OFF : PU_L_KNOB_OFF;
}

/* 'over' laid on 'under' at a of 255, as the desktop mixes */
static inline uint32_t pu_mix(uint32_t over, uint32_t under, int a)
{
    uint32_t out = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        uint32_t o = (over >> sh) & 0xFF, u = (under >> sh) & 0xFF;
        out |= ((o * (uint32_t)a + u * (uint32_t)(255 - a) + 127) / 255) << sh;
    }
    return out;
}

/* The thread wrapped round a control with the keyboard's focus, two
   pixels out and two thick */
static inline void pu_draw_focus(int win, const struct ud_look *l, int x,
                                 int y, int w, int h, int r)
{
    ud_ring(win, x - 3, y - 3, w + 6, h + 6, r + 3, l->accent);
    ud_ring(win, x - 4, y - 4, w + 8, h + 8, r + 4, l->accent);
}

/* States a control is drawn in */
#define PU_FOCUS   1             /* it has the keys */
#define PU_DOWN    2             /* pressed, while the pointer holds it */
#define PU_OFF     4             /* not to be used now */
#define PU_ERROR   8             /* a field whose text is wrong */
#define PU_TR     16             /* its words are to be translated */

/* A button. Under Weave by its kind; under the other styles as v1
   drew it, the accent's or danger's face, a row's for the rest */
static inline void pu_draw_button(int win, const struct ud_look *l, int x,
                                  int y, int w, int h, const char *text,
                                  int kind, int state)
{
    uint32_t face, ink, line = 0;
    int font = UD_SANS;
    if (!pu_weave(l)) {
        face = kind == PU_DANGER ? l->danger : kind == PU_PRIMARY ? l->accent
                                                                  : l->row;
        ink = kind == PU_PRIMARY || kind == PU_DANGER ? l->on_accent : l->text;
        if (state & PU_OFF) {
            face = l->row;
            ink = l->dim;
        }
        if (kind != PU_GHOST)
            ud_round(win, x, y, w, h, UG_R, face);
    } else {
        struct pu_ink p;
        pu_ink_of(l, &p);
        int r = h / 2 < PU_BTN_R ? h / 2 : PU_BTN_R;
        if (state & PU_OFF) {
            ud_ring(win, x, y, w, h, r, p.stitch);
            ink = p.off_ink;
        } else if (kind == PU_PRIMARY) {
            ud_round(win, x, y, w, h, r, (state & PU_DOWN) ? p.pressed
                                                           : l->accent);
            ink = l->on_accent;
            font = UD_BOLD;
        } else if (kind == PU_DANGER) {
            face = pu_mix(l->danger, l->bg, PU_DANGER_A);
            line = pu_mix(l->danger, l->bg, PU_DANGER_LINE_A);
            ud_round(win, x, y, w, h, r, face);
            ud_ring(win, x, y, w, h, r, line);
            ink = p.danger_ink;
            font = UD_BOLD;
        } else if (kind == PU_GHOST) {
            ink = p.link;
        } else {
            ud_round(win, x, y, w, h, r, (state & PU_DOWN) ? p.seg_on
                                                           : p.raised);
            ud_ring(win, x, y, w, h, r, p.raised_line);
            ink = l->text;
        }
        if (state & PU_FOCUS)
            pu_draw_focus(win, l, x, y, w, h, r);
    }
    if (state & PU_TR)
        ud_text_mid_l(win, x, y, w, h, text, ink, font, UD_PX);
    else
        ud_text_mid(win, x, y, w, h, text, ink, font, UD_PX);
}

/* A field under Weave: its surface, a little lighter with the keys, and
   the stitch under it - dashed at rest, the thread when it has the keys,
   madder when what is in it is wrong */
static inline void pu_draw_field_ground(int win, const struct ud_look *l,
                                        int x, int y, int w, int h, int state)
{
    struct pu_ink p;
    pu_ink_of(l, &p);
    ud_round(win, x, y, w, h, 6, (state & PU_FOCUS) ? p.field_on : p.field);
    if (state & PU_ERROR)
        ud_fill(win, x, y + h - 2, w, 2, l->danger);
    else if (state & PU_FOCUS)
        ud_fill(win, x, y + h - 2, w, 2, l->accent);
    else
        ud_stitch(win, x, y + h - 2, w, 0, p.stitch);
}

/* A field to type in: a white (or dark) surface, a line round it in
   the accent when it has the keys, and where the next letter goes */
static inline void ug_draw_field(int win, const struct ud_look *l, int x,
                                 int y, int w, int h, const char *text,
                                 int focused)
{
    if (pu_weave(l)) {
        pu_draw_field_ground(win, l, x, y, w, h, focused ? PU_FOCUS : 0);
        int end = ug_say(win, x + 10, y - 1, h, text ? text : "", l->text,
                         UD_SANS);
        if (focused && end < x + w - 4)
            ud_fill(win, end + 1, y + 4, 1, h - 9, l->text);
        return;
    }
    ud_round(win, x, y, w, h, UG_R, l->field);
    ud_ring(win, x, y, w, h, UG_R, focused ? l->accent : l->line);
    int end = ug_say(win, x + 6, y, h, text ? text : "", l->text, UD_SANS);
    if (focused && end < x + w - 4)
        ud_fill(win, end + 1, y + 4, 1, h - 8, l->text);
}

/* Under Weave a v1 button's kind, from the colours it was asked for:
   the accent's face is the one thing to do, danger's face or words the
   destroying one, a quiet face with quiet words one not to be used now,
   any other face another thing */
static inline int pu_kind_of_face(const struct ud_look *l, uint32_t face,
                                  uint32_t ink, int *state)
{
    if (face == l->accent)
        return PU_PRIMARY;
    if (face == l->danger || ink == l->danger)
        return PU_DANGER;
    if (ink == l->dim)
        *state |= PU_OFF;
    return PU_SECONDARY;
}

/* A button: a rounded surface with its word in the middle */
static inline void ug_draw_button(int win, const struct ud_look *l, int x,
                                  int y, int w, int h, const char *text,
                                  uint32_t face, uint32_t ink)
{
    if (pu_weave(l)) {
        int state = 0;
        int kind = pu_kind_of_face(l, face, ink, &state);
        pu_draw_button(win, l, x, y, w, h, text, kind, state);
        return;
    }
    ud_round(win, x, y, w, h, UG_R, face);
    ud_text_mid(win, x, y, w, h, text, ink, UD_SANS, UD_PX);
}

static inline void ug_draw_button_l(int win, const struct ud_look *l, int x,
                                    int y, int w, int h, const char *text,
                                    uint32_t face, uint32_t ink)
{
    if (pu_weave(l)) {
        int state = PU_TR;
        int kind = pu_kind_of_face(l, face, ink, &state);
        pu_draw_button(win, l, x, y, w, h, text, kind, state);
        return;
    }
    ud_round(win, x, y, w, h, UG_R, face);
    ud_text_mid_l(win, x, y, w, h, text, ink, UD_SANS, UD_PX);
}

/* A box ticked or not, under Weave: the accent's with its tick, or a
   ring of the knots' grey */
static inline void pu_draw_check(int win, const struct ud_look *l, int x,
                                 int y, int on, int state)
{
    struct pu_ink p;
    pu_ink_of(l, &p);
    int b = 16;
    if (on) {
        ud_round(win, x, y, b, b, 4, l->accent);
        ud_line(win, x + 4, y + 8, x + 7, y + 11, 30, l->on_accent);
        ud_line(win, x + 7, y + 11, x + 12, y + 5, 30, l->on_accent);
    } else {
        ud_ring(win, x, y, b, b, 4, p.knot);
        ud_ring(win, x + 1, y + 1, b - 2, b - 2, 3, p.knot);
    }
    if (state & PU_FOCUS)
        pu_draw_focus(win, l, x, y, b, b, 4);
}

/* A switch, PU_SWITCH_W by PU_SWITCH_H at x, y: on, the accent with the
   knob at its right; off, a quiet pill with a smaller grey knob at its
   left */
static inline void pu_draw_switch(int win, const struct ud_look *l, int x,
                                  int y, int on, int state)
{
    struct pu_ink p;
    pu_ink_of(l, &p);
    int w = PU_SWITCH_W, h = PU_SWITCH_H;
    if (!pu_weave(l)) {
        ud_round(win, x, y, w, h, h / 2, on ? l->accent : l->row);
        ud_disc(win, on ? x + w - h + 2 : x + 2, y + 2, h - 4,
                l->dark ? 0xEDEDF0 : 0xFFFFFF);
        return;
    }
    if (on) {
        ud_round(win, x, y, w, h, h / 2, l->accent);
        ud_disc(win, x + w - 3 - 16, y + 3, 16, 0xFFFFFF);
    } else {
        ud_round(win, x, y, w, h, h / 2, p.seg_on);
        ud_ring(win, x, y, w, h, h / 2, p.knot);
        ud_disc(win, x + 4, y + 4, 14, p.knob_off);
    }
    if (state & PU_FOCUS)
        pu_draw_focus(win, l, x, y, w, h, h / 2);
}

/* A knot to choose with: hollow, or filled in the accent when chosen */
static inline void pu_draw_knot(int win, const struct ud_look *l, int x,
                                int y, int chosen, int state)
{
    struct pu_ink p;
    pu_ink_of(l, &p);
    uint32_t c = chosen ? l->accent : (pu_weave(l) ? p.knot : l->line);
    ud_ring(win, x, y, 16, 16, 8, c);
    ud_ring(win, x + 1, y + 1, 14, 14, 7, c);
    if (chosen)
        ud_disc(win, x + 4, y + 4, 8, l->accent);
    if (state & PU_FOCUS)
        pu_draw_focus(win, l, x, y, 16, 16, 8);
}

/* Segments: n words side by side in one strip, the chosen one raised
   with the thread along its foot */
static inline void pu_draw_segments(int win, const struct ud_look *l, int x,
                                    int y, int w, int h,
                                    const char *(*word)(void *ctx, int i),
                                    void *ctx, int n, int chosen, int state)
{
    struct pu_ink p;
    pu_ink_of(l, &p);
    if (n <= 0 || w <= 6 || h <= 6)
        return;
    int weave = pu_weave(l);
    ud_round(win, x, y, w, h, weave ? 9 : UG_R, weave ? p.field : l->row);
    if (weave)
        ud_ring(win, x, y, w, h, 9, p.raised_line);
    for (int i = 0; i < n; i++) {
        int sx = x + 3 + (w - 6) * i / n, ex = x + 3 + (w - 6) * (i + 1) / n;
        const char *t = word ? word(ctx, i) : "";
        if (i == chosen) {
            ud_round(win, sx, y + 3, ex - sx, h - 6, weave ? 6 : UG_R - 2,
                     weave ? p.seg_on : l->accent);
            if (weave)
                ud_fill(win, sx + 3, y + h - 5, ex - sx - 6, 2, l->accent);
        }
        uint32_t ink = i == chosen ? (weave ? l->text : l->on_accent)
                                   : (weave ? p.seg_ink : l->text);
        ud_text_mid_l(win, sx, y, ex - sx, h, t, ink,
                      i == chosen && weave ? UD_BOLD : UD_SANS, UD_PX);
    }
    if ((state & PU_FOCUS) && weave)
        pu_draw_focus(win, l, x, y, w, h, 9);
}

/* Progress, the thread being spun: under Weave a dashed stitch, the
   thread along it as far as value of max and a knot where it has got
   to; value -1, a length not known - a short piece of thread at 'phase'
   (of 1000) along the stitch, like a shuttle */
static inline void pu_draw_progress(int win, const struct ud_look *l, int x,
                                    int y, int w, int value, int max,
                                    int phase)
{
    struct pu_ink p;
    pu_ink_of(l, &p);
    int weave = pu_weave(l);
    if (!weave)
        ud_round(win, x, y + 2, w, 6, 3, l->row);
    else
        ud_stitch(win, x, y + 4, w, 0, p.stitch);
    if (value < 0) {
        int run = w * 22 / 100, at = x + (w - run) * (phase % 1000) / 1000;
        uint32_t c = weave ? l->dye[UD_WOAD] : l->accent;
        for (int k = 0; k < 8; k++)      /* brighter towards its head */
            ud_fill(win, at + run * k / 8, y + 3, run / 8 + 2, 4,
                    pu_mix(c, l->bg, 32 + k * 28));
        return;
    }
    if (max <= 0)
        max = 1;
    if (value > max)
        value = max;
    int fill = w * value / max;
    if (fill > 0)
        ud_round(win, x, y + (weave ? 3 : 2), fill, weave ? 4 : 6,
                 weave ? 2 : 3, l->accent);
    if (weave) {
        int kx = x + fill - 5;
        ud_disc(win, kx, y, 10, l->accent);
        ud_disc(win, kx + 2, y + 2, 6, 0xFFFFFF);
    }
}

/* A row of a list chosen, under Weave: the accent faint behind it and a
   knot at its start */
static inline void pu_draw_row_chosen(int win, const struct ud_look *l, int x,
                                      int y, int w, int h)
{
    ud_round(win, x, y, w, h, 6, pu_mix(l->accent, l->field, PU_SEL_A));
    ud_disc(win, x + 6, y + h / 2 - 3, 6, l->accent);
}

/* Under Weave, or any of v2's own things: drawn by ProtonUI v2. 1 when
   it was. */
static inline int pu_draw_one(struct ug *u, int i)
{
    struct ug_item *t = &u->it[i];
    const struct ud_look *l = &u->look;
    int win = u->win;
    int weave = pu_weave(l);
    int state = u->v2 && u->focus == i ? PU_FOCUS : 0;
    struct pu_ink p;
    pu_ink_of(l, &p);

    switch (t->kind) {
    case UG_BUTTON: {
        if (!weave && !u->v2)
            return 0;
        int kind = t->colour == UG_RED ? PU_DANGER : t->kind2;
        pu_draw_button(win, l, t->x, t->y, t->w, t->h, t->text, kind,
                       state | (t->enabled ? 0 : PU_OFF));
        return 1;
    }
    case UG_SLIDER: {
        if (!weave)
            return 0;
        /* the thread spun as far as the value, on its stitch, and the
           knot that is pulled along it */
        int span = t->w - 16;
        int fill = t->max > 0 ? t->value * span / t->max : 0;
        int cy = t->y + t->h / 2;
        uint32_t c = t->colour ? ug_rgb(l, t->colour) : l->accent;
        ud_stitch(win, t->x, cy - 1, t->w, 0, p.stitch);
        if (fill > 0)
            ud_round(win, t->x, cy - 2, fill + 8, 4, 2, c);
        ud_disc(win, t->x + fill, cy - 8, 16, c);
        ud_disc(win, t->x + fill + 3, cy - 5, 10, 0xFFFFFF);
        if (state)
            pu_draw_focus(win, l, t->x + fill, cy - 8, 16, 16, 8);
        return 1;
    }
    case UG_CHECK: {
        if (!weave)
            return 0;
        int by = t->y + (t->h - 16) / 2;
        pu_draw_check(win, l, t->x, by, t->value, state);
        ug_say(win, t->x + 16 + 10, t->y, t->h, t->text, l->text, UD_SANS);
        return 1;
    }
    case UG_LIST: {
        if (!weave)
            return 0;
        int rows = t->h / UG_ROW;
        ud_round(win, t->x, t->y, t->w, t->h, UG_R, l->field);
        for (int r = 0; r < rows && u->top + r < t->max; r++) {
            int index = u->top + r;
            const char *s = t->row ? t->row(t->ctx, index) : "";
            int y = t->y + r * UG_ROW;
            if (index == t->value) {
                pu_draw_row_chosen(win, l, t->x + 3, y + 1, t->w - 6,
                                   UG_ROW - 2);
                ug_say(win, t->x + 18, y, UG_ROW, s, l->text, UD_BOLD);
            } else {
                ug_say(win, t->x + 18, y, UG_ROW, s, l->text, UD_SANS);
            }
        }
        return 1;
    }
    case UG_TABS:
        if (!weave)
            return 0;
        pu_draw_segments(win, l, t->x, t->y, t->w, t->h, t->row, t->ctx,
                         t->max, t->value, state);
        return 1;
    case UG_SWITCH: {
        int sx = t->w > PU_SWITCH_W ? t->x + t->w - PU_SWITCH_W
                                    : t->x + ud_width_l(t->text, UD_SANS,
                                                        UD_PX) + 12;
        ug_say_l(win, t->x, t->y, t->h, t->text,
                 t->enabled ? l->text : l->dim, UD_SANS);
        pu_draw_switch(win, l, sx, t->y + (t->h - PU_SWITCH_H) / 2, t->value,
                       state);
        return 1;
    }
    case UG_SEGMENTS:
        pu_draw_segments(win, l, t->x, t->y, t->w, t->h, t->row, t->ctx,
                         t->max, t->value, state);
        return 1;
    case UG_PROGRESS:
        pu_draw_progress(win, l, t->x, t->y, t->w, t->value, t->max,
                         t->phase);
        return 1;
    case UG_KNOTS:
        for (int r = 0; r < t->max; r++) {
            int y = t->y + r * PU_KNOT_ROW;
            pu_draw_knot(win, l, t->x, y + (PU_KNOT_ROW - 16) / 2,
                         r == t->value, r == t->value ? state : 0);
            ug_say_l(win, t->x + 16 + 10, y, PU_KNOT_ROW,
                     t->row ? t->row(t->ctx, r) : "", l->text,
                     r == t->value ? UD_BOLD : UD_SANS);
        }
        return 1;
    default:
        return 0;
    }
}

static inline void ug_draw_one(struct ug *u, int i)
{
    struct ug_item *t = &u->it[i];
    const struct ud_look *l = &u->look;
    int win = u->win;

    if (pu_draw_one(u, i))
        return;
    switch (t->kind) {
    case UG_LABEL:
        ug_say(win, t->x, t->y, t->h, t->text,
               t->dim ? l->dim : (t->colour ? ug_rgb(l, t->colour) : l->text),
               t->colour == UG_BLUE ? UD_BOLD : UD_SANS);
        break;

    case UG_LINE:
        ud_fill(win, t->x, t->y, t->w, 1, l->line);
        break;

    case UG_BUTTON:
        if (t->enabled)
            ug_draw_button(win, l, t->x, t->y, t->w, t->h, t->text,
                           t->colour ? ug_rgb(l, t->colour) : l->accent,
                           l->on_accent);
        else
            ug_draw_button(win, l, t->x, t->y, t->w, t->h, t->text, l->row,
                           l->dim);
        break;

    case UG_SLIDER: {
        /* A thin track, filled as far as the value, and a knob there */
        int fill = t->max > 0 ? t->value * (t->w - t->h) / t->max : 0;
        int ty = t->y + t->h / 2 - 3;
        uint32_t c = t->colour ? ug_rgb(l, t->colour) : l->accent;
        ud_round(win, t->x, ty, t->w, 6, 3, l->row);
        if (fill > 0)
            ud_round(win, t->x, ty, fill + t->h / 2, 6, 3, c);
        ud_disc(win, t->x + fill, t->y, t->h, l->dark ? 0xEDEDF0 : 0xFFFFFF);
        ud_op(win, UD_OP_RING, t->x + fill, t->y, t->h, t->h, t->h / 2,
              l->line, 0, 0, 0, (const char *)0);
        break;
    }

    case UG_CHECK: {
        int b = 14, by = t->y + (t->h - b) / 2;
        if (t->value) {
            ud_round(win, t->x, by, b, b, 4,
                     t->colour ? ug_rgb(l, t->colour) : l->accent);
            ud_line(win, t->x + 3, by + 7, t->x + 6, by + 10, 28,
                    l->on_accent);
            ud_line(win, t->x + 6, by + 10, t->x + 11, by + 4, 28,
                    l->on_accent);
        } else {
            ud_round(win, t->x, by, b, b, 4, l->field);
            ud_ring(win, t->x, by, b, b, 4, l->line);
        }
        ug_say(win, t->x + b + 8, t->y, t->h, t->text, l->text, UD_SANS);
        break;
    }

    case UG_FIELD:
        ug_draw_field(win, l, t->x, t->y, t->w, t->h, t->buf, u->focus == i);
        break;

    case UG_LIST: {
        int rows = t->h / UG_ROW;
        ud_round(win, t->x, t->y, t->w, t->h, UG_R, l->field);
        for (int r = 0; r < rows && u->top + r < t->max; r++) {
            int index = u->top + r;
            const char *s = t->row ? t->row(t->ctx, index) : "";
            int y = t->y + r * UG_ROW;

            if (index == t->value) {
                ud_round(win, t->x + 3, y + 1, t->w - 6, UG_ROW - 2, 5,
                         l->accent);
                ug_say(win, t->x + 9, y, UG_ROW, s, l->on_accent, UD_SANS);
            } else {
                ug_say(win, t->x + 9, y, UG_ROW, s, l->text, UD_SANS);
            }
        }
        break;
    }

    case UG_TABS: {
        ud_fill(win, t->x, t->y, t->w, t->h, l->bg);
        if (t->max <= 0 || t->w <= 0 || t->h <= 0)
            break;
        for (int tab = 0; tab < t->max; tab++) {
            int left = t->x + t->w * tab / t->max;
            int right = t->x + t->w * (tab + 1) / t->max;
            int width = right - left;
            if (width <= 0)
                continue;
            const char *label = t->row ? t->row(t->ctx, tab) : "";
            if (tab == t->value) {
                ud_fill(win, left, t->y, width, t->h, l->field);
                ud_fill(win, left, t->y + t->h - 2, width, 2, l->accent);
            }
            int text_width = ud_width(label, UD_SANS, 10);
            int text_x = left + (width - text_width) / 2;
            if (text_x < left + 4)
                text_x = left + 4;
            ud_text(win, text_x, ud_baseline(t->y, t->h, 10), label,
                    tab == t->value ? l->text : l->dim, UD_SANS, 10);
        }
        break;
    }

    default:
        break;
    }
}

static inline void ug_draw_controls(struct ug *u)
{
    for (int i = 0; i < u->n; i++)
        ug_draw_one(u, i);
}

static inline void ug_draw_items(struct ug *u)
{
    ud_look(&u->look);
    ug_draw_controls(u);
}

static inline void ug_draw(struct ug *u)
{
    ud_look(&u->look);
    ud_fill(u->win, 0, 0, u->w, u->h, u->look.bg);
    ug_draw_controls(u);
}

/* --- what a click landed on ------------------------------------------ */

static inline int ug_hit(struct ug *u, int x, int y)
{
    /* Backwards, so that a thing drawn later - and therefore on top -
       takes the click. */
    for (int i = u->n - 1; i >= 0; i--) {
        struct ug_item *t = &u->it[i];
        int w = t->w ? t->w : ud_width(t->text, UD_SANS, UD_PX);

        if (t->kind == UG_LABEL || t->kind == UG_LINE ||
            t->kind == UG_PROGRESS)
            continue;
        if (t->kind == UG_CHECK)
            w = 22 + ud_width(t->text, UD_SANS, UD_PX);
        if (t->kind == UG_SWITCH && t->w <= PU_SWITCH_W)
            w = ud_width_l(t->text, UD_SANS, UD_PX) + 12 + PU_SWITCH_W;
        if (!t->enabled)
            continue;
        if (x >= t->x && x < t->x + w && y >= t->y && y < t->y + t->h)
            return i;
    }
    return -1;
}

/* One event in, one answer out. 'which' says which thing it was about. */
static inline int ug_event(struct ug *u, const struct win_event *ev,
                           int *which)
{
    int id = -1;
    int what = UG_NOTHING;

    if (which)
        *which = -1;

    switch (ev->type) {
    case WEV_CLOSE:
        return UG_CLOSED;

    case WEV_SIZE:
        u->w = ev->x;
        u->h = ev->y;
        return UG_RESIZED;

    case WEV_CLICK:
        id = ug_hit(u, ev->x, ev->y);
        if (id < 0) {
            u->focus = -1;
            return UG_NOTHING;
        }
        if (which)
            *which = id;
        switch (u->it[id].kind) {
        case UG_BUTTON:
            u->focus = -1;
            return UG_PRESSED;
        case UG_CHECK:
            u->it[id].value = !u->it[id].value;
            return UG_CHANGED;
        case UG_SLIDER: {
            struct ug_item *t = &u->it[id];
            /* Divided by one less than the width, so that the last
               pixel of the groove really is the maximum: a slider one
               cannot drag to the end feels broken. */
            int span = t->w - t->h;
            int v = span > 0
                    ? (ev->x - t->x - t->h / 2) * t->max / span : 0;
            if (v < 0)
                v = 0;
            if (v > t->max)
                v = t->max;
            t->value = v;
            return UG_CHANGED;
        }
        case UG_FIELD:
            u->focus = id;
            return UG_NOTHING;
        case UG_LIST: {
            struct ug_item *t = &u->it[id];
            int row = u->top + (ev->y - t->y) / UG_ROW;
            u->focus = id;
            if (row < 0 || row >= t->max)
                return UG_NOTHING;
            t->value = row;
            return UG_CHANGED;
        }
        case UG_SWITCH:
            u->it[id].value = !u->it[id].value;
            if (u->v2)
                u->focus = id;
            return UG_CHANGED;
        case UG_KNOTS: {
            struct ug_item *t = &u->it[id];
            int r = (ev->y - t->y) / PU_KNOT_ROW;
            if (u->v2)
                u->focus = id;
            if (r < 0 || r >= t->max || r == t->value)
                return UG_NOTHING;
            t->value = r;
            return UG_CHANGED;
        }
        case UG_SEGMENTS:
        case UG_TABS: {
            struct ug_item *t = &u->it[id];
            if (t->max <= 0 || t->w <= 0)
                return UG_NOTHING;
            u->focus = id;
            int tab = (ev->x - t->x) * t->max / t->w;
            if (tab < 0)
                tab = 0;
            if (tab >= t->max)
                tab = t->max - 1;
            if (tab == t->value)
                return UG_NOTHING;
            t->value = tab;
            return UG_CHANGED;
        }
        default:
            return UG_NOTHING;
        }

    case WEV_SCROLL:
        id = ug_hit(u, ev->x, ev->y);
        if (id < 0)
            return UG_NOTHING;
        if (which)
            *which = id;
        if (u->it[id].kind == UG_SLIDER) {
            int v = u->it[id].value + ev->val * (u->it[id].max / 20 + 1);
            if (v < 0)
                v = 0;
            if (v > u->it[id].max)
                v = u->it[id].max;
            u->it[id].value = v;
            return UG_CHANGED;
        }
        if (u->it[id].kind == UG_LIST) {
            int rows = u->it[id].h / UG_ROW;
            u->top -= ev->val;
            if (u->top > u->it[id].max - rows)
                u->top = u->it[id].max - rows;
            if (u->top < 0)
                u->top = 0;
            return UG_CHANGED;
        }
        return UG_NOTHING;

    case WEV_KEY: {
        char c = (char)ev->val;
        if (u->v2 && c == '\t') {
            /* the next thing that takes the keys, round to the first */
            for (int k = 1; k <= u->n; k++) {
                int j = (u->focus + k + u->n) % u->n;
                if (u->focus < 0)
                    j = k - 1;
                int kd = u->it[j].kind;
                if (u->it[j].enabled && kd != UG_LABEL && kd != UG_LINE &&
                    kd != UG_PROGRESS) {
                    u->focus = j;
                    if (which)
                        *which = j;
                    return UG_FOCUSED;
                }
            }
            return UG_NOTHING;
        }
        if (u->focus < 0)
            return UG_NOTHING;
        struct ug_item *t = &u->it[u->focus];

        if (which)
            *which = u->focus;

        if (u->v2) {
            int act = c == ' ' || c == '\n' || c == '\r';
            if (t->kind == UG_BUTTON && act)
                return UG_PRESSED;
            if ((t->kind == UG_CHECK || t->kind == UG_SWITCH) && act) {
                t->value = !t->value;
                return UG_CHANGED;
            }
            if (t->kind == UG_SEGMENTS || t->kind == UG_KNOTS) {
                int back = t->kind == UG_KNOTS ? KEY_UP : KEY_LEFT;
                int on = t->kind == UG_KNOTS ? KEY_DOWN : KEY_RIGHT;
                if (c == back && t->value > 0)
                    t->value--;
                else if (c == on && t->value + 1 < t->max)
                    t->value++;
                else
                    return UG_NOTHING;
                return UG_CHANGED;
            }
            if (t->kind == UG_SLIDER && (c == KEY_LEFT || c == KEY_RIGHT)) {
                int v = t->value + (c == KEY_RIGHT ? 1 : -1) *
                                   (t->max / 20 + 1);
                t->value = v < 0 ? 0 : v > t->max ? t->max : v;
                return UG_CHANGED;
            }
        }

        if (t->kind == UG_LIST) {
            int rows = t->h / UG_ROW;
            if (c == KEY_DOWN && t->value + 1 < t->max)
                t->value++;
            else if (c == KEY_UP && t->value > 0)
                t->value--;
            else
                return UG_NOTHING;
            if (t->value < u->top)
                u->top = t->value;
            if (t->value >= u->top + rows)
                u->top = t->value - rows + 1;
            return UG_CHANGED;
        }
        if (t->kind == UG_TABS) {
            int next = t->value;
            if (c == KEY_LEFT && next > 0)
                next--;
            else if (c == KEY_RIGHT && next + 1 < t->max)
                next++;
            else
                return UG_NOTHING;
            t->value = next;
            return UG_CHANGED;
        }
        if (t->kind != UG_FIELD || !t->buf)
            return UG_NOTHING;

        int len = ug_len(t->buf);
        if (c == '\n' || c == '\r')
            return UG_ENTER;
        if (c == '\b') {
            ug_rub(t->buf);
            return UG_TYPED;
        }
        if (ug_typed(c) && len < t->buflen - 1) {
            t->buf[len] = c;
            t->buf[len + 1] = '\0';
            return UG_TYPED;
        }
        return UG_NOTHING;
    }

    default:
        return UG_NOTHING;
    }

    return what;
}

/* --- a question that has to be answered before anything else --------
 *
 * "Are you sure?" is the one thing every program with a destructive
 * button needs, and until now each of them invented its own: gfiles
 * asked by making the second click mean something different from the
 * first, which works and is impossible to guess.
 *
 * It is drawn INSIDE the window rather than as a second one. A program
 * whose question has its own window has to keep both alive, move them
 * together and answer for both when the desktop asks it to close; a
 * panel over the content is all of that for free, and it also cannot
 * be lost behind anything.
 */
struct ug_modal {
    int on;                      /* is it up */
    const char *title;
    const char *note;            /* a second line, or "" */
    char *buf;                   /* something to type, or NULL */
    int buflen;
    const char *yes, *no;
    int answer;                  /* 1 = yes, 0 = no, -1 = still asking */
    int list;                    /* a list widget to show inside, or -1 */
};

#define UG_MODAL_H 116
#define UG_BTN_W   96            /* a dialogue's two buttons */
#define UG_BTN_H   24

static inline void ug_ask(struct ug_modal *m, const char *title,
                          const char *note, const char *yes, const char *no)
{
    m->on = 1;
    m->title = title;
    m->note = note ? note : "";
    m->buf = (char *)0;
    m->buflen = 0;
    m->yes = yes;
    m->no = no;
    m->answer = -1;
    m->list = -1;
}

static inline void ug_ask_text(struct ug_modal *m, const char *title,
                               char *buf, int buflen, const char *yes,
                               const char *no)
{
    ug_ask(m, title, "", yes, no);
    m->buf = buf;
    m->buflen = buflen;
}

/* Where the panel sits: a band across the middle of the window. */
static inline int ug_modal_y(struct ug *u)
{
    int y = (u->h - UG_MODAL_H) / 2;
    return y < 0 ? 0 : y;
}

/* The two answers at the bottom edge of a panel: their left edges and
   their top - one place, for the drawing and the clicking alike */
static inline int ug_yes_x(void) { return 10 + 12; }
static inline int ug_no_x(void)  { return 10 + 12 + UG_BTN_W + 10; }

/* A panel over the window: what is behind it dimmed, so that it is the
   one thing to answer */
static inline void ug_panel(struct ug *u, int y, int h)
{
    int x = 10, w = u->w - 20;
    ud_look(&u->look);
    ud_fill_a(u->win, 0, 0, u->w, u->h, 0x000000, u->look.dark ? 110 : 70);
    ud_round(u->win, x, y, w, h, 10, u->look.field);
    ud_ring(u->win, x, y, w, h, 10, u->look.line);
}

static inline void ug_answers(struct ug *u, int by, const char *yes,
                              const char *no)
{
    const struct ud_look *l = &u->look;
    ug_draw_button_l(u->win, l, ug_yes_x(), by, UG_BTN_W, UG_BTN_H, yes,
                     l->accent, l->on_accent);
    ug_draw_button_l(u->win, l, ug_no_x(), by, UG_BTN_W, UG_BTN_H, no, l->row,
                     l->text);
}

static inline void ug_modal_draw(struct ug *u, struct ug_modal *m)
{
    if (!m->on)
        return;

    int x = 10, w = u->w - 20;
    int y = ug_modal_y(u);
    int by = y + UG_MODAL_H - UG_BTN_H - 12;

    ug_panel(u, y, UG_MODAL_H);
    const struct ud_look *l = &u->look;
    ud_text(u->win, x + 12, y + 22, m->title, l->text, UD_BOLD, UD_PX);
    if (m->note[0])
        ud_text_l(u->win, x + 12, y + 40, m->note, l->dim, UD_SANS, UD_PX);
    if (m->buf)
        ug_draw_field(u->win, l, x + 12, y + 46, w - 24, 22, m->buf, 1);
    ug_answers(u, by, m->yes, m->no);
}

/* The panel takes every event while it is up: that is what makes it a
   question rather than a suggestion. Returns UG_PRESSED when it has
   been answered (m->answer says how), UG_TYPED while typing. */
static inline int ug_modal_event(struct ug *u, struct ug_modal *m,
                                 const struct win_event *ev)
{
    int y = ug_modal_y(u);
    int by = y + UG_MODAL_H - UG_BTN_H - 12;

    if (!m->on)
        return UG_NOTHING;

    if (ev->type == WEV_CLOSE) {
        m->on = 0;
        m->answer = 0;
        return UG_CLOSED;
    }
    if (ev->type == WEV_CLICK) {
        if (ev->y >= by && ev->y < by + UG_BTN_H) {
            if (ev->x >= ug_yes_x() && ev->x < ug_yes_x() + UG_BTN_W) {
                m->on = 0;
                m->answer = 1;
                return UG_PRESSED;
            }
            if (ev->x >= ug_no_x() && ev->x < ug_no_x() + UG_BTN_W) {
                m->on = 0;
                m->answer = 0;
                return UG_PRESSED;
            }
        }
        return UG_NOTHING;
    }
    if (ev->type == WEV_KEY) {
        char c = (char)ev->val;

        if (c == 27) {                    /* Escape says no */
            m->on = 0;
            m->answer = 0;
            return UG_PRESSED;
        }
        if (c == '\n' || c == '\r') {
            m->on = 0;
            m->answer = 1;
            return UG_PRESSED;
        }
        if (!m->buf)
            return UG_NOTHING;

        int len = ug_len(m->buf);
        if (c == '\b') {
            ug_rub(m->buf);
            return UG_TYPED;
        }
        if (ug_typed(c) && len < m->buflen - 1) {
            m->buf[len] = c;
            m->buf[len + 1] = '\0';
            return UG_TYPED;
        }
        return UG_NOTHING;
    }
    return UG_NOTHING;
}

/* --- choosing a file, instead of typing its name blind ---------------
 *
 * The editor asked "Open:" and waited for a name. That works if one
 * remembers what is on the disk, and is otherwise a guessing game -
 * the one part of a graphical system where a person is worse off than
 * in a shell, because a shell at least completes names.
 *
 * So: a list of what is there, a line to type in for the name that is
 * not there yet, and two buttons. Typing narrows the list; an unmatched
 * name can still be opened directly. Clicking a row fills the line, so
 * whichever way one arrives at a name, the same line is opened.
 */
#define UG_PICK_ROWS 7
#define UG_PICK_H   (30 + UG_PICK_ROWS * UG_ROW + 8 + 22 + 10 + UG_BTN_H + 12)

struct ug_pick {
    int on;
    const char *title;
    const char *(*row)(void *ctx, int i);
    void *ctx;
    int rows;                    /* how many there are */
    int chosen;                  /* which one, -1 = none */
    int top;                     /* the first one shown */
    char *buf;                   /* the name, typed or clicked */
    int buflen;
    char query[64];
    const char *yes, *no;
    int answer;                  /* 1 = open it, 0 = never mind, -1 = asking */
};

static inline void ug_pick_open(struct ug_pick *p, const char *title,
                                const char *(*row)(void *ctx, int i),
                                void *ctx, int rows, char *buf, int buflen)
{
    p->on = 1;
    p->title = title;
    p->row = row;
    p->ctx = ctx;
    p->rows = rows;
    p->chosen = -1;
    p->top = 0;
    p->buf = buf;
    p->buflen = buflen;
    p->query[0] = '\0';
    p->yes = "Open";
    p->no = "Cancel";
    p->answer = -1;
    if (buf && buflen > 0)
        buf[0] = '\0';
}

static inline int ug_pick_matches(const struct ug_pick *p, int index)
{
    if (!p->query[0])
        return 1;
    const char *s = p->row ? p->row(p->ctx, index) : "";
    for (int i = 0; s && s[i]; i++) {
        int j = 0;
        while (p->query[j] && s[i + j]) {
            unsigned char a = (unsigned char)p->query[j];
            unsigned char b = (unsigned char)s[i + j];
            if (a >= 'A' && a <= 'Z')
                a = (unsigned char)(a + ('a' - 'A'));
            if (b >= 'A' && b <= 'Z')
                b = (unsigned char)(b + ('a' - 'A'));
            if (a != b)
                break;
            j++;
        }
        if (!p->query[j])
            return 1;
    }
    return 0;
}

static inline int ug_pick_visible_count(const struct ug_pick *p)
{
    int count = 0;
    for (int i = 0; i < p->rows; i++)
        if (ug_pick_matches(p, i))
            count++;
    return count;
}

static inline int ug_pick_visible_at(const struct ug_pick *p, int visible)
{
    if (visible < 0)
        return -1;
    for (int i = 0; i < p->rows; i++) {
        if (!ug_pick_matches(p, i))
            continue;
        if (!visible--)
            return i;
    }
    return -1;
}

static inline int ug_pick_visible_pos(const struct ug_pick *p, int index)
{
    int visible = 0;
    for (int i = 0; i < p->rows; i++) {
        if (!ug_pick_matches(p, i))
            continue;
        if (i == index)
            return visible;
        visible++;
    }
    return -1;
}

static inline void ug_pick_show(struct ug_pick *p, int visible)
{
    if (visible < p->top)
        p->top = visible;
    else if (visible >= p->top + UG_PICK_ROWS)
        p->top = visible - UG_PICK_ROWS + 1;
}

static inline int ug_pick_y(struct ug *u)
{
    int y = (u->h - UG_PICK_H) / 2;
    return y < 2 ? 2 : y;
}

/* The list's top, and the line under it */
static inline int ug_pick_list_y(struct ug *u) { return ug_pick_y(u) + 30; }
static inline int ug_pick_field_y(struct ug *u)
{
    return ug_pick_list_y(u) + UG_PICK_ROWS * UG_ROW + 8;
}

static inline void ug_pick_draw(struct ug *u, struct ug_pick *p)
{
    if (!p->on)
        return;

    int x = 10, w = u->w - 20;
    int y = ug_pick_y(u);
    int listy = ug_pick_list_y(u);
    int by = y + UG_PICK_H - UG_BTN_H - 12;
    int visible = ug_pick_visible_count(p);
    int max_top = visible - UG_PICK_ROWS;
    if (max_top < 0)
        max_top = 0;
    if (p->top > max_top)
        p->top = max_top;

    ug_panel(u, y, UG_PICK_H);
    const struct ud_look *l = &u->look;
    ud_text(u->win, x + 12, y + 20, p->title, l->text, UD_BOLD, UD_PX);
    ud_round(u->win, x + 8, listy, w - 16, UG_PICK_ROWS * UG_ROW, UG_R, l->bg);

    for (int r = 0; r < UG_PICK_ROWS; r++) {
        int index = ug_pick_visible_at(p, p->top + r);
        if (index < 0)
            break;
        const char *s = p->row ? p->row(p->ctx, index) : "";
        int ry = listy + r * UG_ROW;

        if (index == p->chosen) {
            ud_round(u->win, x + 10, ry + 1, w - 20, UG_ROW - 2, 5, l->accent);
            ug_say(u->win, x + 16, ry, UG_ROW, s, l->on_accent, UD_SANS);
        } else {
            ug_say(u->win, x + 16, ry, UG_ROW, s, l->text, UD_SANS);
        }
    }

    if (p->buf)
        ug_draw_field(u->win, l, x + 8, ug_pick_field_y(u), w - 16, 22,
                      p->buf, 1);
    ug_answers(u, by, p->yes, p->no);
}

static inline void ug_pick_take(struct ug_pick *p, int index)
{
    const char *s;
    int i = 0;

    if (index < 0 || index >= p->rows || !p->buf)
        return;
    p->chosen = index;
    s = p->row ? p->row(p->ctx, index) : "";
    while (s[i] && i < p->buflen - 1) {
        p->buf[i] = s[i];
        i++;
    }
    p->buf[i] = '\0';
}

static inline int ug_pick_event(struct ug *u, struct ug_pick *p,
                                const struct win_event *ev)
{
    int x = 10, w = u->w - 20;
    int y = ug_pick_y(u);
    int listy = ug_pick_list_y(u);
    int by = y + UG_PICK_H - UG_BTN_H - 12;

    if (!p->on)
        return UG_NOTHING;

    if (ev->type == WEV_CLOSE) {
        p->on = 0;
        p->answer = 0;
        return UG_CLOSED;
    }
    if (ev->type == WEV_SCROLL) {
        int visible = ug_pick_visible_count(p);
        p->top -= ev->val;
        if (p->top > visible - UG_PICK_ROWS)
            p->top = visible - UG_PICK_ROWS;
        if (p->top < 0)
            p->top = 0;
        return UG_CHANGED;
    }
    if (ev->type == WEV_CLICK) {
        if (ev->y >= by && ev->y < by + UG_BTN_H) {
            if (ev->x >= ug_yes_x() && ev->x < ug_yes_x() + UG_BTN_W) {
                p->on = 0;
                p->answer = 1;
                return UG_PRESSED;
            }
            if (ev->x >= ug_no_x() && ev->x < ug_no_x() + UG_BTN_W) {
                p->on = 0;
                p->answer = 0;
                return UG_PRESSED;
            }
            return UG_NOTHING;
        }
        if (ev->y >= listy && ev->y < listy + UG_PICK_ROWS * UG_ROW &&
            ev->x >= x && ev->x < x + w) {
            int index = ug_pick_visible_at(
                p, p->top + (ev->y - listy) / UG_ROW);
            if (index >= 0)
                ug_pick_take(p, index);
            return UG_CHANGED;
        }
        return UG_NOTHING;
    }
    if (ev->type == WEV_KEY) {
        char c = (char)ev->val;

        if (c == 27) {
            p->on = 0;
            p->answer = 0;
            return UG_PRESSED;
        }
        if (c == '\n' || c == '\r') {
            if (p->chosen < 0 && p->query[0]) {
                int index = ug_pick_visible_at(p, 0);
                if (index >= 0)
                    ug_pick_take(p, index);
            }
            p->on = 0;
            p->answer = 1;
            return UG_PRESSED;
        }
        if (c == KEY_DOWN || c == KEY_UP) {
            int count = ug_pick_visible_count(p);
            int pos = ug_pick_visible_pos(p, p->chosen);
            if (!count)
                return UG_NOTHING;
            if (c == KEY_DOWN)
                pos = pos < 0 || pos + 1 == count ? 0 : pos + 1;
            else
                pos = pos <= 0 ? 0 : pos - 1;
            int index = ug_pick_visible_at(p, pos);
            if (index >= 0) {
                ug_pick_take(p, index);
                ug_pick_show(p, pos);
            }
            return UG_CHANGED;
        }
        if (!p->buf)
            return UG_NOTHING;

        int len = ug_len(p->buf);
        if (p->chosen >= 0) {
            len = 0;
            while (p->buf[len] && len < (int)sizeof(p->query) - 1) {
                p->query[len] = p->buf[len];
                len++;
            }
            p->query[len] = '\0';
            p->chosen = -1;
        }
        if (c == '\b') {
            ug_rub(p->buf);
            ug_rub(p->query);
            p->chosen = -1;
            p->top = 0;
            return UG_TYPED;
        }
        if (ug_typed(c) && len < p->buflen - 1 &&
            len < (int)sizeof(p->query) - 1) {
            p->buf[len] = c;
            p->buf[len + 1] = '\0';
            p->query[len] = c;
            p->query[len + 1] = '\0';
            p->chosen = -1;
            p->top = 0;
            return UG_TYPED;
        }
        return UG_NOTHING;
    }
    return UG_NOTHING;
}

#endif
