/* The desktop's lettering in TrueType
 *
 * kernel/ttf.c draws one glyph into a bitmap of coverage values; this
 * file puts glyphs on the screen. Two things happen here that ttf.c
 * leaves alone:
 *
 * Caching. Drawing a glyph from its outline costs a few thousand
 * operations, and a title bar is redrawn every time anything moves.
 * Each glyph is drawn once and kept - the desktop uses one size and a
 * few dozen characters, so the cache fills once and stays full.
 *
 * Mixing. Coverage is not a colour: a pixel a glyph covers by a third
 * gets a third of the text's colour and two thirds of what was already
 * there. The formula is spelled out once below, because the tests'
 * screen reader does the same arithmetic to know what to expect:
 *
 *     out = (text * a + under * (255 - a) + 127) / 255, per channel
 *
 * In a 256-colour mode the result goes through the palette's nearest
 * colour, which is coarser but still smooth at the edges.
 */

#include <stdint.h>
#include "include/uitext.h"
#include "include/fonts.h"
#include "include/ttf.h"
#include "include/video.h"
#include "include/string.h"
#include "include/spinlock.h"

/* --- the cache ----------------------------------------------------------
 *
 * Open addressing on the glyph number. The bitmaps live in one pool;
 * if it ever fills (it would take a few hundred different characters),
 * everything is forgotten and drawn again - simpler than evicting, and
 * a desktop never gets there. */
#define SLOTS 512
#define POOL  (96 * 1024)

struct cached {
    int key;                        /* the glyph number + 1; 0 = empty */
    int16_t w, h, left, top;
    int32_t advance;                /* 26.6 */
    uint32_t at;                    /* offset in the pool */
};

static struct cached slots[SLOTS];
static uint8_t pool[POOL];
static uint32_t pool_used;
static struct spinlock lock = SPINLOCK_FREE;
static uint8_t scratch[192 * 192];

static void forget_all(void)
{
    memset(slots, 0, sizeof(slots));
    pool_used = 0;
}

/* The glyph's slot, drawn if it was not there. NULL when the glyph
   cannot be drawn at all. Called with the lock held. */
static const struct cached *glyph_of(const struct ttf_font *f, int g)
{
    uint32_t h = ((uint32_t)g * 2654435761u) % SLOTS;

    for (int probe = 0; probe < SLOTS; probe++) {
        struct cached *c = &slots[(h + (uint32_t)probe) % SLOTS];
        /* +1, because glyph 0 - the "missing" glyph - is a glyph too */
        if (c->key == g + 1)
            return c;
        if (c->key)
            continue;

        struct ttf_bitmap b;
        if (ttf_render(f, g, UI_PX, &b, scratch, sizeof(scratch)) < 0)
            return (const struct cached *)0;
        uint32_t size = (uint32_t)(b.w * b.h);
        if (pool_used + size > POOL) {
            forget_all();
            return glyph_of(f, g);
        }
        memcpy(pool + pool_used, scratch, size);
        c->key = g + 1;
        c->w = (int16_t)b.w;
        c->h = (int16_t)b.h;
        c->left = (int16_t)b.left;
        c->top = (int16_t)b.top;
        c->advance = ttf_advance(f, g, UI_PX);
        c->at = pool_used;
        pool_used += size;
        return c;
    }
    forget_all();                        /* every slot taken - start over */
    return glyph_of(f, g);
}

/* --- UTF-8 ----------------------------------------------------------- */

/* The next character, and the pointer moved past it. A broken
   sequence gives U+FFFD and moves one byte on, so one bad byte costs
   one character, not the rest of the string. */
static uint32_t next_char(const char **p)
{
    const uint8_t *s = (const uint8_t *)*p;
    uint32_t c = s[0];
    int more = 0;

    if (c < 0x80) {
        *p += 1;
        return c;
    }
    if ((c & 0xE0) == 0xC0) {
        c &= 0x1F;
        more = 1;
    } else if ((c & 0xF0) == 0xE0) {
        c &= 0x0F;
        more = 2;
    } else if ((c & 0xF8) == 0xF0) {
        c &= 0x07;
        more = 3;
    } else {
        *p += 1;
        return 0xFFFD;
    }
    for (int i = 1; i <= more; i++) {
        if ((s[i] & 0xC0) != 0x80) {
            *p += 1;
            return 0xFFFD;
        }
        c = (c << 6) | (s[i] & 0x3F);
    }
    *p += 1 + more;
    return c;
}

/* --- metrics --------------------------------------------------------- */

int ui_ascent(void)
{
    const struct ttf_font *f = font_sans();
    if (!f)
        return 8;
    int32_t a;
    ttf_vmetrics(f, UI_PX, &a, (int32_t *)0, (int32_t *)0);
    return (a + 63) >> 6;
}

int ui_descent(void)
{
    const struct ttf_font *f = font_sans();
    if (!f)
        return 0;
    int32_t d;
    ttf_vmetrics(f, UI_PX, (int32_t *)0, &d, (int32_t *)0);
    return (-d + 63) >> 6;
}

/* Capital letters are about 0.73 of the em in this font: the baseline
   goes where they come out centred. Worked out from the em rather
   than measured from a glyph, so it cannot change with the text. */
int ui_baseline_in(int top, int height)
{
    if (!font_sans())
        return top + (height - 8) / 2 + 8;
    int cap = (UI_PX * 73 + 50) / 100;
    return top + (height + cap) / 2;
}

int ui_text_width(const char *s)
{
    const struct ttf_font *f = font_sans();
    if (!f) {
        int n = 0;
        while (s[n])
            n++;
        return 8 * n;
    }
    int32_t pen = 0;
    while (*s) {
        uint32_t c = next_char(&s);
        pen += ttf_advance(f, ttf_glyph(f, c), UI_PX);
    }
    return (pen + 63) >> 6;
}

/* --- drawing ------------------------------------------------------------- */

static uint32_t mix(uint32_t text, uint32_t under, uint32_t a)
{
    uint32_t out = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        uint32_t t = (text >> sh) & 0xFF, u = (under >> sh) & 0xFF;
        out |= ((t * a + u * (255 - a) + 127) / 255) << sh;
    }
    return out;
}

int ui_text(int x, int baseline, const char *s, uint8_t colour)
{
    const struct ttf_font *f = font_sans();
    if (!f) {
        video_draw_string(x, baseline - 8, s, colour);
        int n = 0;
        while (s[n])
            n++;
        return x + 8 * n;
    }

    uint32_t rgb = video_palette_rgb(colour);
    int32_t pen = (int32_t)x << 6;

    spin_lock(&lock);
    while (*s) {
        uint32_t c = next_char(&s);
        const struct cached *g = glyph_of(f, ttf_glyph(f, c));
        if (!g)
            continue;
        /* The pen's whole pixel, rounded - glyphs are not placed at
           fractions of a pixel, the distances between them are */
        int gx = ((pen + 32) >> 6) + g->left;
        int gy = baseline - g->top;
        const uint8_t *a = pool + g->at;
        for (int r = 0; r < g->h; r++)
            for (int k = 0; k < g->w; k++) {
                uint8_t v = a[r * g->w + k];
                if (!v)
                    continue;
                int px = gx + k, py = gy + r;
                if (v == 255)
                    video_putpixel_rgb(px, py, rgb);
                else
                    video_putpixel_rgb(px, py,
                                       mix(rgb, video_getpixel_rgb(px, py), v));
            }
        pen += g->advance;
    }
    spin_unlock(&lock);
    return (pen + 63) >> 6;
}
