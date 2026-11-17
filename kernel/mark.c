/* ThreadOS's mark, wherever the system shows it
 *
 * A cog with threads running out of it (art/threados-mark.svg). The
 * kernel reads no SVG: scripts/mklogo.py turns it at build time into
 * coverage, cut to the mark itself and 256 pixels on its longer side
 * (BUILD/gen/logo.h). Here it is drawn at any size, in any colour: each
 * pixel of the target takes the mean of the mask over the area it
 * covers - the mask's pixels on its edges counted by how much of them
 * lies inside, so an edge stays where the SVG has it at 14 pixels as at
 * 96. The loading screen shows it large and white, the menu bar small
 * in the bar's colour, the taskbar's Start and the About box in between.
 */

#include <stdint.h>
#include "include/mark.h"
#include "include/gfx.h"
#include "logo.h"                 /* BUILD/gen, from art/threados-mark.svg */

/* The mask's pixels under the target's pixel i of n across a side of
   len: the first and the last, and how much of each of those two the
   target covers, in 256ths (the ones between, wholly) */
struct span {
    int first, last, w_first, w_last;
};

static struct span span_of(int i, int n, int len)
{
    /* the target pixel in the mask's pixels, in 256ths */
    uint32_t a = (uint32_t)((uint64_t)i * len * 256 / n);
    uint32_t b = (uint32_t)((uint64_t)(i + 1) * len * 256 / n);
    struct span s;
    s.first = (int)(a >> 8);
    s.last = (int)((b - 1) >> 8);
    if (s.first == s.last) {
        s.w_first = s.w_last = (int)(b - a);
    } else {
        s.w_first = 256 - (int)(a & 255);
        s.w_last = (int)(b - ((uint32_t)s.last << 8));
    }
    return s;
}

static int weight(const struct span *s, int k)
{
    return k == s->first ? s->w_first : k == s->last ? s->w_last : 256;
}

void mark_draw(const struct gfx_surface *t, int x, int y, int d, uint32_t c,
               int a)
{
    int k = gfx_k(t);
    int big = LOGO_W > LOGO_H ? LOGO_W : LOGO_H;
    int pd = d * k / 16;
    int pw = pd * LOGO_W / big, ph = pd * LOGO_H / big;
    if (pw < 1 || ph < 1)
        return;
    /* the box in the target's own pixels, the mark in its middle */
    int px0 = x * k / 16 + (pd - pw) / 2, py0 = y * k / 16 + (pd - ph) / 2;
    for (int ty = 0; ty < ph; ty++) {
        struct span sy = span_of(ty, ph, LOGO_H);
        for (int tx = 0; tx < pw; tx++) {
            struct span sx = span_of(tx, pw, LOGO_W);
            uint64_t sum = 0, total = 0;
            for (int my = sy.first; my <= sy.last; my++) {
                int wy = weight(&sy, my);
                for (int mx = sx.first; mx <= sx.last; mx++) {
                    uint32_t w = (uint32_t)(wy * weight(&sx, mx));
                    sum += (uint64_t)w * logo_alpha[my * LOGO_W + mx];
                    total += w;
                }
            }
            int cov = total ? (int)((sum + total / 2) / total) : 0;
            if (cov)
                gfx_pblend_s(t, px0 + tx, py0 + ty, c, cov * a / 255);
        }
    }
}
