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
