/* Weave's icons (docs/WEAVE.md)
 *
 * An icon in Weave is a tile of one dye with a few white strokes on it:
 * the tile carries the cloth's grain (a white weft every third row from
 * the bottom, a dark warp every third column), a darker last row for an
 * edge, a thin line along its foot - the thread - and the program's
 * mark drawn as strokes with round ends, on a grid of 48. The design
 * keeps the marks as SVG path data (docs/weave/canvas/Icon.dc.html);
 * scripts/mkweaveicons.py flattens them into broken lines, and this file
 * strokes those at whatever size the icon is asked for.
 *
 * A stroke's pixel is covered by how far its centre lies from the
 * nearest line - its distance to the line's edge, from half a pixel
 * outside to half a pixel inside - so ends and joins come out round
 * without being drawn, and each pixel takes the most any line covers
 * it, then is coloured once: lines that meet do not darken where they
 * overlap. All in whole numbers, 1/256 of a pixel.
 */

#include <stdint.h>
#include "include/weave.h"
#include "include/gfx.h"
#include "include/uitext.h"
#include "include/string.h"

struct wv_icon {
    const char *name;
    uint32_t dye;
    uint16_t run0, runs, pt0, disc0, discs;
};
#include "weaveicons.h"

/* The largest icon drawn through the mask, in the target's pixels: the
   launcher's 64 at twice the scale, with room */
#define MASK_MAX 192
static uint8_t mask[MASK_MAX * MASK_MAX];

int weave_icon_find(const char *name)
{
    for (int i = 0; i < WV_ICON_N; i++)
        if (!strcmp(wv_icons[i].name, name))
            return i;
    return -1;
}

uint32_t weave_icon_dye(int icon)
{
    return icon >= 0 && icon < WV_ICON_N ? wv_icons[icon].dye : WEAVE_LETTER_DYE;
}

/* The dyes a program without an icon of its own gets one of, by its
   letter: the design's own (Icon.dc.html), so that such a tile sits
   among the others */
static const uint32_t letter_dyes[] = {
    0x4A6BE0, 0x1FA3A3, 0xB23D8C, 0xC77F0A, 0x2F9A5C, 0x6A4FD6, 0xC9302C,
};

uint32_t weave_letter_dye(char letter)
{
    return letter_dyes[(uint8_t)letter % (sizeof(letter_dyes) /
                                          sizeof(letter_dyes[0]))];
}

static uint32_t isqrt64(uint64_t v)
{
    uint64_t r = 0, bit = (uint64_t)1 << 62;
    while (bit > v)
        bit >>= 2;
    while (bit) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return (uint32_t)r;
}

/* A point of the grid (sixteenths of a unit) in the target's pixels,
   in 1/256 of one, for an icon 'ps' pixels across */
static int64_t at256(int g, int ps)
{
    return (int64_t)g * ps * 256 / (48 * WV_UNIT);
}

/* Coverage of the pixels round one piece of line, kept as the most
   any piece gives each */
static void cover_line(int ps, int64_t ax, int64_t ay, int64_t bx, int64_t by,
                       int64_t half)
{
    int64_t reach = half + 256;
    int64_t x0 = (ax < bx ? ax : bx) - reach, x1 = (ax > bx ? ax : bx) + reach;
    int64_t y0 = (ay < by ? ay : by) - reach, y1 = (ay > by ? ay : by) + reach;
    int px0 = x0 < 0 ? 0 : (int)(x0 >> 8), px1 = (int)(x1 >> 8);
    int py0 = y0 < 0 ? 0 : (int)(y0 >> 8), py1 = (int)(y1 >> 8);
    if (px1 >= ps) px1 = ps - 1;
    if (py1 >= ps) py1 = ps - 1;
    int64_t dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy;
    for (int py = py0; py <= py1; py++)
        for (int px = px0; px <= px1; px++) {
            int64_t cx = ((int64_t)px << 8) + 128, cy = ((int64_t)py << 8) + 128;
            int64_t qx = ax, qy = ay;
            if (len2) {
                int64_t t = (cx - ax) * dx + (cy - ay) * dy;
                if (t >= len2) {
                    qx = bx;
                    qy = by;
                } else if (t > 0) {
                    qx = ax + dx * t / len2;
                    qy = ay + dy * t / len2;
                }
            }
            int64_t ex = cx - qx, ey = cy - qy;
            int64_t c = half + 128 - (int64_t)isqrt64((uint64_t)(ex * ex + ey * ey));
            if (c <= 0)
                continue;
            int a = c >= 256 ? 255 : (int)(c * 255 / 256);
            uint8_t *m = &mask[py * ps + px];
            if (a > *m)
                *m = (uint8_t)a;
        }
}

static void cover_disc(int ps, int64_t cx0, int64_t cy0, int64_t r)
{
    int px0 = (int)((cx0 - r - 256) >> 8), px1 = (int)((cx0 + r + 256) >> 8);
    int py0 = (int)((cy0 - r - 256) >> 8), py1 = (int)((cy0 + r + 256) >> 8);
    if (px0 < 0) px0 = 0;
    if (py0 < 0) py0 = 0;
    if (px1 >= ps) px1 = ps - 1;
    if (py1 >= ps) py1 = ps - 1;
    for (int py = py0; py <= py1; py++)
        for (int px = px0; px <= px1; px++) {
            int64_t ex = ((int64_t)px << 8) + 128 - cx0;
            int64_t ey = ((int64_t)py << 8) + 128 - cy0;
            int64_t c = r + 128 - (int64_t)isqrt64((uint64_t)(ex * ex + ey * ey));
            if (c <= 0)
                continue;
            int a = c >= 256 ? 255 : (int)(c * 255 / 256);
            uint8_t *m = &mask[py * ps + px];
            if (a > *m)
                *m = (uint8_t)a;
        }
}

/* An icon's strokes and dots into the mask, cleared first: strokes
   'sw' tenths of a unit of the grid wide */
static void cover_mark(int icon, int ps, int sw)
{
    int64_t half = at256(sw * WV_UNIT / 20, ps);
    memset(mask, 0, (uint32_t)(ps * ps));
    const struct wv_icon *ic = &wv_icons[icon];
    int pt = ic->pt0;
    for (int r = 0; r < ic->runs; r++) {
        int n = wv_runs[ic->run0 + r];
        for (int i = 0; i + 1 < n; i++) {
            const int16_t *a = &wv_pts[2 * (pt + i)], *b = a + 2;
            cover_line(ps, at256(a[0], ps), at256(a[1], ps),
                       at256(b[0], ps), at256(b[1], ps), half);
        }
        if (n == 1)
            cover_disc(ps, at256(wv_pts[2 * pt], ps),
                       at256(wv_pts[2 * pt + 1], ps), half);
        pt += n;
    }
    for (int d = 0; d < ic->discs; d++) {
        const int16_t *c = &wv_discs[3 * (ic->disc0 + d)];
        cover_disc(ps, at256(c[0], ps), at256(c[1], ps), at256(c[2], ps));
    }
}

/* The mask laid on in one colour, at 'a' of 255 */
static void lay_mask(const struct gfx_surface *t, int x, int y, int ps,
                     uint32_t c, int a)
{
    for (int py = 0; py < ps; py++)
        for (int px = 0; px < ps; px++) {
            int m = mask[py * ps + px];
            if (m)
                gfx_pblend_s(t, x + px, y + py, c, m * a / 255);
        }
}

/* The tile: the dye, the grain, the edge. In the target's pixels. */
static void tile(const struct gfx_surface *t, int x, int y, int ps,
                 uint32_t dye, int edge)
{
    for (int py = 0; py < ps; py++)
        for (int px = 0; px < ps; px++)
            gfx_pblend_s(t, x + px, y + py, dye, 255);
    /* the warp, dark, every third column from the left; the weft, light,
       every third row from the bottom, over it (the design's two
       repeating gradients, the light one on top) */
    for (int py = 0; py < ps; py++)
        for (int px = 0; px < ps; px += 3)
            gfx_pblend_s(t, x + px, y + py, 0x000000, WEAVE_WARP_INK);
    for (int py = ps - 1; py >= 0; py -= 3)
        for (int px = 0; px < ps; px++)
            gfx_pblend_s(t, x + px, y + py, 0xFFFFFF, WEAVE_WEFT_INK);
    for (int py = ps - edge; py < ps; py++)
        for (int px = 0; px < ps; px++)
            gfx_pblend_s(t, x + px, y + py, 0x000000, WEAVE_EDGE_INK);
}

void weave_icon_draw(const struct gfx_surface *t, int icon, int x, int y,
                     int s, char letter)
{
    int k = gfx_k(t);
    int ps = (s * k + 8) / 16;
    int X = (x * k) / 16, Y = (y * k) / 16;
    if (ps < 4)
        return;
    uint32_t dye = icon >= 0 && icon < WV_ICON_N ? wv_icons[icon].dye
                                                  : weave_letter_dye(letter);
    /* the edge: a thirty-second of the tile, one pixel at least */
    int edge = ((s + 16) / 32 > 1 ? (s + 16) / 32 : 1) * k / 16;
    tile(t, X, Y, ps, dye, edge > 0 ? edge : 1);
    int grey = dye == 0x2A2D36 || dye == 0x4B505E;

    if (icon < 0 || icon >= WV_ICON_N || ps > MASK_MAX) {
        /* no mark of its own (or too big to draw through the mask): the
           letter, in the bold */
        char txt[2] = { letter ? letter : '?', 0 };
        int px = s * 11 / 20;
        int tw = ui_font_width(UI_BOLD, px, txt);
        ui_font_text_s(t, UI_BOLD, px, x + (s - tw) / 2,
                       ui_font_baseline_in(px, y, s), txt, 0xFFFFFF);
        return;
    }

    /* the thread along its foot: the design's M6 42.5 H42, 1.6 wide */
    memset(mask, 0, (uint32_t)(ps * ps));
    cover_line(ps, at256(6 * WV_UNIT, ps), at256(425 * WV_UNIT / 10, ps),
               at256(42 * WV_UNIT, ps), at256(425 * WV_UNIT / 10, ps),
               at256(8 * WV_UNIT / 10, ps));
    lay_mask(t, X, Y, ps, grey ? WEAVE_GREY_TINT : 0xFFFFFF,
             grey ? WEAVE_GREY_TINT_A : WEAVE_TINT_A);

    /* the mark: 3.6 units wide when small, 3 as a rule, 2.6 large */
    cover_mark(icon, ps, s < 32 ? 36 : s < 72 ? 30 : 26);
    lay_mask(t, X, Y, ps, 0xFFFFFF, 255);
}

void weave_glyph_draw(const struct gfx_surface *t, int icon, int x, int y,
                      int s, uint32_t c, int stroke)
{
    int k = gfx_k(t);
    int ps = (s * k + 8) / 16;
    if (icon < 0 || icon >= WV_ICON_N || ps < 4 || ps > MASK_MAX)
        return;
    cover_mark(icon, ps, stroke);
    lay_mask(t, (x * k) / 16, (y * k) / 16, ps, c, 255);
}
