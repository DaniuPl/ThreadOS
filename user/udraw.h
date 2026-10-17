/* Drawing the way the desktop draws: soft edges, TrueType, the theme
 *
 * A program's window was a grid of eight-by-eight letters in a palette
 * of a few dozen colours, and next to the desktop around it - glass,
 * round corners, shadows - it looked like a machine from a long time
 * ago. These calls hand a program the same tools the desktop has: a
 * rounded rectangle, a circle, a line with soft edges, and text in the
 * system's TrueType faces, all in real colours.
 *
 * And the colours themselves come from the desktop: ud_look() asks
 * which theme is on and what its background, text, accent and the
 * rest are. A program that asks every time it draws follows the theme
 * the moment it changes, as the desktop does.
 *
 * Positions are in whole pixels of the window's content. Text is
 * placed by its BASELINE; ud_baseline() says where that goes for text
 * centred in a band, which is nearly always what is wanted.
 */

#ifndef THREADOS_UDRAW_H
#define THREADOS_UDRAW_H

#include "uapi.h"

/* The faces */
#define UD_SANS 0
#define UD_BOLD 1
#define UD_MONO 2

#define UD_PX   12            /* the size programs write in */

/* What kernel/include/desktop.h calls the operations */
#define UD_OP_FILL    1
#define UD_OP_RRECT   2
#define UD_OP_RING    3
#define UD_OP_GRAD    4
#define UD_OP_DISC    5
#define UD_OP_LINE    6
#define UD_OP_TEXT    7
#define UD_OP_MEASURE 8

/* The theme's colours, as the desktop hands them out (in this order) */
struct ud_look {
    uint32_t dark;            /* 1 = the dark theme */
    uint32_t bg;              /* the window's background */
    uint32_t field;           /* a surface to fill in: a field, a list */
    uint32_t text, dim;       /* text, and quieter text */
    uint32_t line;            /* between one thing and the next */
    uint32_t row;             /* a stripe, a row under the pointer */
    uint32_t accent, on_accent;
    uint32_t danger, good;
};

static inline void ud_look(struct ud_look *l)
{
    sys_ui_look((uint32_t *)l);
}

static inline int ud_op(int win, int op, int x, int y, int w, int h, int r,
                        uint32_t rgb, uint32_t rgb2, int font, int px,
                        const char *s)
{
    struct win_draw_cmd c = { op, x, y, w, h, r, rgb, rgb2, 255, font, px, s };
    return sys_win_draw(win, &c);
}

static inline void ud_fill(int win, int x, int y, int w, int h, uint32_t rgb)
{
    ud_op(win, UD_OP_FILL, x, y, w, h, 0, rgb, 0, 0, 0, (const char *)0);
}

/* The same, letting 'a' (of 255) of the colour through */
static inline void ud_fill_a(int win, int x, int y, int w, int h, uint32_t rgb,
                             int a)
{
    struct win_draw_cmd c = { UD_OP_FILL, x, y, w, h, 0, rgb, 0, a, 0, 0,
                              (const char *)0 };
    sys_win_draw(win, &c);
}

static inline void ud_round(int win, int x, int y, int w, int h, int r,
                            uint32_t rgb)
{
    ud_op(win, UD_OP_RRECT, x, y, w, h, r, rgb, 0, 0, 0, (const char *)0);
}

static inline void ud_ring(int win, int x, int y, int w, int h, int r,
                           uint32_t rgb)
{
    ud_op(win, UD_OP_RING, x, y, w, h, r, rgb, 0, 0, 0, (const char *)0);
}

static inline void ud_grad(int win, int x, int y, int w, int h, int r,
                           uint32_t top, uint32_t bottom)
{
    ud_op(win, UD_OP_GRAD, x, y, w, h, r, top, bottom, 0, 0, (const char *)0);
}

static inline void ud_disc(int win, int x, int y, int d, uint32_t rgb)
{
    ud_op(win, UD_OP_DISC, x, y, d, d, 0, rgb, 0, 0, 0, (const char *)0);
}

/* From the middle of one pixel to the middle of another, 'thick'
   sixteenths of a pixel wide */
static inline void ud_line(int win, int x0, int y0, int x1, int y1,
                           int thick, uint32_t rgb)
{
    ud_op(win, UD_OP_LINE, x0, y0, x1, y1, thick, rgb, 0, 0, 0,
          (const char *)0);
}

/* Text with its baseline at y; returns where the pen ended */
static inline int ud_text(int win, int x, int y, const char *s, uint32_t rgb,
                          int font, int px)
{
    return ud_op(win, UD_OP_TEXT, x, y, 0, 0, 0, rgb, 0, font, px, s);
}

static inline int ud_width(const char *s, int font, int px)
{
    return ud_op(-1, UD_OP_MEASURE, 0, 0, 0, 0, 0, 0, 0, font, px, s);
}

/* The baseline that centres capital letters in a band 'h' tall - the
   arithmetic the desktop uses for its own titles */
static inline int ud_baseline(int top, int h, int px)
{
    int cap = (px * 73 + 50) / 100;
    return top + (h + cap) / 2;
}

/* Text centred in a box */
static inline void ud_text_mid(int win, int x, int y, int w, int h,
                               const char *s, uint32_t rgb, int font, int px)
{
    int tw = ud_width(s, font, px);
    ud_text(win, x + (w - tw) / 2, ud_baseline(y, h, px), s, rgb, font, px);
}

/* Text ending at x - numbers in a column line up on their last digit */
static inline void ud_text_right(int win, int x, int y, const char *s,
                                 uint32_t rgb, int font, int px)
{
    ud_text(win, x - ud_width(s, font, px), y, s, rgb, font, px);
}

#endif
