/* fctest - fontconfig and FreeType on ThreadOS
 *
 * How a web engine finds a font for a family a page names, and reads
 * it. fontconfig is given one directory and nothing of a system's own
 * configuration, so what it says depends on the fonts alone, and
 * scripts/tests/t_fontconfig.py builds this a second time against the
 * host's fontconfig and FreeType, gives it the same fonts, and holds
 * the two to each other:
 *
 *   the directory scanned (opendir and readdir on ThreadOS): every
 *   font with its family, style, weight, slant, spacing, how many
 *   characters it has and whether it covers Polish and English
 *
 *   patterns matched - a family by name, bold, italic, monospace, a
 *   family that is not there - and the fonts sorted for one
 *
 *   FreeType, of each font: its glyph count, units per em, ascender
 *   and descender, and for a Polish sentence the glyphs and their
 *   advances in the font's own units, unscaled and unhinted
 *
 * `fctest --system [FILE]` asks the configuration a program gets when
 * it asks for none (FONTCONFIG_FILE: FILE, or /etc/fonts/fonts.conf) -
 * on ThreadOS the system's fonts and configuration, which the C
 * library makes up out of the initrd (libc/overlay/.../socket.c) - for
 * its fonts and for the families a page names: sans-serif, serif,
 * monospace, and bold. The test gives the host's fontconfig the same
 * configuration and the same fonts.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H

static const char *base(const FcChar8 *path)
{
    const char *s = (const char *)path, *slash = strrchr(s, '/');
    return slash ? slash + 1 : s;
}

static int by_file(const void *a, const void *b)
{
    FcChar8 *fa = NULL, *fb = NULL;
    FcPatternGetString(*(FcPattern *const *)a, FC_FILE, 0, &fa);
    FcPatternGetString(*(FcPattern *const *)b, FC_FILE, 0, &fb);
    return strcmp(base(fa), base(fb));
}

static void describe(FcPattern *p)
{
    FcChar8 *file = NULL, *family = NULL, *style = NULL;
    int weight = -1, slant = -1, spacing = 0;
    FcCharSet *cs = NULL;
    FcLangSet *ls = NULL;
    FcPatternGetString(p, FC_FILE, 0, &file);
    FcPatternGetString(p, FC_FAMILY, 0, &family);
    FcPatternGetString(p, FC_STYLE, 0, &style);
    FcPatternGetInteger(p, FC_WEIGHT, 0, &weight);
    FcPatternGetInteger(p, FC_SLANT, 0, &slant);
    FcPatternGetInteger(p, FC_SPACING, 0, &spacing);
    FcPatternGetCharSet(p, FC_CHARSET, 0, &cs);
    FcPatternGetLangSet(p, FC_LANG, 0, &ls);
    printf("%s: family '%s' style '%s' weight %d slant %d spacing %d, "
           "%u characters, Polish %s, English %s\n", base(file),
           family ? (char *)family : "?", style ? (char *)style : "?",
           weight, slant, spacing, cs ? FcCharSetCount(cs) : 0,
           ls && FcLangSetHasLang(ls, (const FcChar8 *)"pl") == FcLangEqual
               ? "yes" : "no",
           ls && FcLangSetHasLang(ls, (const FcChar8 *)"en") == FcLangEqual
               ? "yes" : "no");
}

static FcPattern *prepared(FcConfig *cfg, const char *name)
{
    FcPattern *pat = FcNameParse((const FcChar8 *)name);
    FcConfigSubstitute(cfg, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    return pat;
}

static void match(FcConfig *cfg, const char *name)
{
    FcPattern *pat = prepared(cfg, name);
    FcResult r;
    FcPattern *m = FcFontMatch(cfg, pat, &r);
    FcChar8 *file = NULL, *family = NULL, *style = NULL;
    if (m) {
        FcPatternGetString(m, FC_FILE, 0, &file);
        FcPatternGetString(m, FC_FAMILY, 0, &family);
        FcPatternGetString(m, FC_STYLE, 0, &style);
    }
    printf("'%s' is matched by %s ('%s', '%s')\n", name,
           file ? base(file) : "nothing", family ? (char *)family : "-",
           style ? (char *)style : "-");
    if (m)
        FcPatternDestroy(m);
    FcPatternDestroy(pat);
}

static void sorted(FcConfig *cfg, const char *name)
{
    FcPattern *pat = prepared(cfg, name);
    FcResult r;
    FcFontSet *set = FcFontSort(cfg, pat, FcFalse, NULL, &r);
    printf("'%s' sorted:", name);
    for (int i = 0; set && i < set->nfont; i++) {
        FcChar8 *file = NULL;
        FcPatternGetString(set->fonts[i], FC_FILE, 0, &file);
        printf(" %s", file ? base(file) : "?");
    }
    printf("\n");
    if (set)
        FcFontSetDestroy(set);
    FcPatternDestroy(pat);
}

/* The next character of UTF-8 text */
static unsigned next_char(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    unsigned c = *p++;
    int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    c &= more == 3 ? 0x07 : more == 2 ? 0x0F : more == 1 ? 0x1F : 0x7F;
    while (more-- > 0 && (*p & 0xC0) == 0x80)
        c = (c << 6) | (*p++ & 0x3F);
    *s = (const char *)p;
    return c;
}

static void freetype(FT_Library lib, const char *path)
{
    FT_Face face;
    if (FT_New_Face(lib, path, 0, &face)) {
        printf("%s: FreeType cannot open it\n", base((const FcChar8 *)path));
        return;
    }
    printf("%s: %ld glyphs, %u units per em, ascender %d, descender %d\n",
           base((const FcChar8 *)path), face->num_glyphs, face->units_per_EM,
           face->ascender, face->descender);
    const char *text = "Zażółć gęślą jaźń, 0123 ŁÓDŹ!";
    long advance = 0;
    unsigned missing = 0, hash = 0;
    for (const char *s = text; *s; ) {
        unsigned c = next_char(&s);
        FT_UInt g = FT_Get_Char_Index(face, c);
        if (!g)
            missing++;
        if (FT_Load_Glyph(face, g, FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING))
            continue;
        advance += face->glyph->advance.x;
        hash = hash * 31 + g * 7 + (unsigned)face->glyph->outline.n_points;
    }
    printf("  the sentence: advance %ld units, %u characters missing, "
           "glyphs and outlines %08x\n", advance, missing, hash);
    FT_Done_Face(face);
}

static int system_fonts(const char *file)
{
    setenv("FONTCONFIG_FILE", file, 1);
    FcConfig *cfg = FcInitLoadConfigAndFonts();
    if (!cfg) {
        printf("the system's configuration could not be read\n");
        return 1;
    }
    FcPattern *all = FcPatternCreate();
    FcObjectSet *os = FcObjectSetBuild(FC_FILE, FC_FAMILY, FC_STYLE,
                                       FC_WEIGHT, FC_SLANT, FC_SPACING,
                                       FC_CHARSET, FC_LANG, (char *)0);
    FcFontSet *set = FcFontList(cfg, all, os);
    printf("the system's fonts: %d\n", set ? set->nfont : 0);
    if (set) {
        qsort(set->fonts, set->nfont, sizeof(*set->fonts), by_file);
        for (int i = 0; i < set->nfont; i++)
            describe(set->fonts[i]);
        FcFontSetDestroy(set);
    }
    /* (a '-' in fontconfig's names starts the size: "sans-serif" would
       be the family "sans") */
    match(cfg, "sans\\-serif");
    match(cfg, "serif");
    match(cfg, "monospace");
    match(cfg, "sans\\-serif:bold");
    match(cfg, "DejaVu Sans ThreadOS:bold");
    match(cfg, "Helvetica");
    FcObjectSetDestroy(os);
    FcPatternDestroy(all);
    FcConfigDestroy(cfg);
    return 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc > 1 && !strcmp(argv[1], "--system"))
        return system_fonts(argc > 2 ? argv[2] : "/etc/fonts/fonts.conf");
    const char *dir = argc > 1 ? argv[1] : "fonts";
    FcConfig *cfg = FcConfigCreate();
    if (!FcConfigAppFontAddDir(cfg, (const FcChar8 *)dir)) {
        printf("the directory '%s' could not be scanned\n", dir);
        return 1;
    }
    FcPattern *all = FcPatternCreate();
    FcObjectSet *os = FcObjectSetBuild(FC_FILE, FC_FAMILY, FC_STYLE,
                                       FC_WEIGHT, FC_SLANT, FC_SPACING,
                                       FC_CHARSET, FC_LANG, (char *)0);
    FcFontSet *set = FcFontList(cfg, all, os);
    printf("fonts in the directory: %d\n", set ? set->nfont : 0);
    if (set) {
        qsort(set->fonts, set->nfont, sizeof(*set->fonts), by_file);
        for (int i = 0; i < set->nfont; i++)
            describe(set->fonts[i]);
    }
    match(cfg, "DejaVu Sans ThreadOS");
    match(cfg, "DejaVu Sans ThreadOS:bold");
    match(cfg, "DejaVu Sans Mono ThreadOS");
    match(cfg, "Comic Sans MS");
    match(cfg, ":spacing=mono");
    match(cfg, ":weight=200:slant=100");
    sorted(cfg, "DejaVu Sans Mono ThreadOS");
    sorted(cfg, ":weight=bold");

    FT_Library lib;
    FT_Init_FreeType(&lib);
    for (int i = 0; set && i < set->nfont; i++) {
        FcChar8 *file = NULL;
        FcPatternGetString(set->fonts[i], FC_FILE, 0, &file);
        if (file)
            freetype(lib, (const char *)file);
    }
    int major, minor, patch;
    FT_Library_Version(lib, &major, &minor, &patch);
    FT_Done_FreeType(lib);
    printf("~fontconfig %d, FreeType %d.%d.%d\n", FcGetVersion(), major,
           minor, patch);
    if (set)
        FcFontSetDestroy(set);
    FcObjectSetDestroy(os);
    FcPatternDestroy(all);
    FcConfigDestroy(cfg);
    return 0;
}
