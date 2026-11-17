/* FreeType against corrupted fonts
 *
 * A web page brings its own fonts, so a font is untrusted input for the
 * engine, and FreeType reads it. The release archive has no fuzzers of
 * its own (they live in a repository apart), so this one does what a
 * page's font goes through: every face in the file opened, a size set,
 * glyphs loaded - outlines and bitmaps, hinted and not - and rendered,
 * kerning asked for, names and tables read.
 */

#include <stddef.h>
#include <stdint.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_SFNT_NAMES_H
#include FT_TRUETYPE_TABLES_H
#include FT_GLYPH_H

static void one_face(FT_Library lib, const uint8_t *data, size_t size,
                     long index)
{
    FT_Face face;
    if (FT_New_Memory_Face(lib, data, (FT_Long)size, index, &face))
        return;
    FT_Set_Char_Size(face, 0, 16 * 64, 96, 96);
    FT_UInt count = FT_Get_Sfnt_Name_Count(face);
    for (FT_UInt i = 0; i < count && i < 32; i++) {
        FT_SfntName name;
        FT_Get_Sfnt_Name(face, i, &name);
    }
    (void)FT_Get_Sfnt_Table(face, FT_SFNT_OS2);
    (void)FT_Get_Sfnt_Table(face, FT_SFNT_HHEA);
    static const FT_Int32 modes[] = {
        FT_LOAD_DEFAULT, FT_LOAD_NO_HINTING, FT_LOAD_FORCE_AUTOHINT,
        FT_LOAD_NO_SCALE, FT_LOAD_COLOR,
    };
    long glyphs = face->num_glyphs < 64 ? face->num_glyphs : 64;
    for (long g = 0; g < glyphs; g++) {
        FT_Int32 mode = modes[g % 5];
        if (FT_Load_Glyph(face, (FT_UInt)g, mode))
            continue;
        if (!(mode & FT_LOAD_NO_SCALE))
            FT_Render_Glyph(face->glyph, g & 1 ? FT_RENDER_MODE_NORMAL
                                               : FT_RENDER_MODE_MONO);
        FT_Glyph copy;
        if (!FT_Get_Glyph(face->glyph, &copy)) {
            FT_BBox box;
            FT_Glyph_Get_CBox(copy, FT_GLYPH_BBOX_PIXELS, &box);
            FT_Done_Glyph(copy);
        }
    }
    FT_ULong code;
    FT_UInt gi;
    int n = 0;
    for (code = FT_Get_First_Char(face, &gi); gi && n < 256;
         code = FT_Get_Next_Char(face, code, &gi), n++) {
        FT_Vector k;
        FT_Get_Kerning(face, gi, (gi + 1) % (FT_UInt)(face->num_glyphs + 1),
                       FT_KERNING_DEFAULT, &k);
    }
    FT_Done_Face(face);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    FT_Library lib;
    if (FT_Init_FreeType(&lib))
        return 0;
    FT_Face probe;
    long faces = 1;
    if (!FT_New_Memory_Face(lib, data, (FT_Long)size, -1, &probe)) {
        faces = probe->num_faces;
        FT_Done_Face(probe);
    }
    for (long i = 0; i < faces && i < 4; i++)
        one_face(lib, data, size, i);
    FT_Done_FreeType(lib);
    return 0;
}
