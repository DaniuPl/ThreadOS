/* The aurora: the background a desktop has when nobody chose a picture
 *
 * A slope from the top of the screen to the bottom, and over it a few
 * large, soft clouds of colour, each fading from its middle to nothing
 * at its edge. It is drawn once, when the desktop starts or the theme
 * changes, and from then on copied - nothing in it moves.
 *
 * The numbers - the slope's two ends, where the clouds are, how large,
 * what colour and how strong - are in kernel/include/look.h, a set for
 * each theme. The arithmetic is whole numbers only and is spelled out
 * here, because the test draws the same picture from the same numbers
 * and expects the very same pixels:
 *
 *   slope:  c = top + (bottom - top) * y / (h - 1), per channel, the
 *           division cutting towards zero, as C's does
 *   cloud:  in doubled coordinates (so a pixel's centre is 2x + 1),
 *           d2 = distance squared from the cloud's centre, R2 = its
 *           doubled radius squared; inside it,
 *             t = (R2 - d2) * 256 / R2         0 at the edge, 256 in the middle
 *             w = t * t >> 8                   the square fades softly
 *             a = w * strength >> 8
 *             c = (c * (256 - a) + colour * a) >> 8, per channel
 *           and the clouds are laid on in the order they are listed.
 */

#include <stdint.h>
#include "include/aurora.h"
#include "include/look.h"
#include "include/gfx.h"
#include "include/heap.h"
#include "include/string.h"

struct cloud {
    int x, y, r;          /* thousandths of the width, the height, the width */
    uint32_t colour;
    int strength;
};

static const struct cloud dark_clouds[AURORA_N] = { AURORA_DARK_CLOUDS };
static const struct cloud light_clouds[AURORA_N] = { AURORA_LIGHT_CLOUDS };

void aurora_draw(uint32_t *buf, int w, int h, int stride, int dark)
{
    uint32_t top = dark ? AURORA_DARK_TOP : AURORA_LIGHT_TOP;
    uint32_t bottom = dark ? AURORA_DARK_BOTTOM : AURORA_LIGHT_BOTTOM;
    const struct cloud *cl = dark ? dark_clouds : light_clouds;
    int cx[AURORA_N], cy[AURORA_N];
    uint32_t rr[AURORA_N];

    for (int k = 0; k < AURORA_N; k++) {
        cx[k] = 2 * (cl[k].x * w / 1000);
        cy[k] = 2 * (cl[k].y * h / 1000);
        uint32_t r2 = (uint32_t)(2 * (cl[k].r * w / 1000));
        rr[k] = r2 * r2;
    }

    for (int y = 0; y < h; y++) {
        int c[3];
        for (int ch = 0; ch < 3; ch++) {
            int t = (int)((top >> (16 - 8 * ch)) & 0xFF);
            int b = (int)((bottom >> (16 - 8 * ch)) & 0xFF);
            c[ch] = h > 1 ? t + (b - t) * y / (h - 1) : t;
        }
        int Y = 2 * y + 1;
        uint32_t *row = buf + (long)y * stride;
        for (int x = 0; x < w; x++) {
            int p[3] = { c[0], c[1], c[2] };
            int X = 2 * x + 1;
            for (int k = 0; k < AURORA_N; k++) {
                int dx = X - cx[k], dy = Y - cy[k];
                uint32_t d2 = (uint32_t)(dx * dx) + (uint32_t)(dy * dy);
                if (d2 >= rr[k])
                    continue;
                uint32_t t = (rr[k] - d2) * 256 / rr[k];
                uint32_t wgt = (t * t) >> 8;
                int a = (int)((wgt * (uint32_t)cl[k].strength) >> 8);
                for (int ch = 0; ch < 3; ch++) {
                    int col = (int)((cl[k].colour >> (16 - 8 * ch)) & 0xFF);
                    p[ch] = (p[ch] * (256 - a) + col * a) >> 8;
                }
            }
            row[x] = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) |
                     (uint32_t)p[2];
        }
    }
}

/* --- the warp: Weave's background --------------------------------------
 *
 * The ground, the faint 6x6 weave mixed in whole numbers as the grain
 * is (kernel/gfx.c), and the threads: each pass of a thread - its glow,
 * then its core - is first gathered as the most any of its straight
 * pieces covers each pixel (gfx_cov_line16), and only then laid on, so
 * that where two pieces meet the thread is no darker than along them. */

struct warp_thread {
    int dye, glow_w, glow_a, core_w, core_a;
    int p[12];
};
static const struct warp_thread warp_threads[WARP_N] = WARP_THREADS;

static uint32_t mix(uint32_t o, uint32_t c, uint32_t a)
{
    uint32_t r = (((o >> 16) & 0xFF) * (255 - a) + ((c >> 16) & 0xFF) * a +
                  127) / 255;
    uint32_t g = (((o >> 8) & 0xFF) * (255 - a) + ((c >> 8) & 0xFF) * a +
                  127) / 255;
    uint32_t b = ((o & 0xFF) * (255 - a) + (c & 0xFF) * a + 127) / 255;
    return r << 16 | g << 8 | b;
}

/* One curve's straight pieces into the mask: P0..P3 in sixteenths */
static void warp_curve(uint8_t *mask, int w, int h, const long *x,
                       const long *y, int thick, int *bx0, int *by0,
                       int *bx1, int *by1)
{
    long len = 0;
    for (int i = 0; i < 3; i++) {
        long dx = x[i + 1] - x[i], dy = y[i + 1] - y[i];
        len += (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
    }
    long n = len / (8 * 16);
    if (n < 4)
        n = 4;
    if (n > 256)
        n = 256;
    long lx = x[0], ly = y[0];
    int m = thick / 32 + 2;               /* the reach, in pixels */
    for (long i = 1; i <= n; i++) {
        long u = n - i, v = i, n3 = n * n * n;
        long cx = (u * u * u * x[0] + 3 * u * u * v * x[1] +
                   3 * u * v * v * x[2] + v * v * v * x[3]) / n3;
        long cy = (u * u * u * y[0] + 3 * u * u * v * y[1] +
                   3 * u * v * v * y[2] + v * v * v * y[3]) / n3;
        int px0 = (int)((lx < cx ? lx : cx) / 16) - m;
        int px1 = (int)((lx > cx ? lx : cx) / 16) + m;
        int py0 = (int)((ly < cy ? ly : cy) / 16) - m;
        int py1 = (int)((ly > cy ? ly : cy) / 16) + m;
        if (px0 < 0) px0 = 0;
        if (py0 < 0) py0 = 0;
        if (px1 >= w) px1 = w - 1;
        if (py1 >= h) py1 = h - 1;
        for (int py = py0; py <= py1; py++)
            for (int px = px0; px <= px1; px++) {
                int cv = gfx_cov_line16(px, py, (int)lx, (int)ly, (int)cx,
                                        (int)cy, thick);
                uint8_t *at = mask + (long)py * w + px;
                if (cv > *at)
                    *at = (uint8_t)(cv > 255 ? 255 : cv);
            }
        if (px0 <= px1 && py0 <= py1) {
            if (px0 < *bx0) *bx0 = px0;
            if (py0 < *by0) *by0 = py0;
            if (px1 > *bx1) *bx1 = px1;
            if (py1 > *by1) *by1 = py1;
        }
        lx = cx;
        ly = cy;
    }
}

void warp_draw(uint32_t *buf, int w, int h, int stride, int dark,
               uint32_t thread)
{
    static const uint32_t night[6] = { DYE_INDIGO_N, DYE_MADDER_N,
                                       DYE_SAFFRON_N, DYE_WELD_N, DYE_WOAD_N,
                                       DYE_COCHINEAL_N };
    static const uint32_t linen[6] = { DYE_INDIGO_L, DYE_MADDER_L,
                                       DYE_SAFFRON_L, DYE_WELD_L, DYE_WOAD_L,
                                       DYE_COCHINEAL_L };
    uint32_t ground = dark ? NIGHT_GROUND : LINEN_GROUND;
    uint32_t ink = dark ? 0xFFFFFF : 0x000000;
    for (int y = 0; y < h; y++) {
        uint32_t *row = buf + (long)y * stride;
        int weft = y % 6 == 3;
        for (int x = 0; x < w; x++) {
            uint32_t a = weft ? WARP_ROW_INK : x % 6 == 0 ? WARP_COL_INK : 0;
            row[x] = a ? mix(ground, ink, a) : ground;
        }
    }
    uint8_t *mask = kmalloc((uint32_t)(w * h));
    if (!mask)
        return;                           /* the weave alone, then */
    memset(mask, 0, (uint32_t)(w * h));
    for (int t = 0; t < WARP_N; t++) {
        const struct warp_thread *th = &warp_threads[t];
        uint32_t colour = th->dye == 6 ? thread
                        : (dark ? night : linen)[th->dye % 6];
        long x[7], y[7];
        for (int i = 0; i < 6; i++) {
            x[i] = (long)th->p[2 * i] * w * 16 / 1000 + 8;
            y[i] = (long)th->p[2 * i + 1] * h * 16 / 1000 + 8;
        }
        /* the second curve: P1, C2 mirrored through P1, C2', P2 */
        long x2[4] = { x[3], 2 * x[3] - x[2], x[4], x[5] };
        long y2[4] = { y[3], 2 * y[3] - y[2], y[4], y[5] };
        for (int pass = 0; pass < 2; pass++) {
            int thick = pass ? th->core_w : th->glow_w;
            int alpha = pass ? th->core_a : th->glow_a;
            if (!thick || !alpha)
                continue;
            int bx0 = w, by0 = h, bx1 = -1, by1 = -1;
            warp_curve(mask, w, h, x, y, thick, &bx0, &by0, &bx1, &by1);
            warp_curve(mask, w, h, x2, y2, thick, &bx0, &by0, &bx1, &by1);
            for (int py = by0; py <= by1; py++) {
                uint32_t *row = buf + (long)py * stride;
                uint8_t *m = mask + (long)py * w;
                for (int px = bx0; px <= bx1; px++) {
                    if (!m[px])
                        continue;
                    row[px] = mix(row[px], colour,
                                  (uint32_t)m[px] * (uint32_t)alpha / 255);
                    m[px] = 0;
                }
            }
        }
    }
    kfree(mask);
}
