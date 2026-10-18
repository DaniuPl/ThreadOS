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
    struct ud_look look;         /* the theme's colours, as of the last draw */
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

/* A field to type in: a white (or dark) surface, a line round it in
   the accent when it has the keys, and where the next letter goes */
static inline void ug_draw_field(int win, const struct ud_look *l, int x,
                                 int y, int w, int h, const char *text,
                                 int focused)
{
    ud_round(win, x, y, w, h, UG_R, l->field);
    ud_ring(win, x, y, w, h, UG_R, focused ? l->accent : l->line);
    int end = ug_say(win, x + 6, y, h, text ? text : "", l->text, UD_SANS);
    if (focused && end < x + w - 4)
        ud_fill(win, end + 1, y + 4, 1, h - 8, l->text);
}

/* A button: a rounded surface with its word in the middle */
static inline void ug_draw_button(int win, const struct ud_look *l, int x,
                                  int y, int w, int h, const char *text,
                                  uint32_t face, uint32_t ink)
{
    (void)l;
    ud_round(win, x, y, w, h, UG_R, face);
    ud_text_mid(win, x, y, w, h, text, ink, UD_SANS, UD_PX);
}

static inline void ug_draw_one(struct ug *u, int i)
{
    struct ug_item *t = &u->it[i];
    const struct ud_look *l = &u->look;
    int win = u->win;

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

    default:
        break;
    }
}

static inline void ug_draw(struct ug *u)
{
    ud_look(&u->look);
    ud_fill(u->win, 0, 0, u->w, u->h, u->look.bg);
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
        int w = t->w ? t->w : ud_width(t->text, UD_SANS, UD_PX);

        if (t->kind == UG_LABEL || t->kind == UG_LINE)
            continue;
        if (t->kind == UG_CHECK)
            w = 22 + ud_width(t->text, UD_SANS, UD_PX);
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
    ug_draw_button(u->win, l, ug_yes_x(), by, UG_BTN_W, UG_BTN_H, yes,
                   l->accent, l->on_accent);
    ug_draw_button(u->win, l, ug_no_x(), by, UG_BTN_W, UG_BTN_H, no, l->row,
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
        ud_text(u->win, x + 12, y + 40, m->note, l->dim, UD_SANS, UD_PX);
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
 * not there yet, and two buttons. Clicking a row fills the line, so
 * whichever way one arrives at a name, the same line is opened - and
 * typing goes on working exactly as it did, which is why the old way
 * of using it still works.
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
    p->yes = "Open";
    p->no = "Cancel";
    p->answer = -1;
    if (buf && buflen > 0)
        buf[0] = '\0';
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

    ug_panel(u, y, UG_PICK_H);
    const struct ud_look *l = &u->look;
    ud_text(u->win, x + 12, y + 20, p->title, l->text, UD_BOLD, UD_PX);
    ud_round(u->win, x + 8, listy, w - 16, UG_PICK_ROWS * UG_ROW, UG_R, l->bg);

    for (int r = 0; r < UG_PICK_ROWS && p->top + r < p->rows; r++) {
        int index = p->top + r;
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
        p->top -= ev->val;
        if (p->top > p->rows - UG_PICK_ROWS)
            p->top = p->rows - UG_PICK_ROWS;
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
            ug_pick_take(p, p->top + (ev->y - listy) / UG_ROW);
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
            p->on = 0;
            p->answer = 1;
            return UG_PRESSED;
        }
        if (c == KEY_DOWN) {
            ug_pick_take(p, p->chosen + 1 < p->rows ? p->chosen + 1 : 0);
            if (p->chosen >= p->top + UG_PICK_ROWS)
                p->top = p->chosen - UG_PICK_ROWS + 1;
            if (p->chosen < p->top)
                p->top = p->chosen;
            return UG_CHANGED;
        }
        if (c == KEY_UP) {
            ug_pick_take(p, p->chosen > 0 ? p->chosen - 1 : 0);
            if (p->chosen < p->top)
                p->top = p->chosen;
            return UG_CHANGED;
        }
        if (!p->buf)
            return UG_NOTHING;

        int len = ug_len(p->buf);
        if (c == '\b') {
            ug_rub(p->buf);
            p->chosen = -1;
            return UG_TYPED;
        }
        if (ug_typed(c) && len < p->buflen - 1) {
            p->buf[len] = c;
            p->buf[len + 1] = '\0';
            p->chosen = -1;
            return UG_TYPED;
        }
        return UG_NOTHING;
    }
    return UG_NOTHING;
}

#endif
