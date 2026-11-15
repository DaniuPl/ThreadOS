/* fonttest - FreeType and HarfBuzz on ThreadOS
 *
 * Plain C against both libraries. The test builds the same program with
 * the same FreeType and HarfBuzz on the host and compares the outputs
 * line for line: which glyphs HarfBuzz picks for a text and where it
 * puts them (kerning, ligatures, marks), and the pixels FreeType draws
 * for each, hinted and not.
 *
 *   fonttest FONT...
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_FONT_FORMATS_H
#include <hb.h>
#include <hb-ft.h>

static unsigned long fnv(const unsigned char *p, size_t n, unsigned long h)
{
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 16777619ul & 0xFFFFFFFFul;
    return h;
}

static const char *texts[] = {
    "Hello, ThreadOS",
    "AVATAR Wave To",                    /* kerning pairs */
    "office fluff ffi",                  /* ligatures, where the font has them */
    "Za\xc5\xbc\xc3\xb3\xc5\x82\xc4\x87 g\xc4\x99\xc5\x9bl\xc4\x85 ja\xc5\xba\xc5\x84", /* Polish */
    "e\xcc\x81 a\xcc\x8a n\xcc\x83",     /* base letters and combining marks */
};

int main(int argc, char **argv)
{
    FT_Library lib;
    if (FT_Init_FreeType(&lib)) {
        printf("FT_Init_FreeType failed\n");
        return 1;
    }
    FT_Int ma, mi, pa;
    FT_Library_Version(lib, &ma, &mi, &pa);
    printf("FreeType %d.%d.%d, HarfBuzz %s\n", ma, mi, pa, hb_version_string());

    for (int f = 1; f < argc; f++) {
        FT_Face face;
        int e = FT_New_Face(lib, argv[f], 0, &face);
        if (e) {
            printf("%s: FT_New_Face error %d\n", argv[f], e);
            continue;
        }
        printf("%s: %s %s, %ld glyphs, %s outlines\n", argv[f],
               face->family_name, face->style_name, face->num_glyphs,
               FT_IS_SFNT(face) && (face->face_flags & FT_FACE_FLAG_SCALABLE)
                   ? (strstr(FT_Get_Font_Format(face), "CFF") ? "CFF" : "TrueType")
                   : "other");
        FT_Set_Pixel_Sizes(face, 0, 20);
        hb_font_t *font = hb_ft_font_create_referenced(face);
        for (size_t t = 0; t < sizeof(texts) / sizeof(texts[0]); t++) {
            hb_buffer_t *buf = hb_buffer_create();
            hb_buffer_add_utf8(buf, texts[t], -1, 0, -1);
            hb_buffer_guess_segment_properties(buf);
            hb_shape(font, buf, NULL, 0);
            unsigned n;
            hb_glyph_info_t *info = hb_buffer_get_glyph_infos(buf, &n);
            hb_glyph_position_t *pos = hb_buffer_get_glyph_positions(buf, &n);
            long pen = 0;
            unsigned long plain = 2166136261ul, hinted = 2166136261ul;
            printf("  [%zu] %u glyphs:", t, n);
            for (unsigned i = 0; i < n; i++) {
                printf(" %u@%u%+d", info[i].codepoint, info[i].cluster,
                       pos[i].x_advance);
                if (pos[i].x_offset || pos[i].y_offset)
                    printf("(%+d,%+d)", pos[i].x_offset, pos[i].y_offset);
                pen += pos[i].x_advance;
                for (int h = 0; h < 2; h++) {
                    if (FT_Load_Glyph(face, info[i].codepoint,
                                      h ? FT_LOAD_TARGET_LIGHT : FT_LOAD_NO_HINTING) ||
                        FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL))
                        continue;
                    FT_Bitmap *b = &face->glyph->bitmap;
                    unsigned long *s = h ? &hinted : &plain;
                    unsigned char dims[8] = { (unsigned char)b->width,
                                              (unsigned char)b->rows,
                                              (unsigned char)face->glyph->bitmap_left,
                                              (unsigned char)face->glyph->bitmap_top };
                    *s = fnv(dims, 4, *s);
                    for (unsigned r = 0; r < b->rows; r++)
                        *s = fnv(b->buffer + (long)r * b->pitch, b->width, *s);
                }
            }
            printf("\n      width %ld/64 px, pixels %08lx unhinted, %08lx hinted\n",
                   pen, plain, hinted);
            hb_buffer_destroy(buf);
        }
        hb_font_destroy(font);
    }
    FT_Done_FreeType(lib);
    printf("done\n");
    return 0;
}
