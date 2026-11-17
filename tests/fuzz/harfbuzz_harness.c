/* HarfBuzz under the sanitizers, the way a web engine uses it
 *
 * A page's fonts are its own, and whatever a server sends as one goes
 * to HarfBuzz. third_party/harfbuzz keeps only src/ of the release, so
 * HarfBuzz's own fuzzers are not there; this does what shaping a page
 * does with a font, for libFuzzer's entry point (tests/fuzz/onefile.c
 * drives it): the face and its tables, text in several scripts shaped
 * with the default features and with some turned on and off, left to
 * right and right to left, and each glyph's extents and outline.
 */

#include <stdint.h>
#include <string.h>
#include "hb.h"
#include "hb-ot.h"

/* Latin with kerning pairs and ligatures, Arabic, Hebrew with a point,
   Devanagari with a conjunct, Thai, and marks stacked on one letter -
   as UTF-8 bytes */
static const char *const texts[] = {
    "AVATAR To, office ffi 0123 \"quoted\"",
    "\xd8\xa7\xd9\x84\xd8\xb9\xd8\xb1\xd8\xa8\xd9\x8a\xd8\xa9",
    "\xd7\xa9\xd6\xb8\xd7\x9c\xd7\x95\xd7\x9d",
    "\xe0\xa4\x95\xe0\xa5\x8d\xe0\xa4\xb7\xe0\xa4\xbf",
    "\xe0\xb8\xaa\xe0\xb8\xa7\xe0\xb8\xb1\xe0\xb8\xaa\xe0\xb8\x94\xe0\xb8\xb5",
    "e\xcc\x81\xcc\x82\xcc\xa3 a\xcc\x8a",
};

static const char *const feature_sets[] = {
    "", "-kern,-liga", "+smcp,+onum,+ss01,+frac", "+dlig,+salt=2,-calt",
};

static void move_to(hb_draw_funcs_t *f, void *d, hb_draw_state_t *s,
                    float x, float y, void *u)
{
    (void)f; (void)s; (void)x; (void)y; (void)u;
    (*(unsigned *)d)++;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    hb_blob_t *blob = hb_blob_create((const char *)data, (unsigned)size,
                                     HB_MEMORY_MODE_READONLY, NULL, NULL);
    hb_face_t *face = hb_face_create(blob, 0);
    hb_font_t *font = hb_font_create(face);
    hb_font_set_scale(font, 1200, 1200);

    for (unsigned t = 0; t < sizeof texts / sizeof texts[0]; t++) {
        for (unsigned s = 0; s < sizeof feature_sets / sizeof feature_sets[0];
             s++) {
            hb_feature_t feats[8];
            unsigned nf = 0;
            char list[64];
            strcpy(list, feature_sets[s]);
            for (char *p = strtok(list, ","); p && nf < 8; p = strtok(NULL, ","))
                nf += hb_feature_from_string(p, -1, &feats[nf]);
            hb_buffer_t *buf = hb_buffer_create();
            hb_buffer_add_utf8(buf, texts[t], -1, 0, -1);
            hb_buffer_guess_segment_properties(buf);
            if (s == 3)                  /* the other way round, on purpose */
                hb_buffer_set_direction(buf, HB_DIRECTION_IS_FORWARD(
                    hb_buffer_get_direction(buf)) ? HB_DIRECTION_RTL
                                                  : HB_DIRECTION_LTR);
            hb_shape(font, buf, feats, nf);
            hb_buffer_destroy(buf);
        }
    }

    hb_draw_funcs_t *draw = hb_draw_funcs_create();
    hb_draw_funcs_set_move_to_func(draw, move_to, NULL, NULL);
    unsigned glyphs = hb_face_get_glyph_count(face), moves = 0;
    for (unsigned g = 0; g < glyphs && g < 300; g++) {
        hb_glyph_extents_t ext;
        hb_font_get_glyph_extents(font, g, &ext);
        hb_font_draw_glyph(font, g, draw, &moves);
    }
    hb_draw_funcs_destroy(draw);

    hb_font_destroy(font);
    hb_face_destroy(face);
    hb_blob_destroy(blob);
    return 0;
}
