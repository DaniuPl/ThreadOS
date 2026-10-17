/* The desktop's lettering in TrueType
 *
 * kernel/ttf.c draws one glyph into a bitmap of coverage values; this
 * file puts glyphs on the screen. Two things happen here that ttf.c
 * leaves alone:
 *
 * Caching. Drawing a glyph from its outline costs a few thousand
 * operations, and a title bar is redrawn every time anything moves.
 * Each glyph is drawn once and kept - the desktop uses a few sizes and
 * a few dozen characters, so the cache fills once and stays full.
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
#include "include/gfx.h"
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
    uint32_t key;                   /* see key_of(); 0 = empty */
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

/* Which font, at which size, which glyph: one number. The glyph is
   counted from one, because glyph 0 - the "missing" glyph - is a glyph
   too and 0 means an empty slot. */
static uint32_t key_of(int font, int px, int g)
{
    return ((uint32_t)font << 24) | ((uint32_t)px << 16) | (uint32_t)(g + 1);
}

/* The glyph's slot, drawn if it was not there. NULL when the glyph
   cannot be drawn at all. Called with the lock held. */
static const struct cached *glyph_of(const struct ttf_font *f, int font,
                                     int px, int g)
{
    uint32_t key = key_of(font, px, g);
    uint32_t h = (key * 2654435761u) % SLOTS;

    for (int probe = 0; probe < SLOTS; probe++) {
        struct cached *c = &slots[(h + (uint32_t)probe) % SLOTS];
        if (c->key == key)
            return c;
        if (c->key)
            continue;

        struct ttf_bitmap b;
        if (ttf_render(f, g, px, &b, scratch, sizeof(scratch)) < 0)
            return (const struct cached *)0;
        uint32_t size = (uint32_t)(b.w * b.h);
        if (pool_used + size > POOL) {
            forget_all();
            return glyph_of(f, font, px, g);
        }
        memcpy(pool + pool_used, scratch, size);
        c->key = key;
        c->w = (int16_t)b.w;
        c->h = (int16_t)b.h;
        c->left = (int16_t)b.left;
        c->top = (int16_t)b.top;
        c->advance = ttf_advance(f, g, px);
        c->at = pool_used;
        pool_used += size;
        return c;
    }
    forget_all();                        /* every slot taken - start over */
    return glyph_of(f, font, px, g);
}

/* The font behind a name. Bold falls back to the plain sans, and that
   to nothing - the 8x8 letters - so a missing file costs looks, never
   text. */
static const struct ttf_font *font_of(int font)
{
    const struct ttf_font *f = (const struct ttf_font *)0;
    if (font == UI_BOLD)
        f = font_bold();
    else if (font == UI_MONO)
        f = font_mono();
    return f ? f : font_sans();
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

/* Capital letters are about 0.73 of the em in these fonts, the bold as
   much as the plain: the baseline goes where they come out centred.
   Worked out from the em rather than measured from a glyph, so it
   cannot change with the text. */
int ui_font_baseline_in(int px, int top, int height)
{
    if (!font_sans())
        return top + (height - 8) / 2 + 8;
    int cap = (px * 73 + 50) / 100;
    return top + (height + cap) / 2;
}

int ui_baseline_in(int top, int height)
{
    return ui_font_baseline_in(UI_PX, top, height);
}

int ui_font_width(int font, int px, const char *s)
{
    const struct ttf_font *f = font_of(font);
    if (!f) {
        int n = 0;
        while (s[n])
            n++;
        return 8 * n;
    }
    int32_t pen = 0;
    while (*s) {
        uint32_t c = next_char(&s);
        pen += ttf_advance(f, ttf_glyph(f, c), px);
    }
    return (pen + 63) >> 6;
}

int ui_text_width(const char *s)
{
    return ui_font_width(UI_SANS, UI_PX, s);
}

/* --- drawing ------------------------------------------------------------- */

/* 'cur' says the compositor's current target (gfx_target()), and then
   't' is not looked at; otherwise the text goes onto 't' */
static int draw_text(int cur, const struct gfx_surface *t, int font, int px,
                     int x, int baseline, const char *s, uint32_t rgb)
{
    const struct ttf_font *f = font_of(font);
    if (!f) {
        int n = 0;
        while (s[n])
            n++;
        if (cur)
            video_draw_string(x, baseline - 8, s, video_nearest_index(rgb));
        return x + 8 * n;
    }

    int32_t pen = (int32_t)x << 6;

    /* With interrupts off, and not only for the lock's sake: the desktop
       thread draws text with them on, and a timer landing on it while it
       held this lock would wait for the big lock - which a program on
       the other core may hold, drawing text into its window, spinning
       right here for the lock the interrupted thread cannot let go of */
    irqflags_t fl = spin_lock_irq(&lock);
    while (*s) {
        uint32_t c = next_char(&s);
        const struct cached *g = glyph_of(f, font, px, ttf_glyph(f, c));
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
                if (cur)
                    gfx_blend(gx + k, gy + r, rgb, v);
                else
                    gfx_blend_s(t, gx + k, gy + r, rgb, v);
            }
        pen += g->advance;
    }
    spin_unlock_irq(&lock, fl);
    return (pen + 63) >> 6;
}

int ui_font_text(int font, int px, int x, int baseline, const char *s,
                 uint32_t rgb)
{
    return draw_text(1, (const struct gfx_surface *)0, font, px, x, baseline,
                     s, rgb);
}

int ui_font_text_s(const struct gfx_surface *t, int font, int px, int x,
                   int baseline, const char *s, uint32_t rgb)
{
    return draw_text(0, t, font, px, x, baseline, s, rgb);
}

int ui_text(int x, int baseline, const char *s, uint8_t colour)
{
    return ui_font_text(UI_SANS, UI_PX, x, baseline, s,
                        video_palette_rgb(colour));
}
