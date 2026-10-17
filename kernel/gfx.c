/* Drawing with soft edges and light let through
 *
 * Everything the desktop draws used to be a rectangle with square
 * corners in one of a few dozen palette colours. This file is what a
 * screen of real colours makes possible: corners that are round, and
 * round without steps; circles; shadows that fade; glass that blurs
 * what is behind it.
 *
 * All of it rests on one idea. A shape's edge seldom falls exactly
 * between two pixels, so a pixel on the edge is partly inside - and it
 * gets that part of the colour, mixed with what was already there:
 *
 *     out = (colour * a + under * (255 - a) + 127) / 255, per channel
 *
 * which is the same mix kernel/uitext.c gives the letters. How much is
 * inside is worked out from the distance between the pixel's centre
 * and the edge: half a pixel or more inside counts as all of it, half
 * a pixel or more outside as none, and in between it goes evenly. For
 * a straight edge that is the exact area; for a curve it is within a
 * few 1/255ths of it, which the test measures.
 *
 * Distances are kept in whole numbers - sixteenths of a pixel, and for
 * the centre of a shape, which can fall on half a sixteenth, twice
 * that. Nothing here is a floating point number: the kernel does not
 * save the FPU's state for itself.
 */

#include <stdint.h>
#include "include/gfx.h"
#include "include/video.h"
#include "include/pmm.h"
#include "include/printk.h"

/* --- where drawing goes ------------------------------------------------- */

static struct gfx_surface tgt;
static int to_screen = 1;

void gfx_target(const struct gfx_surface *s)
{
    if (s) {
        tgt = *s;
        to_screen = 0;
    } else {
        to_screen = 1;
    }
}

/* What the compositor draws on unless told otherwise; everything below
   takes its target as a parameter ('t', NULL = the screen), so that a
   program drawing into its window through a system call - on another
   core, or in the middle of a frame - never touches anybody else's. */
static const struct gfx_surface *cur(void)
{
    return to_screen ? (const struct gfx_surface *)0 : &tgt;
}

static int tw(const struct gfx_surface *t) { return t ? t->w : video_w(); }
static int th(const struct gfx_surface *t) { return t ? t->h : video_h(); }

/* The row as words, or NULL when the pixels have to go one at a time
   through video.c (a screen that is not four bytes a pixel) */
static uint32_t *row_of(const struct gfx_surface *t, int y)
{
    if (!t)
        return video_row32(y);
    if ((unsigned)y >= (unsigned)t->h)
        return (uint32_t *)0;
    return t->px + (long)y * t->stride;
}

/* Division by 255 without dividing: exact for everything up to 65534,
   and the largest number the mix makes is 255 * 255 + 127. */
static inline uint32_t div255(uint32_t m)
{
    return (m + 1 + (m >> 8)) >> 8;
}

uint32_t gfx_mix(uint32_t over, uint32_t under, int a)
{
    if (a >= 255)
        return over & 0xFFFFFF;
    if (a <= 0)
        return under & 0xFFFFFF;
    uint32_t b = 255 - (uint32_t)a, out = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        uint32_t o = (over >> sh) & 0xFF, u = (under >> sh) & 0xFF;
        out |= div255(o * (uint32_t)a + u * b + 127) << sh;
    }
    return out;
}

uint32_t gfx_get_s(const struct gfx_surface *t, int x, int y)
{
    if ((unsigned)x >= (unsigned)tw(t) || (unsigned)y >= (unsigned)th(t))
        return 0;
    uint32_t *row = row_of(t, y);
    if (row)
        return row[x] & 0xFFFFFF;
    return video_getpixel_rgb(x, y);
}

void gfx_put_s(const struct gfx_surface *t, int x, int y, uint32_t c)
{
    if ((unsigned)x >= (unsigned)tw(t) || (unsigned)y >= (unsigned)th(t))
        return;
    uint32_t *row = row_of(t, y);
    if (row)
        row[x] = c & 0xFFFFFF;
    else
        video_putpixel_rgb(x, y, c);
}

void gfx_blend_s(const struct gfx_surface *t, int x, int y, uint32_t c, int a)
{
    if (a <= 0 || (unsigned)x >= (unsigned)tw(t) ||
        (unsigned)y >= (unsigned)th(t))
        return;
    uint32_t *row = row_of(t, y);
    if (row) {
        row[x] = a >= 255 ? (c & 0xFFFFFF) : gfx_mix(c, row[x], a);
        return;
    }
    video_putpixel_rgb(x, y, a >= 255 ? c : gfx_mix(c, video_getpixel_rgb(x, y), a));
}

/* One run of a row, clipped once */
static void span_s(const struct gfx_surface *t, int x0, int x1, int y, uint32_t c, int a)
{
    if ((unsigned)y >= (unsigned)th(t) || a <= 0)
        return;
    if (x0 < 0)
        x0 = 0;
    if (x1 > tw(t))
        x1 = tw(t);
    uint32_t *row = row_of(t, y);
    if (!row) {
        for (int x = x0; x < x1; x++)
            gfx_blend_s(t, x, y, c, a);
        return;
    }
    if (a >= 255) {
        for (int x = x0; x < x1; x++)
            row[x] = c & 0xFFFFFF;
        return;
    }
    /* The same colour over a run: what it contributes is worked out
       once, and only what is under it changes */
    uint32_t b = 255 - (uint32_t)a;
    uint32_t cr = ((c >> 16) & 0xFF) * (uint32_t)a + 127;
    uint32_t cg = ((c >> 8) & 0xFF) * (uint32_t)a + 127;
    uint32_t cb = (c & 0xFF) * (uint32_t)a + 127;
    for (int x = x0; x < x1; x++) {
        uint32_t u = row[x];
        row[x] = (div255(cr + ((u >> 16) & 0xFF) * b) << 16) |
                 (div255(cg + ((u >> 8) & 0xFF) * b) << 8) |
                 div255(cb + (u & 0xFF) * b);
    }
}

void gfx_fill_s(const struct gfx_surface *t, int x, int y, int w, int h, uint32_t c, int a)
{
    for (int j = 0; j < h; j++)
        span_s(t, x, x + w, y + j, c, a);
}

static uint32_t lerp_rgb(uint32_t p, uint32_t q, int num, int den)
{
    if (den <= 0)
        return p;
    uint32_t out = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        int a = (int)((p >> sh) & 0xFF), b = (int)((q >> sh) & 0xFF);
        out |= (uint32_t)(a + (b - a) * num / den) << sh;
    }
    return out;
}

void gfx_fill_grad_s(const struct gfx_surface *t, int x, int y, int w, int h, uint32_t top,
                   uint32_t bottom, int a)
{
    for (int j = 0; j < h; j++)
        span_s(t, x, x + w, y + j, lerp_rgb(top, bottom, j, h - 1), a);
}

/* --- how much of a pixel a shape covers ----------------------------------- */

static uint32_t isqrt(uint32_t v)
{
    uint32_t r = 0, bit = 1u << 30;
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
    return r;
}

/* ... to the nearest whole number rather than down: the distances
   below are in thirty-seconds of a pixel, and always cutting down would
   make every edge a little fatter than it is */
static uint32_t isqrt_near(uint32_t v)
{
    uint32_t r = isqrt(v);
    return v - r * r > r ? r + 1 : r;
}

static int iabs(int v) { return v < 0 ? -v : v; }

/* A signed distance in 1/32 of a pixel -> how much is covered.
   Half a pixel inside (-16) and further is all of it. */
static int cov_of(int d)
{
    if (d <= -16)
        return 255;
    if (d >= 16)
        return 0;
    return (16 - d) * 255 / 32;
}

int gfx_cov_rrect16(int px, int py, int x, int y, int w, int h, int r)
{
    if (w <= 0 || h <= 0)
        return 0;
    if (r < 0)
        r = 0;
    if (2 * r > w)
        r = w / 2;
    if (2 * r > h)
        r = h / 2;
    /* Twice everything, so that the centre of the shape - which can
       fall half way between two sixteenths - is a whole number */
    int qx = iabs(px * 32 + 16 - (2 * x + w)) - (w - 2 * r);
    int qy = iabs(py * 32 + 16 - (2 * y + h)) - (h - 2 * r);
    int lim = 2 * r + 32;
    if (qx > lim || qy > lim)
        return 0;                       /* far outside */
    int ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    int out;
    if (!ox)
        out = oy;
    else if (!oy)
        out = ox;
    else
        out = (int)isqrt_near((uint32_t)(ox * ox + oy * oy));
    int in = qx > qy ? qx : qy;
    if (in > 0)
        in = 0;
    return cov_of(out + in - 2 * r);
}

int gfx_cov_line16(int px, int py, int x0, int y0, int x1, int y1,
                   int thick)
{
    int X = px * 16 + 8, Y = py * 16 + 8;
    int dx = x1 - x0, dy = y1 - y0;
    int cx = x0, cy = y0;
    uint32_t len2 = (uint32_t)(dx * dx + dy * dy);
    int dot = (X - x0) * dx + (Y - y0) * dy;
    if (len2 && dot > 0) {
        if ((uint32_t)dot >= len2) {
            cx = x1;
            cy = y1;
        } else {
            uint32_t d = (uint32_t)dot, l = len2;
            while (d > 0xFFFFF) {       /* room for the shift below */
                d >>= 1;
                l >>= 1;
            }
            int t = (int)((d << 12) / l);       /* 0..4095, in 1/4096 */
            cx = x0 + dx * t / 4096;
            cy = y0 + dy * t / 4096;
        }
    }
    /* The distance in thirty-seconds where the numbers allow it - that
       is, anywhere near the line, which is where it matters */
    int ex = X - cx, ey = Y - cy;
    uint32_t e2 = (uint32_t)(ex * ex + ey * ey);
    int dist = e2 < (1u << 29) ? (int)isqrt_near(e2 * 4)
                               : 2 * (int)isqrt(e2);
    return cov_of(dist - thick);
}

/* --- shapes ----------------------------------------------------------------- */

/* The pixels a shape in sixteenths can touch */
static void bounds16(int x, int y, int w, int h, int *px0, int *py0,
                     int *px1, int *py1)
{
    *px0 = x >= 0 ? x / 16 : -((15 - x) / 16);
    *py0 = y >= 0 ? y / 16 : -((15 - y) / 16);
    *px1 = (x + w + 15) / 16;
    *py1 = (y + h + 15) / 16;
}

/* The work of every filled rounded rectangle. A row is scanned in from
   both ends only until the coverage is whole; what lies between is one
   run of the colour. The shape is convex and symmetric, so if the
   middle of a row is not covered, nothing in it is. */
static void rrect_rows_s(const struct gfx_surface *t, int x, int y, int w, int h, int r, uint32_t top,
                       uint32_t bottom, int a)
{
    int px0, py0, px1, py1;
    bounds16(x, y, w, h, &px0, &py0, &px1, &py1);
    if (py0 < 0)
        py0 = 0;
    if (py1 > th(t))
        py1 = th(t);
    int mid = (px0 + px1 - 1) / 2;
    int rows = py1 - py0;
    for (int py = py0; py < py1; py++) {
        uint32_t c = top == bottom ? top
                     : lerp_rgb(top, bottom, py - py0, rows - 1);
        if (!gfx_cov_rrect16(mid, py, x, y, w, h, r))
            continue;
        int l = px0, rr = px1 - 1;
        for (; l <= rr; l++) {
            int cv = gfx_cov_rrect16(l, py, x, y, w, h, r);
            if (cv >= 255)
                break;
            gfx_blend_s(t, l, py, c, cv * a / 255);
        }
        for (; rr >= l; rr--) {
            int cv = gfx_cov_rrect16(rr, py, x, y, w, h, r);
            if (cv >= 255)
                break;
            gfx_blend_s(t, rr, py, c, cv * a / 255);
        }
        if (l <= rr)
            span_s(t, l, rr + 1, py, c, a);
    }
}

void gfx_rrect16_s(const struct gfx_surface *t, int x, int y, int w, int h, int r, uint32_t c, int a)
{
    rrect_rows_s(t, x, y, w, h, r, c, c, a);
}

void gfx_rrect_s(const struct gfx_surface *t, int x, int y, int w, int h, int r, uint32_t c, int a)
{
    rrect_rows_s(t, x * 16, y * 16, w * 16, h * 16, r * 16, c, c, a);
}

void gfx_rrect_grad_s(const struct gfx_surface *t, int x, int y, int w, int h, int r, uint32_t top,
                    uint32_t bottom, int a)
{
    rrect_rows_s(t, x * 16, y * 16, w * 16, h * 16, r * 16, top, bottom, a);
}

void gfx_rring16_s(const struct gfx_surface *t, int x, int y, int w, int h, int r, int thick, uint32_t c,
                 int a)
{
    int px0, py0, px1, py1;
    bounds16(x, y, w, h, &px0, &py0, &px1, &py1);
    int ir = r - thick > 0 ? r - thick : 0;
    int ix = x + thick, iy = y + thick, iw = w - 2 * thick, ih = h - 2 * thick;
    if (py0 < 0)
        py0 = 0;
    if (py1 > th(t))
        py1 = th(t);
    for (int py = py0; py < py1; py++) {
        /* From each end until the pixel is wholly inside the hole:
           from there to the middle there is no edge to draw */
        int l = px0, rr = px1 - 1;
        for (; l <= rr; l++) {
            int inner = iw > 0 && ih > 0
                        ? gfx_cov_rrect16(l, py, ix, iy, iw, ih, ir) : 0;
            int cv = gfx_cov_rrect16(l, py, x, y, w, h, r) - inner;
            if (cv > 0)
                gfx_blend_s(t, l, py, c, cv * a / 255);
            if (inner >= 255)
                break;
        }
        for (; rr > l; rr--) {
            int inner = iw > 0 && ih > 0
                        ? gfx_cov_rrect16(rr, py, ix, iy, iw, ih, ir) : 0;
            int cv = gfx_cov_rrect16(rr, py, x, y, w, h, r) - inner;
            if (cv > 0)
                gfx_blend_s(t, rr, py, c, cv * a / 255);
            if (inner >= 255)
                break;
        }
    }
}

void gfx_disc16_s(const struct gfx_surface *t, int cx, int cy, int rad, uint32_t c, int a)
{
    rrect_rows_s(t, cx - rad, cy - rad, 2 * rad, 2 * rad, rad, c, c, a);
}

void gfx_circle16_s(const struct gfx_surface *t, int cx, int cy, int rad, int thick, uint32_t c, int a)
{
    gfx_rring16_s(t, cx - rad, cy - rad, 2 * rad, 2 * rad, rad, thick, c, a);
}

void gfx_line16_s(const struct gfx_surface *t, int x0, int y0, int x1, int y1, int thick, uint32_t c,
                int a)
{
    int m = thick / 2 + 16;
    int lx = (x0 < x1 ? x0 : x1) - m, ly = (y0 < y1 ? y0 : y1) - m;
    int hx = (x0 > x1 ? x0 : x1) + m, hy = (y0 > y1 ? y0 : y1) + m;
    int px0, py0, px1, py1;
    bounds16(lx, ly, hx - lx, hy - ly, &px0, &py0, &px1, &py1);
    for (int py = py0; py < py1; py++)
        for (int px = px0; px < px1; px++) {
            int cv = gfx_cov_line16(px, py, x0, y0, x1, y1, thick);
            if (cv)
                gfx_blend_s(t, px, py, c, cv * a / 255);
        }
}

/* --- shadows ------------------------------------------------------------------ */

void gfx_shadow_s(const struct gfx_surface *t, int x, int y, int w, int h, int r, int s, int a, int dy)
{
    if (s <= 0 || a <= 0 || w <= 0 || h <= 0)
        return;
    if (2 * r > w)
        r = w / 2;
    if (2 * r > h)
        r = h / 2;
    /* How dark, by how far out: a * (1 - d/s)^2. A straight fall would
       end in a visible line; the square of it fades into nothing. The
       distances are in half pixels, so the table is 2s long. */
    static uint8_t fall[2 * 64 + 1];
    int s2 = 2 * s;
    if (s2 > 128)
        s2 = 128;
    for (int d = 0; d <= s2; d++)
        fall[d] = (uint8_t)(a * (s2 - d) * (s2 - d) / (s2 * s2));

    /* The shadow is the shape moved down by dy; what is skipped is the
       shape where it really is, which covers it */
    int sy = y + dy;
    int cx2 = 2 * x + w, cy2 = 2 * sy + h;
    int y0 = sy - s, y1 = sy + h + s;
    if (y0 < 0)
        y0 = 0;
    if (y1 > th(t))
        y1 = th(t);
    for (int py = y0; py < y1; py++) {
        int qy = iabs(2 * py + 1 - cy2) - (h - 2 * r);
        int in_rows = py >= y && py < y + h;
        int in_core = py >= y + r && py < y + h - r;
        for (int px = x - s; px < x + w + s; px++) {
            /* Wholly inside the shape - the window goes there */
            if (in_core && px >= x && px < x + w) {
                px = x + w - 1;
                continue;
            }
            if (in_rows && px >= x + r && px < x + w - r) {
                px = x + w - r - 1;
                continue;
            }
            int qx = iabs(2 * px + 1 - cx2) - (w - 2 * r);
            int ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
            int out = !ox ? oy : !oy ? ox
                      : (int)isqrt((uint32_t)(ox * ox + oy * oy));
            int d = out - 2 * r;
            if (d < 0)
                d = 0;
            if (d >= s2)
                continue;
            gfx_blend_s(t, px, py, 0x000000, fall[d]);
        }
    }
}

/* --- blur ------------------------------------------------------------------- */

/* Two buffers, each room for 81920 pixels: a menu bar across a wide
   screen, a dock, a menu - with what lies around them that the blur
   reads. Taken from the frame allocator the first time glass is drawn,
   and kept. */
#define BLUR_PX 81920
static uint32_t *blur_a, *blur_b;

static int blur_room(void)
{
    if (blur_a)
        return 1;
    uint32_t pages = (BLUR_PX * 4 + 4095) / 4096;
    uint32_t a = pmm_alloc_contiguous(pages);
    uint32_t b = a ? pmm_alloc_contiguous(pages) : 0;
    if (!a || !b)
        return 0;
    blur_a = (uint32_t *)(uintptr_t)a;
    blur_b = (uint32_t *)(uintptr_t)b;
    return 1;
}

/* One pass of the box along rows: 'in' is iw wide, 'out' is iw - 2r */
static void box_rows(const uint32_t *in, uint32_t *out, int iw, int rows,
                     int r)
{
    int n = 2 * r + 1, ow = iw - 2 * r;
    uint32_t inv = (65536u + (uint32_t)n / 2) / (uint32_t)n;
    for (int j = 0; j < rows; j++) {
        const uint32_t *s = in + (long)j * iw;
        uint32_t *d = out + (long)j * ow;
        uint32_t sr = 0, sg = 0, sb = 0;
        for (int k = 0; k < n; k++) {
            sr += (s[k] >> 16) & 0xFF;
            sg += (s[k] >> 8) & 0xFF;
            sb += s[k] & 0xFF;
        }
        for (int i = 0; i < ow; i++) {
            d[i] = (((sr * inv + 32768) >> 16) << 16) |
                   (((sg * inv + 32768) >> 16) << 8) |
                   ((sb * inv + 32768) >> 16);
            if (i + 1 < ow) {
                uint32_t o = s[i], e = s[i + n];
                sr += ((e >> 16) & 0xFF) - ((o >> 16) & 0xFF);
                sg += ((e >> 8) & 0xFF) - ((o >> 8) & 0xFF);
                sb += (e & 0xFF) - (o & 0xFF);
            }
        }
    }
}

/* ... and down the columns: 'in' has ih rows, 'out' ih - 2r */
static void box_cols(const uint32_t *in, uint32_t *out, int w, int ih,
                     int r)
{
    int n = 2 * r + 1, oh = ih - 2 * r;
    uint32_t inv = (65536u + (uint32_t)n / 2) / (uint32_t)n;
    for (int i = 0; i < w; i++) {
        uint32_t sr = 0, sg = 0, sb = 0;
        for (int k = 0; k < n; k++) {
            uint32_t p = in[(long)k * w + i];
            sr += (p >> 16) & 0xFF;
            sg += (p >> 8) & 0xFF;
            sb += p & 0xFF;
        }
        for (int j = 0; j < oh; j++) {
            out[(long)j * w + i] = (((sr * inv + 32768) >> 16) << 16) |
                                   (((sg * inv + 32768) >> 16) << 8) |
                                   ((sb * inv + 32768) >> 16);
            if (j + 1 < oh) {
                uint32_t o = in[(long)j * w + i], e = in[(long)(j + n) * w + i];
                sr += ((e >> 16) & 0xFF) - ((o >> 16) & 0xFF);
                sg += ((e >> 8) & 0xFF) - ((o >> 8) & 0xFF);
                sb += (e & 0xFF) - (o & 0xFF);
            }
        }
    }
}

int gfx_blur_s(const struct gfx_surface *t, int x, int y, int w, int h, int radius)
{
    if (w <= 0 || h <= 0 || radius <= 0)
        return 0;
    int e = 2 * radius;               /* two passes eat 2r on each side */
    int w1 = w + 2 * e, h1 = h + 2 * e;
    if ((long)w1 * h1 > BLUR_PX || !blur_room())
        return -1;

    /* What is read beyond the screen is the nearest pixel on it: an
       edge of glass against the edge of the screen should not go dark
       because there is nothing out there. */
    int sw = tw(t), sh = th(t);
    for (int j = 0; j < h1; j++) {
        int sy = y - e + j;
        sy = sy < 0 ? 0 : sy >= sh ? sh - 1 : sy;
        for (int i = 0; i < w1; i++) {
            int sx = x - e + i;
            sx = sx < 0 ? 0 : sx >= sw ? sw - 1 : sx;
            blur_a[(long)j * w1 + i] = gfx_get_s(t, sx, sy);
        }
    }
    int w2 = w1 - 2 * radius, h2 = h1 - 2 * radius;
    box_rows(blur_a, blur_b, w1, h1, radius);       /* w2 x h1 */
    box_cols(blur_b, blur_a, w2, h1, radius);       /* w2 x h2 */
    box_rows(blur_a, blur_b, w2, h2, radius);       /* w  x h2 */
    box_cols(blur_b, blur_a, w, h2, radius);        /* w  x h  */
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            gfx_put_s(t, x + i, y + j, blur_a[(long)j * w + i]);
    return 0;
}

/* --- round corners on something already drawn --------------------------------- */

static void corner_xy(int k, int x, int y, int w, int h, int r, int *cx,
                      int *cy)
{
    *cx = (k & 1) ? x + w - r : x;
    *cy = (k & 2) ? y + h - r : y;
}

void gfx_round_save_s(const struct gfx_surface *t, int x, int y, int w, int h, int r, uint32_t *keep)
{
    for (int k = 0; k < 4; k++) {
        int cx, cy;
        corner_xy(k, x, y, w, h, r, &cx, &cy);
        for (int j = 0; j < r; j++)
            for (int i = 0; i < r; i++)
                *keep++ = gfx_get_s(t, cx + i, cy + j);
    }
}

void gfx_round_apply_s(const struct gfx_surface *t, int x, int y, int w, int h, int r,
                     const uint32_t *keep)
{
    for (int k = 0; k < 4; k++) {
        int cx, cy;
        corner_xy(k, x, y, w, h, r, &cx, &cy);
        for (int j = 0; j < r; j++)
            for (int i = 0; i < r; i++, keep++) {
                int cv = gfx_cov_rrect16(cx + i, cy + j, x * 16, y * 16,
                                         w * 16, h * 16, r * 16);
                if (cv < 255)
                    gfx_put_s(t, cx + i, cy + j,
                            gfx_mix(gfx_get_s(t, cx + i, cy + j), *keep, cv));
            }
    }
}

void gfx_frost_s(const struct gfx_surface *t, int x, int y, int w, int h, int r, int blur, uint32_t tint,
               int a)
{
    static uint32_t keep[4 * 32 * 32];
    if (r > 32)
        r = 32;
    gfx_round_save_s(t, x, y, w, h, r, keep);
    if (blur > 0)
        gfx_blur_s(t, x, y, w, h, blur);
    gfx_fill_s(t, x, y, w, h, tint, a);
    gfx_round_apply_s(t, x, y, w, h, r, keep);
}

void gfx_blit_round_s(const struct gfx_surface *t, int x, int y, int w, int h, int r, const uint32_t *src,
                    int stride, int a)
{
    for (int j = 0; j < h; j++) {
        int edge_row = j < r || j >= h - r;
        for (int i = 0; i < w; i++) {
            int cv = 255;
            if (edge_row && (i < r || i >= w - r))
                cv = gfx_cov_rrect16(x + i, y + j, x * 16, y * 16, w * 16,
                                     h * 16, r * 16);
            if (cv)
                gfx_blend_s(t, x + i, y + j, src[(long)j * stride + i],
                          cv * a / 255);
        }
    }
}


/* --- the same, onto the compositor's current target ----------------------- */

uint32_t gfx_get(int x, int y)
{
    return gfx_get_s(cur(), x, y);
}

void gfx_put(int x, int y, uint32_t c)
{
    gfx_put_s(cur(), x, y, c);
}

void gfx_blend(int x, int y, uint32_t c, int a)
{
    gfx_blend_s(cur(), x, y, c, a);
}

void gfx_fill(int x, int y, int w, int h, uint32_t c, int a)
{
    gfx_fill_s(cur(), x, y, w, h, c, a);
}

void gfx_fill_grad(int x, int y, int w, int h, uint32_t top,
                   uint32_t bottom, int a)
{
    gfx_fill_grad_s(cur(), x, y, w, h, top, bottom, a);
}

void gfx_rrect16(int x, int y, int w, int h, int r, uint32_t c, int a)
{
    gfx_rrect16_s(cur(), x, y, w, h, r, c, a);
}

void gfx_rrect(int x, int y, int w, int h, int r, uint32_t c, int a)
{
    gfx_rrect_s(cur(), x, y, w, h, r, c, a);
}

void gfx_rrect_grad(int x, int y, int w, int h, int r, uint32_t top,
                    uint32_t bottom, int a)
{
    gfx_rrect_grad_s(cur(), x, y, w, h, r, top, bottom, a);
}

void gfx_rring16(int x, int y, int w, int h, int r, int thick, uint32_t c,
                 int a)
{
    gfx_rring16_s(cur(), x, y, w, h, r, thick, c, a);
}

void gfx_disc16(int cx, int cy, int rad, uint32_t c, int a)
{
    gfx_disc16_s(cur(), cx, cy, rad, c, a);
}

void gfx_circle16(int cx, int cy, int rad, int thick, uint32_t c, int a)
{
    gfx_circle16_s(cur(), cx, cy, rad, thick, c, a);
}

void gfx_line16(int x0, int y0, int x1, int y1, int thick, uint32_t c,
                int a)
{
    gfx_line16_s(cur(), x0, y0, x1, y1, thick, c, a);
}

void gfx_shadow(int x, int y, int w, int h, int r, int s, int a, int dy)
{
    gfx_shadow_s(cur(), x, y, w, h, r, s, a, dy);
}

int gfx_blur(int x, int y, int w, int h, int radius)
{
    return gfx_blur_s(cur(), x, y, w, h, radius);
}

void gfx_round_save(int x, int y, int w, int h, int r, uint32_t *keep)
{
    gfx_round_save_s(cur(), x, y, w, h, r, keep);
}

void gfx_round_apply(int x, int y, int w, int h, int r,
                     const uint32_t *keep)
{
    gfx_round_apply_s(cur(), x, y, w, h, r, keep);
}

void gfx_frost(int x, int y, int w, int h, int r, int blur, uint32_t tint,
               int a)
{
    gfx_frost_s(cur(), x, y, w, h, r, blur, tint, a);
}

void gfx_blit_round(int x, int y, int w, int h, int r, const uint32_t *src,
                    int stride, int a)
{
    gfx_blit_round_s(cur(), x, y, w, h, r, src, stride, a);
}

/* --- the 'shape' command ---------------------------------------------------------
 *
 *   shape rrect X Y W H R PW PH        a rounded rectangle, in sixteenths
 *   shape ring X Y W H R T PW PH       only its edge, T thick
 *   shape line X0 Y0 X1 Y1 T PW PH     a line with round ends
 *   shape blur R PW PH                 a pattern, blurred
 *
 * Each draws white onto a black canvas PW x PH of its own and prints
 * the canvas a row per line, a byte per pixel in hex: the red channel,
 * which for white on black is the coverage. Nobody reads it by eye -
 * the test compares it with the exact area of the shape. The blur
 * prints all three channels, because there they differ.
 */

static uint32_t sheet[64 * 64];

static int word_is(const char **p, const char *w)
{
    const char *s = *p;
    while (*s == ' ')
        s++;
    while (*w)
        if (*s++ != *w++)
            return 0;
    if (*s && *s != ' ')
        return 0;
    *p = s;
    return 1;
}

static int num(const char **p)
{
    int v = 0, neg = 0;
    while (**p == ' ')
        (*p)++;
    if (**p == '-') {
        neg = 1;
        (*p)++;
    }
    while (**p >= '0' && **p <= '9')
        v = v * 10 + (*(*p)++ - '0');
    return neg ? -v : v;
}

static void print_sheet(int pw, int ph, int channels)
{
    static const char hex[] = "0123456789abcdef";
    char line[64 * 6 + 1];
    for (int j = 0; j < ph; j++) {
        int n = 0;
        for (int i = 0; i < pw; i++) {
            uint32_t v = sheet[j * pw + i];
            for (int c = channels - 1; c >= 0; c--) {
                uint32_t b = (v >> (c * 8)) & 0xFF;
                line[n++] = hex[b >> 4];
                line[n++] = hex[b & 15];
            }
        }
        line[n] = 0;
        printk("%s\n", line);
    }
}

void gfx_command(const char *arg)
{
    const char *p = arg ? arg : "";
    int shape = word_is(&p, "rrect") ? 1 : word_is(&p, "ring") ? 2 :
                word_is(&p, "line") ? 3 : word_is(&p, "blur") ? 4 : 0;
    if (!shape) {
        printk("usage: shape rrect|ring|line|blur ...\n");
        return;
    }
    int v[8], n = shape == 2 ? 6 : shape == 4 ? 1 : 5;
    for (int i = 0; i < n; i++)
        v[i] = num(&p);
    int pw = num(&p), ph = num(&p);
    if (pw < 1 || ph < 1 || pw > 64 || ph > 64) {
        printk("shape: the canvas is 1..64 on each side\n");
        return;
    }
    struct gfx_surface s = { sheet, pw, ph, pw };
    const struct gfx_surface *t = &s;
    if (shape == 4) {
        /* A pattern with edges in it and no symmetry, so that a blur
           going the wrong way, or a pixel off, shows */
        for (int j = 0; j < ph; j++)
            for (int i = 0; i < pw; i++)
                sheet[j * pw + i] =
                    ((uint32_t)((i * 37 + j * 11) & 0xFF) << 16) |
                    ((uint32_t)(((i / 4 + j / 3) & 1) ? 230 : 20) << 8) |
                    (uint32_t)((i * j * 7) & 0xFF);
        int rc = gfx_blur_s(t, 2, 2, pw - 4, ph - 4, v[0]);
        print_sheet(pw, ph, 3);
        printk("shape: blur %s\n", rc ? "too large" : "drawn");
        return;
    }
    for (int i = 0; i < pw * ph; i++)
        sheet[i] = 0;
    if (shape == 1)
        gfx_rrect16_s(t, v[0], v[1], v[2], v[3], v[4], 0xFFFFFF, 255);
    else if (shape == 2)
        gfx_rring16_s(t, v[0], v[1], v[2], v[3], v[4], v[5], 0xFFFFFF, 255);
    else
        gfx_line16_s(t, v[0], v[1], v[2], v[3], v[4], 0xFFFFFF, 255);
    print_sheet(pw, ph, 1);
    printk("shape: %dx%d drawn\n", pw, ph);
}
