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
/* the serif, in every style: Notes writes a note in it */
#define UD_SERIF 3
#define UD_SERIF_BOLD 4

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
#define UD_OP_CELLS   9
/* Localized variants are for fixed interface labels, never user data. */
#define UD_OP_LTEXT   10
#define UD_OP_LMEASURE 11
#define UD_OP_CLIP    12
#define UD_OP_GLASS   13
#define UD_OP_HOLD    14
/* Weave's marks (ProtonUI v2) */
#define UD_OP_GRAIN   15
#define UD_OP_STITCH  16
#define UD_OP_KNOT    17
#define UD_OP_THREAD  18

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
    /* ProtonUI v2 (docs/protonui-plan.md): the desktop fills these only
       for a program that says it knows them, so one built before them
       still gets exactly the eleven words above */
    uint32_t style;           /* UD_STYLE_*: the desktop's style */
    uint32_t dye[6];          /* Weave's dyes in this theme: UD_INDIGO.. */
    uint32_t thread;          /* the person's own focus colour */
    uint32_t grain;           /* the grain tile's ink, 0..255 (0: none) */
    uint32_t radius;          /* a control's corners */
    uint32_t density;         /* UD_POINTER or UD_FINGER */
};

/* UI_LOOK_MAX words (kernel/include/desktop.h): the kernel fills no
   more than the struct says, and a struct that grew without the kernel
   would be filled short - this stops the build instead */
typedef char ud_look_is_22_words[sizeof(struct ud_look) == 22 * 4 ? 1 : -1];

/* What kernel/include/desktop.h and look.h call them */
#define UD_STYLE_BAR_DOCK 0
#define UD_STYLE_TASKBAR  1
#define UD_STYLE_WEAVE    2
#define UD_STYLE_PHONE    3
#define UD_INDIGO    0       /* focus, the one thread; primary buttons */
#define UD_MADDER    1       /* close, delete, errors */
#define UD_SAFFRON   2       /* warnings, the microphone is on */
#define UD_WELD      3       /* done, connected, the camera is on */
#define UD_WOAD      4       /* links, the network */
#define UD_COCHINEAL 5       /* media: music, pictures, sound */
#define UD_POINTER   0
#define UD_FINGER    1

static inline void ud_look(struct ud_look *l)
{
    sys_ui_look_n(0, (uint32_t *)l, sizeof(*l) / sizeof(uint32_t));
}

/* The colours of a theme that may not be the one in use - to show it
   before it is chosen: UD_DARK or UD_LIGHT */
#define UD_DARK  1
#define UD_LIGHT 2
static inline void ud_look_of(int which, struct ud_look *l)
{
    sys_ui_look_n(which, (uint32_t *)l, sizeof(*l) / sizeof(uint32_t));
}

static inline int ud_op(int win, int op, int x, int y, int w, int h, int r,
                        uint32_t rgb, uint32_t rgb2, int font, int px,
                        const char *s)
{
    struct win_draw_cmd c = { op, x, y, w, h, r, rgb, rgb2, 255, font, px, s };
    return sys_win_draw(win, &c);
}

/* Everything drawn from now on held to x, y, w, h - text, shapes and
   pictures alike; ud_unclip lets it go */
static inline void ud_clip(int win, int x, int y, int w, int h)
{
    ud_op(win, UD_OP_CLIP, x, y, w > 0 ? w : 0, h > 0 ? h : 0, 0, 0, 0, 0, 0,
          (const char *)0);
    if (w <= 0 || h <= 0)              /* (an empty box hides all) */
        ud_op(win, UD_OP_CLIP, -1, -1, 1, 1, 0, 0, 0, 0, 0, (const char *)0);
}

/* A panel in the system's look (Liquid Glass, matte or solid on a
   phone) over what the window shows there: how light it came out,
   0..255 - dark ink on it above the middle, light below */
static inline int ud_glass(int win, int x, int y, int w, int h, int r)
{
    return ud_op(win, UD_OP_GLASS, x, y, w, h, r, 0, 0, 0, 0,
                 (const char *)0);
}

/* The window's pixels kept from the screen while a picture and what
   goes over it are drawn (1), and let go (0) */
static inline void ud_hold(int win, int on)
{
    ud_op(win, UD_OP_HOLD, 0, 0, on, 0, 0, 0, 0, 0, 0, (const char *)0);
}

static inline void ud_unclip(int win)
{
    ud_op(win, UD_OP_CLIP, 0, 0, 0, 0, 0, 0, 0, 0, 0, (const char *)0);
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

static inline int ud_text_l(int win, int x, int y, const char *s,
                            uint32_t rgb, int font, int px)
{
    return ud_op(win, UD_OP_LTEXT, x, y, 0, 0, 0, rgb, 0, font, px, s);
}

/* Text on a grid of cells 'cell' wide - a terminal's columns: each
   character starts at x + i * cell, whatever the font's own advance */
static inline int ud_cells(int win, int x, int y, int cell, const char *s,
                           uint32_t rgb, int font, int px)
{
    return ud_op(win, UD_OP_CELLS, x, y, cell, 0, 0, rgb, 0, font, px, s);
}

static inline int ud_width(const char *s, int font, int px)
{
    return ud_op(-1, UD_OP_MEASURE, 0, 0, 0, 0, 0, 0, 0, font, px, s);
}

static inline int ud_width_l(const char *s, int font, int px)
{
    return ud_op(-1, UD_OP_LMEASURE, 0, 0, 0, 0, 0, 0, 0, font, px, s);
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

static inline void ud_text_mid_l(int win, int x, int y, int w, int h,
                                 const char *s, uint32_t rgb, int font,
                                 int px)
{
    int tw = ud_width_l(s, font, px);
    ud_text_l(win, x + (w - tw) / 2, ud_baseline(y, h, px), s, rgb, font, px);
}

/* Text ending at x - numbers in a column line up on their last digit */
static inline void ud_text_right(int win, int x, int y, const char *s,
                                 uint32_t rgb, int font, int px)
{
    ud_text(win, x - ud_width(s, font, px), y, s, rgb, font, px);
}

static inline void ud_text_right_l(int win, int x, int y, const char *s,
                                   uint32_t rgb, int font, int px)
{
    ud_text_l(win, x - ud_width_l(s, font, px), y, s, rgb, font, px);
}

/* --- Weave's marks (ProtonUI v2, docs/WEAVE.md) ------------------------ */

/* The grain: the 3x3 woven tile over a rectangle, in the ink colour at
   'ink' of 255 - struct ud_look's grain says how much the style wants
   (0: none). Counted from the window's corner, so rectangles side by
   side are one cloth. */
static inline void ud_grain(int win, int x, int y, int w, int h,
                            uint32_t rgb, int ink)
{
    ud_op(win, UD_OP_GRAIN, x, y, w, h, ink, rgb, 0, 0, 0, (const char *)0);
}

/* A stitch, two pixels across: the line between parts of one surface,
   under a field at rest, the unset part of a slider */
#define UD_STITCH_W 2
static inline void ud_stitch(int win, int x, int y, int len, int vertical,
                             uint32_t rgb)
{
    ud_op(win, UD_OP_STITCH, x, y, vertical ? UD_STITCH_W : len,
          vertical ? len : UD_STITCH_W, 0, rgb, 0, 0, 0, (const char *)0);
}

/* A knot, d across: hollow is empty, ringed has something in it,
   filled is here */
#define UD_KNOT_HOLLOW 0
#define UD_KNOT_RINGED 1
#define UD_KNOT_FILLED 2
static inline void ud_knot(int win, int x, int y, int d, int state,
                           uint32_t rgb)
{
    ud_op(win, UD_OP_KNOT, x, y, d, d, state, rgb, 0, 0, 0, (const char *)0);
}

/* A thread: the curve from (x0, y0) to (x3, y3), pulled towards (x1, y1)
   and (x2, y2), 'width' sixteenths of a pixel wide, at 'a' of 255 */
static inline void ud_thread(int win, int x0, int y0, int x1, int y1,
                             int x2, int y2, int x3, int y3, int width,
                             uint32_t rgb, int a)
{
    struct win_draw_cmd c = {
        UD_OP_THREAD, x0, y0, x3, y3, width, rgb, 0, a,
        (int)((uint32_t)(uint16_t)x1 << 16 | (uint16_t)y1),
        (int)((uint32_t)(uint16_t)x2 << 16 | (uint16_t)y2), (const char *)0
    };
    sys_win_draw(win, &c);
}

#endif
