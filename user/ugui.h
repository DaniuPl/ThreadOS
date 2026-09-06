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
 * What it deliberately is not: it does not own the loop, it does not
 * keep a program's data, and it does not lay anything out by itself.
 * Positions are given by the program, in pixels, because that is what
 * every window here already knows and because a layout engine would
 * be a second framework on top of the first.
 *
 * The colours follow the rule the whole system follows: dark text on
 * a light surface, light text on a dark one.
 */

#ifndef PROTONOS_UGUI_H
#define PROTONOS_UGUI_H

#include "uapi.h"

/* The palette, by the names the programs already use */
#define UG_BLACK 0
#define UG_RED   1
#define UG_BLUE  3
#define UG_WHITE 4
#define UG_BG    8
#define UG_DIM   15

#define UG_MAX   24              /* things in one window */
#define UG_ROW   12              /* one row of text */

/* What a thing is */
#define UG_LABEL  0
#define UG_BUTTON 1
#define UG_SLIDER 2
#define UG_CHECK  3
#define UG_FIELD  4
#define UG_LIST   5
#define UG_LINE   6

/* What happened */
#define UG_NOTHING 0
#define UG_PRESSED 1             /* a button */
#define UG_CHANGED 2             /* a slider, a box, a row of a list */
#define UG_TYPED   3             /* a field's text changed */
#define UG_ENTER   4             /* Enter in a field */
#define UG_CLOSED  5             /* the window is to go */
#define UG_RESIZED 6

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
};

struct ug {
    int win;
    int w, h;
    struct ug_item it[UG_MAX];
    int n;
    int focus;                   /* which thing has the keyboard, -1 = none */
    int top;                     /* the first visible row of a list */
};

/* --- putting a window together -------------------------------------- */

static inline void ug_start(struct ug *u, int win, int w, int h)
{
    u->win = win;
    u->w = w;
    u->h = h;
    u->n = 0;
    u->focus = -1;
    u->top = 0;
}

static inline int ug_add(struct ug *u, int kind, int x, int y, int w, int h)
{
    if (u->n >= UG_MAX)
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

static inline int ug_line(struct ug *u, int x, int y, int w)
{
    return ug_add(u, UG_LINE, x, y, w, 1);
}

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

static inline void ug_draw_one(struct ug *u, int i)
{
    struct ug_item *t = &u->it[i];
    int win = u->win;

    switch (t->kind) {
    case UG_LABEL:
        win_text(win, t->x, t->y, t->text,
                 t->dim ? UG_DIM : (t->colour ? t->colour : UG_BLACK));
        break;

    case UG_LINE:
        win_rect(win, t->x, t->y, t->w, 1, UG_DIM);
        break;

    case UG_BUTTON: {
        int face = t->colour ? t->colour : UG_BLUE;
        /* A button is a dark surface, so its caption is light - the
           other way round is the one thing this system does not do. */
        win_rect(win, t->x, t->y, t->w, t->h, t->enabled ? face : UG_DIM);
        int len = ug_len(t->text) * 8;
        int at = t->x + (t->w - len) / 2;
        if (at < t->x + 2)
            at = t->x + 2;
        win_text(win, at, t->y + (t->h - 8) / 2, t->text, UG_WHITE);
        break;
    }

    case UG_SLIDER: {
        int fill = t->max > 0 ? t->value * t->w / t->max : 0;
        win_rect(win, t->x, t->y, t->w, t->h, UG_BLACK);
        if (fill > 0)
            win_rect(win, t->x, t->y, fill, t->h,
                     t->colour ? t->colour : UG_BLUE);
        /* A line where the filling ends, so that the edge can be seen
           even when the slider is at either end of its travel. */
        if (fill > 0 && fill < t->w)
            win_rect(win, t->x + fill - 1, t->y, 1, t->h, UG_WHITE);
        break;
    }

    case UG_CHECK: {
        win_rect(win, t->x, t->y, 10, 10, UG_BLACK);
        if (t->value)
            win_rect(win, t->x + 2, t->y + 2, 6, 6,
                     t->colour ? t->colour : UG_BLUE);
        win_text(win, t->x + 16, t->y + 1, t->text, UG_BLACK);
        break;
    }

    case UG_FIELD: {
        int focused = (u->focus == i);
        win_rect(win, t->x, t->y, t->w, t->h, UG_WHITE);
        win_rect(win, t->x, t->y, t->w, 1, focused ? UG_BLUE : UG_DIM);
        win_rect(win, t->x, t->y + t->h - 1, t->w, 1,
                 focused ? UG_BLUE : UG_DIM);
        if (t->buf)
            win_text(win, t->x + 3, t->y + 3, t->buf, UG_BLACK);
        if (focused && t->buf) {
            int at = t->x + 3 + ug_len(t->buf) * 8;
            if (at < t->x + t->w - 2)
                win_rect(win, at, t->y + 2, 1, t->h - 4, UG_BLACK);
        }
        break;
    }

    case UG_LIST: {
        int rows = t->h / UG_ROW;
        win_rect(win, t->x, t->y, t->w, t->h, UG_WHITE);
        for (int r = 0; r < rows && u->top + r < t->max; r++) {
            int index = u->top + r;
            const char *s = t->row ? t->row(t->ctx, index) : "";
            int y = t->y + r * UG_ROW;

            if (index == t->value) {
                win_rect(win, t->x, y, t->w, UG_ROW, UG_BLUE);
                win_text(win, t->x + 3, y + 2, s, UG_WHITE);
            } else {
                win_text(win, t->x + 3, y + 2, s, UG_BLACK);
            }
        }
        break;
    }

    default:
        break;
    }
}

static inline void ug_draw(struct ug *u)
{
    win_rect(u->win, 0, 0, u->w, u->h, UG_BG);
    for (int i = 0; i < u->n; i++)
        ug_draw_one(u, i);
}

/* --- what a click landed on ------------------------------------------ */

static inline int ug_hit(struct ug *u, int x, int y)
{
    /* Backwards, so that a thing drawn later - and therefore on top -
       takes the click. */
    for (int i = u->n - 1; i >= 0; i--) {
        struct ug_item *t = &u->it[i];
        int w = t->w ? t->w : ug_len(t->text) * 8;

        if (t->kind == UG_LABEL || t->kind == UG_LINE)
            continue;
        if (t->kind == UG_CHECK)
            w = 16 + ug_len(t->text) * 8;
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
            int v = t->w > 1
                    ? (ev->x - t->x) * t->max / (t->w - 1) : 0;
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
        if (u->focus < 0)
            return UG_NOTHING;
        struct ug_item *t = &u->it[u->focus];
        char c = (char)ev->val;

        if (which)
            *which = u->focus;

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
        if (t->kind != UG_FIELD || !t->buf)
            return UG_NOTHING;

        int len = ug_len(t->buf);
        if (c == '\n' || c == '\r')
            return UG_ENTER;
        if (c == '\b') {
            if (len > 0)
                t->buf[len - 1] = '\0';
            return UG_TYPED;
        }
        if (c >= 0x20 && c < 0x7F && len < t->buflen - 1) {
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

#define UG_MODAL_H 96

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

static inline void ug_modal_draw(struct ug *u, struct ug_modal *m)
{
    if (!m->on)
        return;

    int x = 8, w = u->w - 16;
    int y = ug_modal_y(u);
    int by = y + UG_MODAL_H - 26;
    int bw = 84;

    win_rect(u->win, x, y, w, UG_MODAL_H, UG_WHITE);
    win_rect(u->win, x, y, w, 2, UG_BLUE);
    win_rect(u->win, x, y + UG_MODAL_H - 2, w, 2, UG_BLUE);
    win_text(u->win, x + 8, y + 10, m->title, UG_BLACK);
    if (m->note[0])
        win_text(u->win, x + 8, y + 10 + UG_ROW, m->note, UG_DIM);

    if (m->buf) {
        win_rect(u->win, x + 8, y + 40, w - 16, UG_ROW + 4, UG_BG);
        win_text(u->win, x + 11, y + 43, m->buf, UG_BLACK);
        int at = x + 11 + ug_len(m->buf) * 8;
        if (at < x + w - 10)
            win_rect(u->win, at, y + 42, 1, UG_ROW, UG_BLACK);
    }

    win_rect(u->win, x + 8, by, bw, 16, UG_BLUE);
    win_text(u->win, x + 8 + 8, by + 4, m->yes, UG_WHITE);
    win_rect(u->win, x + 8 + bw + 10, by, bw, 16, UG_DIM);
    win_text(u->win, x + 8 + bw + 18, by + 4, m->no, UG_WHITE);
}

/* The panel takes every event while it is up: that is what makes it a
   question rather than a suggestion. Returns UG_PRESSED when it has
   been answered (m->answer says how), UG_TYPED while typing. */
static inline int ug_modal_event(struct ug *u, struct ug_modal *m,
                                 const struct win_event *ev)
{
    int x = 8, w = u->w - 16;
    int y = ug_modal_y(u);
    int by = y + UG_MODAL_H - 26;
    int bw = 84;

    if (!m->on)
        return UG_NOTHING;

    if (ev->type == WEV_CLOSE) {
        m->on = 0;
        m->answer = 0;
        return UG_CLOSED;
    }
    if (ev->type == WEV_CLICK) {
        if (ev->y >= by && ev->y < by + 16) {
            if (ev->x >= x + 8 && ev->x < x + 8 + bw) {
                m->on = 0;
                m->answer = 1;
                return UG_PRESSED;
            }
            if (ev->x >= x + 8 + bw + 10 && ev->x < x + 8 + 2 * bw + 10) {
                m->on = 0;
                m->answer = 0;
                return UG_PRESSED;
            }
        }
        (void)w;
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
            if (len > 0)
                m->buf[len - 1] = '\0';
            return UG_TYPED;
        }
        if (c >= 0x20 && c < 0x7F && len < m->buflen - 1) {
            m->buf[len] = c;
            m->buf[len + 1] = '\0';
            return UG_TYPED;
        }
        return UG_NOTHING;
    }
    return UG_NOTHING;
}

#endif
