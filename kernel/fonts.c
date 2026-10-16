/* The fonts in the initrd, and the 'font' command
 *
 * Two files, cut out of DejaVu by scripts/mkfonts.py: sans.ttf for the
 * desktop and mono.ttf for anything that wants columns. They are read
 * where they lie in the initrd - kernel/ttf.c copies nothing.
 *
 * 'font' on its own says what was found. 'font sheet NAME PX FIRST
 * LAST' draws every character in the range and prints each glyph as
 * hex, one row of coverage per line. Nobody reads that with their eyes:
 * the test does, and compares it pixel by pixel with what FreeType
 * draws from the same file.
 */

#include <stdint.h>
#include "include/fonts.h"
#include "include/initrd.h"
#include "include/printk.h"

static struct ttf_font sans, mono;
static int have_sans, have_mono;
static const char *why_sans = "not looked for", *why_mono = "not looked for";

static int open_one(struct ttf_font *f, const char *name, const char **why)
{
    uint32_t size = 0;
    const void *data = initrd_find(name, &size);
    if (!data) {
        *why = "not in the initrd";
        return 0;
    }
    return ttf_open(f, data, size, why) == 0;
}

void fonts_init(void)
{
    have_sans = open_one(&sans, "sans.ttf", &why_sans);
    have_mono = open_one(&mono, "mono.ttf", &why_mono);
}

const struct ttf_font *font_sans(void)
{
    return have_sans ? &sans : (const struct ttf_font *)0;
}

const struct ttf_font *font_mono(void)
{
    return have_mono ? &mono : (const struct ttf_font *)0;
}

static uint32_t number(const char **p)
{
    uint32_t v = 0;
    while (**p == ' ')
        (*p)++;
    if ((*p)[0] == '0' && ((*p)[1] == 'x' || (*p)[1] == 'X')) {
        *p += 2;
        for (;;) {
            char c = **p;
            int d = c >= '0' && c <= '9' ? c - '0' :
                    c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                    c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (d < 0)
                break;
            v = v * 16 + (uint32_t)d;
            (*p)++;
        }
        return v;
    }
    while (**p >= '0' && **p <= '9')
        v = v * 10 + (uint32_t)(*(*p)++ - '0');
    return v;
}

static void describe(const char *name, const struct ttf_font *f,
                     int have, const char *why)
{
    if (!have) {
        printk("font: %s - none (%s)\n", name, why);
        return;
    }
    int32_t asc, desc, gap;
    ttf_vmetrics(f, 16, &asc, &desc, &gap);
    printk("font: %s - %d glyphs, %d units per em, cmap format %d, "
           "at 16 px ascent %d/64 descent %d/64\n", name, f->glyphs,
           f->units_per_em, f->cmap_format, asc, desc);
}

static uint8_t sheet_buf[192 * 192];

static void sheet(const struct ttf_font *f, int px, uint32_t first,
                  uint32_t last)
{
    static const char hex[] = "0123456789abcdef";
    char row[2 * 192 + 1];
    int drawn = 0;

    for (uint32_t c = first; c <= last; c++) {
        int g = ttf_glyph(f, c);
        if (!g)
            continue;
        struct ttf_bitmap b;
        int r = ttf_render(f, g, px, &b, sheet_buf, sizeof(sheet_buf));
        printk("U+%x g=%d adv=%d r=%d box=%d,%d,%d,%d\n", c, g,
               ttf_advance(f, g, px), r, b.left, b.top, b.w, b.h);
        for (int y = 0; y < b.h; y++) {
            for (int x = 0; x < b.w; x++) {
                uint8_t v = b.alpha[y * b.w + x];
                row[2 * x] = hex[v >> 4];
                row[2 * x + 1] = hex[v & 15];
            }
            row[2 * b.w] = '\0';
            printk("%s\n", row);
        }
        drawn++;
    }
    printk("font sheet: %d glyphs at %d px\n", drawn, px);
}

/* Is the next word this one? If so, step over it. The word is given as
   a string, so renaming a subcommand cannot leave a stale length or a
   stale letter behind - the trap the translation fell into three times. */
static int take(const char **p, const char *word)
{
    const char *s = *p;
    while (*s == ' ')
        s++;
    while (*word)
        if (*s++ != *word++)
            return 0;
    if (*s && *s != ' ')
        return 0;
    *p = s;
    return 1;
}

void fonts_command(const char *arg)
{
    const char *p = arg ? arg : "";
    while (*p == ' ')
        p++;

    if (!*p) {
        describe("sans.ttf", &sans, have_sans, why_sans);
        describe("mono.ttf", &mono, have_mono, why_mono);
        return;
    }
    if (take(&p, "sheet")) {
        const struct ttf_font *f = (const struct ttf_font *)0;
        if (take(&p, "sans"))
            f = font_sans();
        else if (take(&p, "mono"))
            f = font_mono();
        uint32_t px = number(&p), first = number(&p), last = number(&p);
        if (!f || px < 1 || px > 255 || last < first) {
            printk("usage: font sheet sans|mono <px> <first> <last>\n");
            return;
        }
        sheet(f, (int)px, first, last);
        return;
    }
    printk("usage: font  |  font sheet sans|mono <px> <first> <last>\n");
}
