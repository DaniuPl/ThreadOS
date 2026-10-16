#ifndef THREADOS_TTF_H
#define THREADOS_TTF_H

#include <stdint.h>

/* TrueType fonts - reading them and drawing their glyphs
 *
 * A font is a file of tables; this reads the ones that say what a glyph
 * looks like (glyf, loca), which glyph a character is (cmap) and how
 * far the pen moves after it (hmtx, hhea). Glyphs are drawn with
 * anti-aliasing - every pixel gets the fraction of it the outline
 * covers - and without hinting: the outline is drawn as designed, not
 * nudged to the pixel grid, which is what FreeType does with hinting
 * off and is what the test compares against.
 *
 * Everything is integer arithmetic. The 64-bit kernel is built without
 * SSE and the 32-bit one without libgcc, so neither floating point nor
 * a 64-bit division is to be had here.
 */

struct ttf_font {
    const uint8_t *data;
    uint32_t len;
    uint32_t glyf, glyf_len;
    uint32_t loca;
    uint32_t hmtx, hmtx_len;
    uint32_t cmap_sub;           /* the subtable used, 0 = none */
    int cmap_format;             /* 4 or 12 */
    int long_loca;
    int glyphs;
    int units_per_em;
    int ascender, descender, line_gap;
    int hmetrics;
};

/* Reads a font out of memory (it stays there - nothing is copied).
   0, or -1 with the reason in 'why' when it is not a font this reads. */
int ttf_open(struct ttf_font *f, const void *data, uint32_t len,
             const char **why);

/* The glyph for a character; 0 is the "missing" glyph every font has */
int ttf_glyph(const struct ttf_font *f, uint32_t codepoint);

/* How far the pen moves after a glyph at a size, in 1/64 of a pixel -
   unrounded, as the outline is drawn */
int32_t ttf_advance(const struct ttf_font *f, int glyph, int px);

/* The font's ascender and descender at a size, in 1/64 of a pixel
   (the descender is negative) */
void ttf_vmetrics(const struct ttf_font *f, int px, int32_t *ascent,
                  int32_t *descent, int32_t *gap);

/* A drawn glyph: w x h coverage values, 0 (nothing) .. 255 (all of the
   pixel), row after row. 'left' is how many pixels right of the pen
   the first column is, 'top' how many pixels above the baseline the
   first row is. */
struct ttf_bitmap {
    int w, h;
    int left, top;
    uint8_t *alpha;
};

/* Draws a glyph into 'buf' (cap bytes). 0, or -1 when the glyph is
   damaged or larger than the rasteriser takes. An empty glyph (a
   space) is 0 with w = h = 0. */
int ttf_render(const struct ttf_font *f, int glyph, int px,
               struct ttf_bitmap *out, uint8_t *buf, uint32_t cap);

#endif
