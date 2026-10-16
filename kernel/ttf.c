/* TrueType - reading a font and drawing its glyphs
 *
 * Three steps, and each is the same thing FreeType does with hinting
 * off, because FreeType is what the test holds this up against:
 *
 *   1. The outline. A glyph is contours of points in font units, each
 *      point either ON the curve or a control point between two that
 *      are. Accented letters are mostly COMPOSITE glyphs - an 'e' and
 *      an acute put together with an offset - so a glyph may be built
 *      out of others, recursively.
 *
 *   2. Scaling, to 26.6 fixed point (1/64 of a pixel), with FreeType's
 *      own rounding: every coordinate is FT_MulFix(units, scale). The
 *      bounding box comes from the scaled points, and so does the size
 *      of the bitmap - get one rounding different and the bitmap is a
 *      pixel narrower than FreeType's and nothing lines up.
 *
 *   3. Coverage. Each quadratic arc is cut into straight lines (the
 *      number of pieces chosen the way FreeType chooses it), and each
 *      line adds the area it sweeps to an accumulation buffer - the
 *      method of Raph Levien's font-rs. A running sum along each row
 *      then gives, for every pixel, the fraction of it inside the
 *      glyph. That fraction IS the anti-aliasing: nothing is sampled,
 *      the area is computed.
 *
 * All of it in integers. The 64-bit kernel is built without SSE and
 * the 32-bit one without libgcc - no floating point and no 64-bit
 * division - so the few divisions that need 64 bits go through div64()
 * below.
 *
 * The file is read with every offset checked against its length. Fonts
 * are files, and files come from disks and networks; a bad one has to
 * be refused, not followed.
 */

#include <stdint.h>
#include "include/ttf.h"
#include "include/string.h"
#include "include/spinlock.h"

/* --- reading the file ------------------------------------------------ */

static uint16_t u16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static int16_t s16(const uint8_t *p)
{
    return (int16_t)u16(p);
}

static uint32_t u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

/* Are n bytes at 'off' inside the file? Written so that it cannot
   overflow, whatever the two numbers are. */
static int inside(const struct ttf_font *f, uint32_t off, uint32_t n)
{
    return off <= f->len && n <= f->len - off;
}

#define TAG(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | \
                         ((uint32_t)(c) << 8) | (uint32_t)(d))

/* --- fixed point ------------------------------------------------------- */

/* A signed 64-bit division without libgcc. Long division, one bit at a
   time - slow by the standards of a divide instruction and irrelevant
   next to the cost of drawing: it runs a few times per row of a glyph. */
static int64_t div64(int64_t a, int64_t b)
{
#ifdef __x86_64__
    return a / b;
#else
    int neg = (a < 0) != (b < 0);
    uint64_t n = a < 0 ? (uint64_t)-a : (uint64_t)a;
    uint64_t d = b < 0 ? (uint64_t)-b : (uint64_t)b;
    uint64_t q = 0, r = 0;
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) {
            r -= d;
            q |= (uint64_t)1 << i;
        }
    }
    return neg ? -(int64_t)q : (int64_t)q;
#endif
}

/* FreeType's FT_MulFix: a * b / 65536, rounded, the sign applied to
   the magnitude - so -x rounds exactly as x does */
static int32_t mulfix(int32_t a, int32_t b)
{
    int neg = (a < 0) != (b < 0);
    uint64_t ua = a < 0 ? (uint64_t)-(int64_t)a : (uint64_t)a;
    uint64_t ub = b < 0 ? (uint64_t)-(int64_t)b : (uint64_t)b;
    uint32_t c = (uint32_t)((ua * ub + 0x8000) >> 16);
    return neg ? -(int32_t)c : (int32_t)c;
}

/* The scale from font units to 26.6 at a size: FT_DivFix(px * 64,
   units_per_em). Fits in 32 bits for any size up to 255 pixels. */
static int32_t scale_for(const struct ttf_font *f, int px)
{
    uint32_t a = (uint32_t)px << 22;          /* (px * 64) << 16 */
    uint32_t b = (uint32_t)f->units_per_em;
    return (int32_t)((a + b / 2) / b);
}

/* --- opening a font ---------------------------------------------------- */

static int fail(const char **why, const char *reason)
{
    if (why)
        *why = reason;
    return -1;
}

/* Which character map to use. A font carries several, for different
   platforms; the Unicode ones are the only ones that mean anything
   here, and the full-range one (format 12) beats the 16-bit one. */
static void choose_cmap(struct ttf_font *f, uint32_t cmap, uint32_t clen)
{
    const uint8_t *d = f->data;
    int best = 0;

    if (clen < 4)
        return;
    int n = u16(d + cmap + 2);
    if (4u + 8u * (uint32_t)n > clen)
        return;
    for (int i = 0; i < n; i++) {
        const uint8_t *r = d + cmap + 4 + 8 * i;
        int platform = u16(r), encoding = u16(r + 2);
        uint32_t off = cmap + u32(r + 4);
        if (!inside(f, off, 8))
            continue;
        int format = u16(d + off);
        int rank = 0;
        if (format == 12 && (platform == 0 || (platform == 3 && encoding == 10)))
            rank = 3;
        else if (format == 4 && (platform == 0 || (platform == 3 && encoding == 1)))
            rank = 2;
        if (format == 4 && !inside(f, off, u16(d + off + 2)))
            rank = 0;
        if (format == 12 && !inside(f, off, u32(d + off + 4)))
            rank = 0;
        if (rank > best) {
            best = rank;
            f->cmap_sub = off;
            f->cmap_format = format;
        }
    }
}

int ttf_open(struct ttf_font *f, const void *data, uint32_t len,
             const char **why)
{
    memset(f, 0, sizeof(*f));
    f->data = (const uint8_t *)data;
    f->len = len;
    const uint8_t *d = f->data;

    if (len < 12)
        return fail(why, "too short to be a font");
    uint32_t version = u32(d);
    if (version != 0x00010000 && version != TAG('t', 'r', 'u', 'e'))
        return fail(why, "not a TrueType font (or outlines in CFF, "
                         "which this does not read)");

    int tables = u16(d + 4);
    if (!inside(f, 12, 16u * (uint32_t)tables))
        return fail(why, "the table directory runs past the end");

    uint32_t head = 0, maxp = 0, hhea = 0, cmap = 0;
    uint32_t head_len = 0, maxp_len = 0, hhea_len = 0, cmap_len = 0;
    uint32_t loca_len = 0;
    for (int i = 0; i < tables; i++) {
        const uint8_t *r = d + 12 + 16 * i;
        uint32_t tag = u32(r), off = u32(r + 8), tlen = u32(r + 12);
        if (!inside(f, off, tlen))
            return fail(why, "a table runs past the end of the file");
        switch (tag) {
        case TAG('h', 'e', 'a', 'd'): head = off; head_len = tlen; break;
        case TAG('m', 'a', 'x', 'p'): maxp = off; maxp_len = tlen; break;
        case TAG('h', 'h', 'e', 'a'): hhea = off; hhea_len = tlen; break;
        case TAG('h', 'm', 't', 'x'): f->hmtx = off; f->hmtx_len = tlen; break;
        case TAG('l', 'o', 'c', 'a'): f->loca = off; loca_len = tlen; break;
        case TAG('g', 'l', 'y', 'f'): f->glyf = off; f->glyf_len = tlen; break;
        case TAG('c', 'm', 'a', 'p'): cmap = off; cmap_len = tlen; break;
        }
    }
    if (!head || !maxp || !hhea || !f->hmtx || !f->loca || !f->glyf)
        return fail(why, "a table every TrueType font has is missing");
    if (head_len < 54 || maxp_len < 6 || hhea_len < 36)
        return fail(why, "a table is shorter than its format");

    f->units_per_em = u16(d + head + 18);
    if (f->units_per_em < 16 || f->units_per_em > 16384)
        return fail(why, "units per em out of range");
    f->long_loca = s16(d + head + 50) != 0;
    f->glyphs = u16(d + maxp + 4);
    if (f->glyphs < 1)
        return fail(why, "a font with no glyphs");

    f->ascender = s16(d + hhea + 4);
    f->descender = s16(d + hhea + 6);
    f->line_gap = s16(d + hhea + 8);
    f->hmetrics = u16(d + hhea + 34);
    if (f->hmetrics < 1 || f->hmetrics > f->glyphs ||
        (uint32_t)f->hmetrics * 4 > f->hmtx_len)
        return fail(why, "the horizontal metrics do not add up");

    uint32_t need = ((uint32_t)f->glyphs + 1) * (f->long_loca ? 4u : 2u);
    if (loca_len < need)
        return fail(why, "the glyph index is shorter than the glyph count");

    if (cmap)
        choose_cmap(f, cmap, cmap_len);
    if (!f->cmap_sub)
        return fail(why, "no Unicode character map");
    if (why)
        *why = (const char *)0;
    return 0;
}

/* --- characters to glyphs ---------------------------------------------- */

static int cmap4(const struct ttf_font *f, uint32_t c)
{
    const uint8_t *d = f->data;
    uint32_t sub = f->cmap_sub;

    if (c > 0xFFFF || !inside(f, sub, 14))
        return 0;
    uint32_t seg2 = u16(d + sub + 6);
    uint32_t ends = sub + 14, starts = ends + seg2 + 2;
    uint32_t deltas = starts + seg2, ranges = deltas + seg2;
    if (!inside(f, sub, 16 + 4 * seg2))
        return 0;

    /* The segments are sorted by their last character: the first one
       ending at or after c is the only one that can hold it */
    uint32_t lo = 0, hi = seg2 / 2;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (u16(d + ends + 2 * mid) < c)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo >= seg2 / 2)
        return 0;
    uint32_t i = lo;
    uint32_t start = u16(d + starts + 2 * i);
    if (start > c)
        return 0;
    uint16_t delta = u16(d + deltas + 2 * i);
    uint32_t ro = u16(d + ranges + 2 * i);
    if (!ro)
        return (int)((c + delta) & 0xFFFF);
    /* idRangeOffset counts from its own position in the file - the
       famous pointer trick of format 4, spelled out */
    uint32_t at = ranges + 2 * i + ro + 2 * (c - start);
    if (!inside(f, at, 2))
        return 0;
    uint32_t g = u16(d + at);
    return g ? (int)((g + delta) & 0xFFFF) : 0;
}

static int cmap12(const struct ttf_font *f, uint32_t c)
{
    const uint8_t *d = f->data;
    uint32_t sub = f->cmap_sub;

    if (!inside(f, sub, 16))
        return 0;
    uint32_t n = u32(d + sub + 12);
    if (n > (f->len - sub - 16) / 12)
        return 0;
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        const uint8_t *g = d + sub + 16 + 12 * mid;
        if (u32(g + 4) < c)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo >= n)
        return 0;
    const uint8_t *g = d + sub + 16 + 12 * lo;
    if (u32(g) > c)
        return 0;
    return (int)(u32(g + 8) + (c - u32(g)));
}

int ttf_glyph(const struct ttf_font *f, uint32_t codepoint)
{
    int g = f->cmap_format == 12 ? cmap12(f, codepoint)
                                 : cmap4(f, codepoint);
    return (g > 0 && g < f->glyphs) ? g : 0;
}

/* --- metrics ------------------------------------------------------------- */

int32_t ttf_advance(const struct ttf_font *f, int glyph, int px)
{
    if (glyph < 0 || glyph >= f->glyphs || px < 1 || px > 255)
        return 0;
    int i = glyph < f->hmetrics ? glyph : f->hmetrics - 1;
    int32_t units = u16(f->data + f->hmtx + 4 * i);
    return mulfix(units, scale_for(f, px));
}

void ttf_vmetrics(const struct ttf_font *f, int px, int32_t *ascent,
                  int32_t *descent, int32_t *gap)
{
    int32_t s = (px >= 1 && px <= 255) ? scale_for(f, px) : 0;
    if (ascent)
        *ascent = mulfix(f->ascender, s);
    if (descent)
        *descent = mulfix(f->descender, s);
    if (gap)
        *gap = mulfix(f->line_gap, s);
}

/* --- outlines ------------------------------------------------------------ */

#define MAXPTS   1024
#define MAXCONT  256
#define DEPTH    6                  /* composites inside composites, at most */

struct outline {
    int n, nc;
    int32_t x[MAXPTS], y[MAXPTS];  /* 26.6, y up */
    uint8_t on[MAXPTS];
    uint16_t end[MAXCONT];
};

static int glyph_range(const struct ttf_font *f, int g, uint32_t *off,
                       uint32_t *len)
{
    const uint8_t *d = f->data;
    uint32_t a, b;

    if (g < 0 || g >= f->glyphs)
        return -1;
    if (f->long_loca) {
        a = u32(d + f->loca + 4 * g);
        b = u32(d + f->loca + 4 * g + 4);
    } else {
        a = 2u * u16(d + f->loca + 2 * g);
        b = 2u * u16(d + f->loca + 2 * g + 2);
    }
    if (b < a || b > f->glyf_len)
        return -1;
    *off = f->glyf + a;
    *len = b - a;
    return 0;
}

#define F_ON       0x01
#define F_XSHORT   0x02
#define F_YSHORT   0x04
#define F_REPEAT   0x08
#define F_XSAME    0x10
#define F_YSAME    0x20

/* A simple glyph: its points, scaled, appended to the outline */
static int load_simple(const struct ttf_font *f, uint32_t off, uint32_t len,
                       int contours, int32_t scale, struct outline *o)
{
    const uint8_t *d = f->data;
    uint32_t p = off + 10, stop = off + len;

    if (o->nc + contours > MAXCONT || p + 2u * (uint32_t)contours + 2 > stop)
        return -1;
    int first = o->n;
    int npts = 0;
    for (int c = 0; c < contours; c++) {
        int e = u16(d + p + 2 * c);
        if (e < npts - 1 || (c && e <= o->end[o->nc + c - 1] - first))
            return -1;                   /* contour ends must grow */
        o->end[o->nc + c] = (uint16_t)(first + e);
        npts = e + 1;
    }
    if (first + npts > MAXPTS)
        return -1;
    p += 2u * (uint32_t)contours;
    uint32_t ilen = u16(d + p);
    p += 2 + ilen;                       /* the hinting program - not run */
    if (p > stop)
        return -1;

    /* Flags, run-length coded */
    for (int i = 0; i < npts;) {
        if (p >= stop)
            return -1;
        uint8_t fl = d[p++];
        int times = 1;
        if (fl & F_REPEAT) {
            if (p >= stop)
                return -1;
            times += d[p++];
        }
        while (times-- && i < npts)
            o->on[first + i++] = fl;
    }
    /* X, then Y: each a delta from the point before, a byte or a word */
    int32_t v = 0;
    for (int i = 0; i < npts; i++) {
        uint8_t fl = o->on[first + i];
        if (fl & F_XSHORT) {
            if (p + 1 > stop)
                return -1;
            v += (fl & F_XSAME) ? d[p] : -(int32_t)d[p];
            p += 1;
        } else if (!(fl & F_XSAME)) {
            if (p + 2 > stop)
                return -1;
            v += s16(d + p);
            p += 2;
        }
        o->x[first + i] = v;
    }
    v = 0;
    for (int i = 0; i < npts; i++) {
        uint8_t fl = o->on[first + i];
        if (fl & F_YSHORT) {
            if (p + 1 > stop)
                return -1;
            v += (fl & F_YSAME) ? d[p] : -(int32_t)d[p];
            p += 1;
        } else if (!(fl & F_YSAME)) {
            if (p + 2 > stop)
                return -1;
            v += s16(d + p);
            p += 2;
        }
        o->y[first + i] = v;
    }
    /* Scaled straight away, point by point - the order FreeType does
       it in, which decides how the rounding falls */
    for (int i = first; i < first + npts; i++) {
        o->x[i] = mulfix(o->x[i], scale);
        o->y[i] = mulfix(o->y[i], scale);
        o->on[i] &= F_ON;
    }
    o->n = first + npts;
    o->nc += contours;
    return 0;
}

#define C_WORDS     0x0001
#define C_XY        0x0002
#define C_SCALE     0x0008
#define C_MORE      0x0020
#define C_XYSCALE   0x0040
#define C_2X2       0x0080
#define C_SCALED_OFFSET   0x0800
#define C_UNSCALED_OFFSET 0x1000

static int load_glyph(const struct ttf_font *f, int g, int32_t scale,
                      struct outline *o, int depth);

/* A composite: other glyphs, each moved (and perhaps scaled) into
   place. The offsets are in font units and are scaled on their own,
   after the component's points - FreeType's order again. */
static int load_composite(const struct ttf_font *f, uint32_t off,
                          uint32_t len, int32_t scale, struct outline *o,
                          int depth)
{
    const uint8_t *d = f->data;
    uint32_t p = off + 10, stop = off + len;
    uint16_t flags;

    do {
        if (p + 4 > stop)
            return -1;
        flags = u16(d + p);
        int sub = u16(d + p + 2);
        p += 4;

        int32_t dx, dy;
        if (flags & C_WORDS) {
            if (p + 4 > stop)
                return -1;
            dx = s16(d + p);
            dy = s16(d + p + 2);
            p += 4;
        } else {
            if (p + 2 > stop)
                return -1;
            dx = (int8_t)d[p];
            dy = (int8_t)d[p + 1];
            p += 2;
        }
        /* The matrix in 16.16, from 2.14 */
        int32_t xx = 0x10000, xy = 0, yx = 0, yy = 0x10000;
        int identity = 1;
        if (flags & C_SCALE) {
            if (p + 2 > stop)
                return -1;
            xx = yy = (int32_t)s16(d + p) * 4;
            p += 2;
            identity = 0;
        } else if (flags & C_XYSCALE) {
            if (p + 4 > stop)
                return -1;
            xx = (int32_t)s16(d + p) * 4;
            yy = (int32_t)s16(d + p + 2) * 4;
            p += 4;
            identity = 0;
        } else if (flags & C_2X2) {
            if (p + 8 > stop)
                return -1;
            xx = (int32_t)s16(d + p) * 4;
            yx = (int32_t)s16(d + p + 2) * 4;
            xy = (int32_t)s16(d + p + 4) * 4;
            yy = (int32_t)s16(d + p + 6) * 4;
            p += 8;
            identity = 0;
        }

        int from = o->n;
        if (load_glyph(f, sub, scale, o, depth + 1) < 0)
            return -1;

        if (!identity) {
            for (int i = from; i < o->n; i++) {
                int32_t x = o->x[i], y = o->y[i];
                o->x[i] = mulfix(x, xx) + mulfix(y, xy);
                o->y[i] = mulfix(x, yx) + mulfix(y, yy);
            }
        }
        /* Point matching (offsets given as point numbers rather than
           distances) is a feature nobody's fonts here use; such a
           component is placed with no offset rather than refused */
        if (!(flags & C_XY))
            dx = dy = 0;
        int32_t ox = mulfix(dx, scale), oy = mulfix(dy, scale);
        if (!identity && (flags & C_SCALED_OFFSET) &&
            !(flags & C_UNSCALED_OFFSET)) {
            int32_t tx = mulfix(ox, xx) + mulfix(oy, xy);
            int32_t ty = mulfix(ox, yx) + mulfix(oy, yy);
            ox = tx;
            oy = ty;
        }
        for (int i = from; i < o->n; i++) {
            o->x[i] += ox;
            o->y[i] += oy;
        }
    } while (flags & C_MORE);
    return 0;
}

static int load_glyph(const struct ttf_font *f, int g, int32_t scale,
                      struct outline *o, int depth)
{
    uint32_t off, len;

    if (depth > DEPTH || glyph_range(f, g, &off, &len) < 0)
        return -1;
    if (!len)
        return 0;                        /* a space: no outline at all */
    if (len < 10)
        return -1;
    int contours = s16(f->data + off);
    if (contours >= 0)
        return load_simple(f, off, len, contours, scale, o);
    return load_composite(f, off, len, scale, o, depth);
}

/* --- coverage ------------------------------------------------------------ */

#define MAXW 192
#define MAXH 192
#define STRIDE (MAXW + 2)

static int32_t acc[STRIDE * MAXH];
static int bw, bh;                      /* the bitmap being drawn */

/* One straight edge, in bitmap coordinates of 1/256 pixel, y down.
 *
 * For every row it crosses, the edge adds to the row the signed area
 * between itself and the right edge of the bitmap - split between the
 * pixels it passes through, and the rest carried by the pixel after.
 * A running sum along the row later turns those increments into the
 * coverage of each pixel. Coverage is kept in 1/65536 of a pixel. */
static void line(int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    if (y0 == y1)
        return;
    int32_t dir = 1;
    if (y0 > y1) {
        int32_t t = x0; x0 = x1; x1 = t;
        t = y0; y0 = y1; y1 = t;
        dir = -1;
    }
    if (y1 <= 0 || y0 >= bh * 256)
        return;

    int64_t dx = (int64_t)x1 - x0, dy_all = (int64_t)y1 - y0;
    int row = y0 > 0 ? y0 >> 8 : 0;
    int last = (y1 + 255) >> 8;
    if (last > bh)
        last = bh;

    for (; row < last; row++) {
        int32_t top = row * 256 > y0 ? row * 256 : y0;
        int32_t bot = (row + 1) * 256 < y1 ? (row + 1) * 256 : y1;
        if (bot <= top)
            continue;
        /* x where the edge enters and leaves this row, each computed
           from the end points rather than accumulated, so no error
           builds up along a tall edge */
        int32_t xa = x0 + (int32_t)div64(dx * (top - y0), dy_all);
        int32_t xb = x0 + (int32_t)div64(dx * (bot - y0), dy_all);
        int32_t d = (bot - top) * 256 * dir;     /* the row's share, 1/65536 */
        int32_t *a = acc + row * STRIDE;

        int32_t lo = xa < xb ? xa : xb, hi = xa < xb ? xb : xa;
        if (lo < 0)
            lo = 0;
        if (hi < 0)
            hi = 0;
        int i0 = lo >> 8;
        int i1 = (hi + 255) >> 8;
        if (i1 > bw)
            i1 = bw;
        if (i0 >= bw)
            i0 = bw - 1;

        if (i1 <= i0 + 1) {
            /* Inside one pixel: split by where the edge crosses it on
               average (in 1/512 of a pixel, so the half is exact) */
            int32_t mid = (xa < 0 ? 0 : xa) + (xb < 0 ? 0 : xb) - (i0 << 9);
            int32_t part = (int32_t)(((int64_t)d * mid) >> 9);
            a[i0] += d - part;
            a[i0 + 1] += part;
        } else {
            /* Across several: a triangle in the first pixel, a
               triangle in the last, equal strips between */
            int32_t width = hi - lo;
            int32_t s = (int32_t)((1u << 24) / (uint32_t)width); /* 1/px, Q16 */
            int32_t f0 = lo - (i0 << 8);
            int32_t a0 = (int32_t)(((int64_t)s * (256 - f0) * (256 - f0)) >> 17);
            int32_t f1 = hi - ((i1 - 1) << 8);
            int32_t am = (int32_t)(((int64_t)s * f1 * f1) >> 17);
            a[i0] += (int32_t)(((int64_t)d * a0) >> 16);
            if (i1 == i0 + 2) {
                a[i0 + 1] += (int32_t)(((int64_t)d * (65536 - a0 - am)) >> 16);
            } else {
                int32_t a1 = (int32_t)(((int64_t)s * (384 - f0)) >> 8);
                a[i0 + 1] += (int32_t)(((int64_t)d * (a1 - a0)) >> 16);
                int32_t ds = (int32_t)(((int64_t)d * s) >> 16);
                for (int i = i0 + 2; i < i1 - 1; i++)
                    a[i] += ds;
                int32_t a2 = a1 + (i1 - i0 - 3) * s;
                a[i1 - 1] += (int32_t)(((int64_t)d * (65536 - a2 - am)) >> 16);
            }
            a[i1] += (int32_t)(((int64_t)d * am) >> 16);
        }
    }
}

/* A quadratic arc, cut into 2^k straight pieces. k is chosen exactly as
   FreeType's rasteriser chooses it: each halving of t divides the
   deviation from the chord by four, so halve until it is under a
   quarter of a pixel. */
static void arc(int32_t x0, int32_t y0, int32_t cx, int32_t cy,
                int32_t x2, int32_t y2)
{
    int32_t ax = x0 - 2 * cx + x2, ay = y0 - 2 * cy + y2;
    int32_t dev = ax < 0 ? -ax : ax;
    int32_t devy = ay < 0 ? -ay : ay;
    if (devy > dev)
        dev = devy;
    if (dev <= 64) {
        line(x0, y0, x2, y2);
        return;
    }
    int k = 0;
    do {
        dev >>= 2;
        k++;
    } while (dev > 64);

    int n = 1 << k;
    int64_t bx = cx - x0, by = cy - y0;
    int32_t px = x0, py = y0;
    /* B(t) = P0 + 2t(C - P0) + t^2 (P0 - 2C + P2), t = i/n, evaluated
       exactly and rounded once */
    for (int i = 1; i <= n; i++) {
        int64_t t1 = (int64_t)i << k;            /* t * n^2 / n... */
        int64_t t2 = (int64_t)i * i;
        int64_t round = (int64_t)1 << (2 * k - 1);
        int32_t qx = x0 + (int32_t)((2 * bx * t1 + ax * t2 + round) >> (2 * k));
        int32_t qy = y0 + (int32_t)((2 * by * t1 + ay * t2 + round) >> (2 * k));
        line(px, py, qx, qy);
        px = qx;
        py = qy;
    }
}

static struct outline ol;
static struct spinlock lock = SPINLOCK_FREE;

/* 26.6 (translated so the box starts at zero, y up) to bitmap space */
#define BX(v)  ((v) * 4)
#define BY(v)  (bh * 256 - (v) * 4)

static void contour(int s, int e, int32_t ox, int32_t oy)
{
    int32_t *X = ol.x, *Y = ol.y;
    int32_t sx, sy;                      /* where the contour starts (26.6) */
    int first;

    /* Start on an on-curve point. If the first point is a control
       point, the last may be on the curve; if neither is, the contour
       starts halfway between them. */
    if (ol.on[s]) {
        sx = X[s] - ox;
        sy = Y[s] - oy;
        first = s + 1;
    } else if (ol.on[e]) {
        sx = X[e] - ox;
        sy = Y[e] - oy;
        first = s;
        e--;
    } else {
        sx = ((X[s] - ox) + (X[e] - ox)) / 2;
        sy = ((Y[s] - oy) + (Y[e] - oy)) / 2;
        first = s;
    }

    int32_t cx = sx, cy = sy;            /* the pen */
    int have_ctrl = 0;
    int32_t kx = 0, ky = 0;              /* a pending control point */

    for (int i = first; i <= e; i++) {
        int32_t x = X[i] - ox, y = Y[i] - oy;
        if (ol.on[i]) {
            if (have_ctrl)
                arc(BX(cx), BY(cy), BX(kx), BY(ky), BX(x), BY(y));
            else
                line(BX(cx), BY(cy), BX(x), BY(y));
            cx = x;
            cy = y;
            have_ctrl = 0;
        } else {
            if (have_ctrl) {
                /* Two control points in a row: the on-curve point
                   between them is implied, halfway */
                int32_t mx = (kx + x) / 2, my = (ky + y) / 2;
                arc(BX(cx), BY(cy), BX(kx), BY(ky), BX(mx), BY(my));
                cx = mx;
                cy = my;
            }
            kx = x;
            ky = y;
            have_ctrl = 1;
        }
    }
    if (have_ctrl)
        arc(BX(cx), BY(cy), BX(kx), BY(ky), BX(sx), BY(sy));
    else
        line(BX(cx), BY(cy), BX(sx), BY(sy));
}

int ttf_render(const struct ttf_font *f, int glyph, int px,
               struct ttf_bitmap *out, uint8_t *buf, uint32_t cap)
{
    if (px < 1 || px > 255)
        return -1;

    spin_lock(&lock);
    ol.n = ol.nc = 0;
    out->w = out->h = out->left = out->top = 0;
    out->alpha = buf;
    if (load_glyph(f, glyph, scale_for(f, px), &ol, 0) < 0) {
        spin_unlock(&lock);
        return -1;
    }
    if (!ol.n) {
        spin_unlock(&lock);
        return 0;                        /* nothing to draw */
    }

    /* The box: every point, control points included, then out to whole
       pixels - FreeType's control box */
    int32_t x0 = ol.x[0], x1 = ol.x[0], y0 = ol.y[0], y1 = ol.y[0];
    for (int i = 1; i < ol.n; i++) {
        if (ol.x[i] < x0) x0 = ol.x[i];
        if (ol.x[i] > x1) x1 = ol.x[i];
        if (ol.y[i] < y0) y0 = ol.y[i];
        if (ol.y[i] > y1) y1 = ol.y[i];
    }
    x0 &= ~63;                           /* floor, also for negatives */
    y0 &= ~63;
    x1 = (x1 + 63) & ~63;
    y1 = (y1 + 63) & ~63;
    int w = (x1 - x0) >> 6, h = (y1 - y0) >> 6;
    if (w <= 0 || h <= 0) {
        spin_unlock(&lock);
        return 0;
    }
    if (w > MAXW || h > MAXH || (uint32_t)(w * h) > cap) {
        spin_unlock(&lock);
        return -1;
    }

    bw = w;
    bh = h;
    memset(acc, 0, sizeof(int32_t) * (uint32_t)(STRIDE * h));
    int s = 0;
    for (int c = 0; c < ol.nc; c++) {
        int e = ol.end[c];
        if (e >= ol.n || e < s) {
            spin_unlock(&lock);
            return -1;
        }
        if (e > s)
            contour(s, e, x0, y0);
        s = e + 1;
    }

    /* The running sum along each row: coverage, 0 .. 65536. Turned
       into 0 .. 255 the way FreeType does it - a full pixel is 256,
       which is then held to 255. */
    for (int r = 0; r < h; r++) {
        int32_t sum = 0;
        int32_t *a = acc + r * STRIDE;
        for (int c = 0; c < w; c++) {
            sum += a[c];
            int32_t cov = sum < 0 ? -sum : sum;
            int32_t v = cov >> 8;
            buf[r * w + c] = (uint8_t)(v > 255 ? 255 : v);
        }
    }
    out->w = w;
    out->h = h;
    out->left = x0 >> 6;
    out->top = y1 >> 6;
    spin_unlock(&lock);
    return 0;
}
