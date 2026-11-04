/* uwoff2 - a WOFF2 font unpacked into the TrueType file it holds
 *
 * WOFF2 (W3C, 2018) is how most sites serve fonts now. All the tables
 * go through one Brotli stream (ubrotli.h), and before that the biggest
 * of them are rewritten to compress better:
 *
 *   glyf/loca: the glyphs taken apart into seven streams - the number of
 *     contours of each glyph, the points of each contour, a flag a point,
 *     the coordinates as deltas in a packed "triplet" code, composite
 *     glyphs' parts, bounding boxes, instructions - and loca dropped, as
 *     it follows from glyf. Putting them back together is most of this.
 *   hmtx: the left side bearings may be left out where they equal the
 *     glyph's xMin.
 *
 * The rest of the tables come out of the stream as they were. The result
 * is the sfnt ttf_open() reads. Every offset and length is checked.
 */

#ifndef UWOFF2_H
#define UWOFF2_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "uwoff.h"
#include "ubrotli.h"

static inline int woff2_is(const unsigned char *in, uint32_t len)
{
    return len >= 48 && in[0] == 'w' && in[1] == 'O' && in[2] == 'F' && in[3] == '2';
}

static const char woff2_tags[63][5] = {
    "cmap", "head", "hhea", "hmtx", "maxp", "name", "OS/2", "post", "cvt ", "fpgm", "glyf",
    "loca", "prep", "CFF ", "VORG", "EBDT", "EBLC", "gasp", "hdmx", "kern", "LTSH", "PCLT",
    "VDMX", "vhea", "vmtx", "BASE", "GDEF", "GPOS", "GSUB", "EBSC", "JSTF", "MATH", "CBDT",
    "CBLC", "COLR", "CPAL", "SVG ", "sbix", "acnt", "avar", "bdat", "bloc", "bsln", "cvar",
    "fdsc", "feat", "fmtx", "fvar", "gvar", "hsty", "just", "lcar", "mort", "morx", "opbd",
    "prop", "trak", "Zapf", "Silf", "Glat", "Gloc", "Feat", "Sill"};

struct w2_rd {
    const unsigned char *p;
    uint32_t len, pos;
    int err;
};

static inline uint32_t w2_u8(struct w2_rd *r)
{
    if (r->pos >= r->len) {
        r->err = 1;
        return 0;
    }
    return r->p[r->pos++];
}

static inline uint32_t w2_u16(struct w2_rd *r)
{
    uint32_t a = w2_u8(r);
    return a << 8 | w2_u8(r);
}

static inline uint32_t w2_u32(struct w2_rd *r)
{
    uint32_t a = w2_u16(r);
    return a << 16 | w2_u16(r);
}

static inline uint32_t w2_base128(struct w2_rd *r)
{
    uint32_t v = 0;
    for (int i = 0; i < 5; i++) {
        uint32_t b = w2_u8(r);
        if (i == 0 && b == 0x80)
            r->err = 1;                  /* no leading zeros */
        if (v & 0xFE000000u)
            r->err = 1;
        v = v << 7 | (b & 0x7F);
        if (!(b & 0x80))
            return v;
    }
    r->err = 1;
    return 0;
}

static inline uint32_t w2_255u16(struct w2_rd *r)
{
    uint32_t c = w2_u8(r);
    if (c == 253)
        return w2_u16(r);
    if (c == 255)
        return w2_u8(r) + 253;
    if (c == 254)
        return w2_u8(r) + 506;
    return c;
}

/* A growing output buffer */
struct w2_out {
    unsigned char *p;
    uint32_t len, cap;
    int err;
};

static inline void w2_put(struct w2_out *o, const void *src, uint32_t n)
{
    if (o->err)
        return;
    if (o->len + n > o->cap) {
        uint32_t cap = o->cap ? o->cap : 4096;
        while (cap < o->len + n)
            cap *= 2;
        if (cap > (64u << 20)) {
            o->err = 1;
            return;
        }
        unsigned char *np = (unsigned char *)realloc(o->p, cap);
        if (!np) {
            o->err = 1;
            return;
        }
        o->p = np;
        o->cap = cap;
    }
    const unsigned char *s = (const unsigned char *)src;
    for (uint32_t i = 0; i < n; i++)
        o->p[o->len + i] = s[i];
    o->len += n;
}

static inline void w2_put16(struct w2_out *o, uint32_t v)
{
    unsigned char b[2] = {(unsigned char)(v >> 8), (unsigned char)v};
    w2_put(o, b, 2);
}

/* One point's deltas out of the glyph stream (WOFF2, 5.2) */
static inline int w2_triplet(struct w2_rd *g, uint32_t flag, int *dx, int *dy)
{
    flag &= 0x7F;
    int sx = (flag & 1) ? 1 : -1, sy = (flag & 2) ? 1 : -1;
    if (flag < 10) {
        *dx = 0;
        *dy = (flag & 1 ? 1 : -1) * (int)(((flag & 14) << 7) + w2_u8(g));
    } else if (flag < 20) {
        *dx = (flag & 1 ? 1 : -1) * (int)((((flag - 10) & 14) << 7) + w2_u8(g));
        *dy = 0;
    } else if (flag < 84) {
        uint32_t b0 = flag - 20, b1 = w2_u8(g);
        *dx = sx * (int)(1 + (b0 & 0x30) + (b1 >> 4));
        *dy = sy * (int)(1 + ((b0 & 0x0C) << 2) + (b1 & 0x0F));
    } else if (flag < 120) {
        uint32_t b0 = flag - 84;
        *dx = sx * (int)(1 + ((b0 / 12) << 8) + w2_u8(g));
        *dy = sy * (int)(1 + (((b0 % 12) >> 2) << 8) + w2_u8(g));
    } else if (flag < 124) {
        uint32_t b1 = w2_u8(g), b2 = w2_u8(g), b3 = w2_u8(g);
        *dx = sx * (int)((b1 << 4) + (b2 >> 4));
        *dy = sy * (int)(((b2 & 0x0F) << 8) + b3);
    } else {
        uint32_t b1 = w2_u8(g), b2 = w2_u8(g), b3 = w2_u8(g), b4 = w2_u8(g);
        *dx = sx * (int)((b1 << 8) + b2);
        *dy = sy * (int)((b3 << 8) + b4);
    }
    return g->err ? -1 : 0;
}

/* The transformed glyf back into glyf and loca; the glyphs' xMin kept
   for hmtx. 0, or -1. */
static int w2_glyf(const unsigned char *t, uint32_t tlen, struct w2_out *glyf,
                   struct w2_out *loca, int16_t **xmin_out, uint32_t *nglyphs_out,
                   int *loca_long)
{
    struct w2_rd h = {t, tlen, 0, 0};
    w2_u16(&h);                          /* reserved */
    uint32_t options = w2_u16(&h), n = w2_u16(&h), index_format = w2_u16(&h);
    uint32_t size[7], off = 36;
    for (int i = 0; i < 7; i++)
        size[i] = w2_u32(&h);
    if (h.err)
        return -1;
    struct w2_rd s[7];
    for (int i = 0; i < 7; i++) {
        if (size[i] > tlen - off || off > tlen)
            return -1;
        s[i].p = t + off;
        s[i].len = size[i];
        s[i].pos = 0;
        s[i].err = 0;
        off += size[i];
    }
    (void)options;                       /* (the overlap bitmap: not needed to draw) */
    struct w2_rd *ncont = &s[0], *npts = &s[1], *flags = &s[2], *gl = &s[3], *comp = &s[4],
                 *bbox = &s[5], *instr = &s[6];
    uint32_t bitmap = ((n + 31) >> 5) << 2;
    if (bitmap > bbox->len)
        return -1;
    const unsigned char *bbits = bbox->p;
    bbox->pos = bitmap;
    int16_t *xmin = (int16_t *)calloc(n ? n : 1, sizeof(int16_t));
    if (!xmin)
        return -1;
    *loca_long = index_format != 0;
    uint16_t *ends = 0;
    int32_t *xs = 0, *ys = 0;
    unsigned char *on = 0;
    uint32_t cap_pts = 0, cap_ends = 0;
    for (uint32_t g = 0; g < n; g++) {
        /* loca: where this glyph starts */
        if (*loca_long) {
            unsigned char b[4] = {(unsigned char)(glyf->len >> 24), (unsigned char)(glyf->len >> 16),
                                  (unsigned char)(glyf->len >> 8), (unsigned char)glyf->len};
            w2_put(loca, b, 4);
        } else {
            w2_put16(loca, glyf->len >> 1);
        }
        int nc = (int16_t)w2_u16(ncont);
        int has_box = bbits[g >> 3] & (0x80 >> (g & 7));
        int16_t box[4] = {0, 0, 0, 0};
        if (nc == 0) {
            if (has_box)
                goto bad;                /* an empty glyph has no box */
            continue;
        }
        if (nc > 0) {
            if ((uint32_t)nc > cap_ends) {
                cap_ends = (uint32_t)nc;
                uint16_t *ne = (uint16_t *)realloc(ends, cap_ends * sizeof(uint16_t));
                if (!ne)
                    goto bad;
                ends = ne;
            }
            uint32_t total = 0;
            for (int c = 0; c < nc; c++) {
                total += w2_255u16(npts);
                if (total > 65535)
                    goto bad;
                ends[c] = (uint16_t)(total - 1);
            }
            if (total > cap_pts) {
                cap_pts = total;
                int32_t *nx = (int32_t *)realloc(xs, cap_pts * sizeof(int32_t));
                if (nx)
                    xs = nx;
                int32_t *ny = (int32_t *)realloc(ys, cap_pts * sizeof(int32_t));
                if (ny)
                    ys = ny;
                unsigned char *no = (unsigned char *)realloc(on, cap_pts);
                if (no)
                    on = no;
                if (!nx || !ny || !no)
                    goto bad;
            }
            int32_t x = 0, y = 0;
            for (uint32_t i = 0; i < total; i++) {
                uint32_t f = w2_u8(flags);
                int dx, dy;
                if (w2_triplet(gl, f, &dx, &dy) < 0)
                    goto bad;
                x += dx;
                y += dy;
                xs[i] = x;
                ys[i] = y;
                on[i] = !(f & 0x80);
            }
            uint32_t ilen = w2_255u16(gl);
            if (ncont->err || npts->err || flags->err || gl->err || ilen > instr->len - instr->pos)
                goto bad;
            if (has_box) {
                for (int k = 0; k < 4; k++)
                    box[k] = (int16_t)w2_u16(bbox);
            } else if (total) {
                box[0] = box[2] = (int16_t)xs[0];
                box[1] = box[3] = (int16_t)ys[0];
                for (uint32_t i = 1; i < total; i++) {
                    if (xs[i] < box[0]) box[0] = (int16_t)xs[i];
                    if (ys[i] < box[1]) box[1] = (int16_t)ys[i];
                    if (xs[i] > box[2]) box[2] = (int16_t)xs[i];
                    if (ys[i] > box[3]) box[3] = (int16_t)ys[i];
                }
            }
            xmin[g] = box[0];
            w2_put16(glyf, (uint32_t)nc);
            for (int k = 0; k < 4; k++)
                w2_put16(glyf, (uint16_t)box[k]);
            for (int c = 0; c < nc; c++)
                w2_put16(glyf, ends[c]);
            w2_put16(glyf, ilen);
            w2_put(glyf, instr->p + instr->pos, ilen);
            instr->pos += ilen;
            /* flags and coordinates, plainly: a flag a point, x and y as
               short deltas where they fit */
            int32_t px = 0, py = 0;
            for (uint32_t i = 0; i < total; i++) {
                int32_t dx = xs[i] - px, dy = ys[i] - py;
                unsigned char f = on[i] ? 1 : 0;
                if (dx == 0)
                    f |= 0x10;
                else if (dx > -256 && dx < 256)
                    f |= 0x02 | (dx > 0 ? 0x10 : 0);
                if (dy == 0)
                    f |= 0x20;
                else if (dy > -256 && dy < 256)
                    f |= 0x04 | (dy > 0 ? 0x20 : 0);
                w2_put(glyf, &f, 1);
                px = xs[i];
                py = ys[i];
            }
            px = 0;
            for (uint32_t i = 0; i < total; i++) {
                int32_t dx = xs[i] - px;
                px = xs[i];
                if (dx == 0)
                    continue;
                if (dx > -256 && dx < 256) {
                    unsigned char b = (unsigned char)(dx < 0 ? -dx : dx);
                    w2_put(glyf, &b, 1);
                } else {
                    w2_put16(glyf, (uint16_t)dx);
                }
            }
            py = 0;
            for (uint32_t i = 0; i < total; i++) {
                int32_t dy = ys[i] - py;
                py = ys[i];
                if (dy == 0)
                    continue;
                if (dy > -256 && dy < 256) {
                    unsigned char b = (unsigned char)(dy < 0 ? -dy : dy);
                    w2_put(glyf, &b, 1);
                } else {
                    w2_put16(glyf, (uint16_t)dy);
                }
            }
        } else {
            /* composite: its parts as they were, a box always given */
            if (!has_box)
                goto bad;
            for (int k = 0; k < 4; k++)
                box[k] = (int16_t)w2_u16(bbox);
            xmin[g] = box[0];
            w2_put16(glyf, 0xFFFF);
            for (int k = 0; k < 4; k++)
                w2_put16(glyf, (uint16_t)box[k]);
            int instructions = 0;
            uint32_t more = 1;
            while (more) {
                uint32_t start = comp->pos;
                uint32_t f = w2_u16(comp);
                w2_u16(comp);            /* the glyph */
                uint32_t args = (f & 1) ? 4 : 2;
                uint32_t scale = (f & 8) ? 2 : (f & 0x40) ? 4 : (f & 0x80) ? 8 : 0;
                comp->pos += args + scale;
                if (comp->err || comp->pos > comp->len)
                    goto bad;
                if (f & 0x100)
                    instructions = 1;
                more = f & 0x20;
                w2_put(glyf, comp->p + start, comp->pos - start);
            }
            if (instructions) {
                uint32_t ilen = w2_255u16(gl);
                if (gl->err || ilen > instr->len - instr->pos)
                    goto bad;
                w2_put16(glyf, ilen);
                w2_put(glyf, instr->p + instr->pos, ilen);
                instr->pos += ilen;
            }
        }
        while (glyf->len & 3) {
            unsigned char z = 0;
            w2_put(glyf, &z, 1);
        }
        if (bbox->err)
            goto bad;
    }
    if (*loca_long) {
        unsigned char b[4] = {(unsigned char)(glyf->len >> 24), (unsigned char)(glyf->len >> 16),
                              (unsigned char)(glyf->len >> 8), (unsigned char)glyf->len};
        w2_put(loca, b, 4);
    } else {
        if (glyf->len >= (2u << 16))
            goto bad;
        w2_put16(loca, glyf->len >> 1);
    }
    free(ends);
    free(xs);
    free(ys);
    free(on);
    *xmin_out = xmin;
    *nglyphs_out = n;
    return glyf->err || loca->err ? -1 : 0;
bad:
    free(ends);
    free(xs);
    free(ys);
    free(on);
    free(xmin);
    return -1;
}

/* The transformed hmtx back, its left side bearings from the glyphs'
   xMin where they were left out */
static int w2_hmtx(const unsigned char *t, uint32_t tlen, uint32_t nhm, uint32_t ng,
                   const int16_t *xmin, struct w2_out *out)
{
    struct w2_rd r = {t, tlen, 0, 0};
    uint32_t f = w2_u8(&r);
    if (nhm > ng || !nhm)
        return -1;
    const unsigned char *adv = t + 1;
    r.pos += 2 * nhm;
    const unsigned char *lsb = t + r.pos;
    if (!(f & 1))
        r.pos += 2 * nhm;
    const unsigned char *lsb2 = t + r.pos;
    if (!(f & 2))
        r.pos += 2 * (ng - nhm);
    if (r.pos > tlen || r.err)
        return -1;
    for (uint32_t i = 0; i < ng; i++) {
        if (i < nhm)
            w2_put(out, adv + 2 * i, 2);
        if (i < nhm)
            (f & 1) ? w2_put16(out, (uint16_t)xmin[i]) : w2_put(out, lsb + 2 * i, 2);
        else
            (f & 2) ? w2_put16(out, (uint16_t)xmin[i]) : w2_put(out, lsb2 + 2 * (i - nhm), 2);
    }
    return out->err ? -1 : 0;
}

/* Unpacks a WOFF2 font. The sfnt in a malloc'd buffer (*out, *out_len),
   or -1 when it is damaged or a collection. */
static int woff2_unpack(const unsigned char *in, uint32_t len, unsigned char **out,
                        uint32_t *out_len)
{
    if (!woff2_is(in, len))
        return -1;
    struct w2_rd r = {in, len, 4, 0};
    uint32_t flavor = w2_u32(&r);
    if (flavor == 0x74746366)
        return -1;                       /* 'ttcf': a collection - not here */
    w2_u32(&r);                          /* length */
    uint32_t n = w2_u16(&r);
    w2_u16(&r);
    uint32_t total = w2_u32(&r), comp = w2_u32(&r);
    r.pos = 48;
    if (!n || n > 256 || total > (64u << 20))
        return -1;
    struct {
        char tag[4];
        uint32_t olen, tlen, ver;
        uint32_t at;                     /* where in the unpacked stream */
    } t[256];
    uint32_t sum = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t f = w2_u8(&r), idx = f & 63, ver = f >> 6;
        if (idx == 63) {
            for (int k = 0; k < 4; k++)
                t[i].tag[k] = (char)w2_u8(&r);
        } else {
            for (int k = 0; k < 4; k++)
                t[i].tag[k] = woff2_tags[idx][k];
        }
        t[i].olen = w2_base128(&r);
        int glyf_or_loca = !memcmp(t[i].tag, "glyf", 4) || !memcmp(t[i].tag, "loca", 4);
        int transformed = glyf_or_loca ? ver != 3 : ver != 0;
        t[i].ver = transformed ? 1 : 0;
        t[i].tlen = transformed ? w2_base128(&r) : t[i].olen;
        if (transformed && !glyf_or_loca && memcmp(t[i].tag, "hmtx", 4))
            return -1;                   /* a transform this does not know */
        t[i].at = sum;
        sum += t[i].tlen;
        if (r.err || sum > (64u << 20))
            return -1;
    }
    if (comp > len - r.pos)
        return -1;
    unsigned char *raw = (unsigned char *)malloc(sum ? sum : 1);
    if (!raw)
        return -1;
    if (brotli_decode(in + r.pos, comp, raw, sum) != (int)sum) {
        free(raw);
        return -1;
    }

    /* the tables each sfnt needs, by tag */
    int ig = -1, il = -1, ih = -1, ihh = -1, im = -1;
    for (uint32_t i = 0; i < n; i++) {
        if (!memcmp(t[i].tag, "glyf", 4)) ig = (int)i;
        if (!memcmp(t[i].tag, "loca", 4)) il = (int)i;
        if (!memcmp(t[i].tag, "hmtx", 4)) ih = (int)i;
        if (!memcmp(t[i].tag, "hhea", 4)) ihh = (int)i;
        if (!memcmp(t[i].tag, "maxp", 4)) im = (int)i;
    }
    struct w2_out glyf = {0, 0, 0, 0}, loca = {0, 0, 0, 0}, hmtx = {0, 0, 0, 0};
    int16_t *xmin = 0;
    uint32_t ng = 0;
    int loca_long = 0, ok = 1;
    if (ig >= 0 && t[ig].ver) {
        ok = il >= 0 && w2_glyf(raw + t[ig].at, t[ig].tlen, &glyf, &loca, &xmin, &ng,
                                &loca_long) == 0;
    }
    if (ok && ih >= 0 && t[ih].ver) {
        ok = xmin && ihh >= 0 && im >= 0 && t[ihh].tlen >= 36 && t[im].tlen >= 6;
        if (ok) {
            const unsigned char *hh = raw + t[ihh].at;
            uint32_t nhm = (uint32_t)hh[34] << 8 | hh[35];
            ok = w2_hmtx(raw + t[ih].at, t[ih].tlen, nhm, ng, xmin, &hmtx) == 0;
        }
    }
    free(xmin);

    /* the sfnt: offset table, records, tables padded to four */
    struct w2_out o = {0, 0, 0, 0};
    if (ok) {
        int sel = 0;
        while ((2u << sel) <= n)
            sel++;
        unsigned char head[12];
        woff_put32(head, flavor);
        woff_put16(head + 4, n);
        woff_put16(head + 6, 16u << sel);
        woff_put16(head + 8, (uint32_t)sel);
        woff_put16(head + 10, n * 16 - (16u << sel));
        w2_put(&o, head, 12);
        unsigned char zero[16] = {0};
        for (uint32_t i = 0; i < n; i++)
            w2_put(&o, zero, 16);
        for (uint32_t i = 0; i < n && !o.err; i++) {
            const unsigned char *data = raw + t[i].at;
            uint32_t dlen = t[i].tlen;
            if ((int)i == ig && t[i].ver) {
                data = glyf.p;
                dlen = glyf.len;
            } else if ((int)i == il && ig >= 0 && t[ig].ver) {
                data = loca.p;
                dlen = loca.len;
            } else if ((int)i == ih && t[i].ver) {
                data = hmtx.p;
                dlen = hmtx.len;
            }
            uint32_t at = o.len;
            w2_put(&o, data, dlen);
            while (o.len & 3)
                w2_put(&o, zero, 1);
            if (o.err)
                break;
            unsigned char *rec = o.p + 12 + 16 * i;
            for (int k = 0; k < 4; k++)
                rec[k] = (unsigned char)t[i].tag[k];
            woff_put32(rec + 4, 0);
            woff_put32(rec + 8, at);
            woff_put32(rec + 12, dlen);
            if (!memcmp(t[i].tag, "head", 4) && dlen >= 52 && ig >= 0 && t[ig].ver)
                woff_put16(o.p + at + 50, (uint32_t)loca_long);   /* indexToLocFormat */
        }
    }
    free(raw);
    free(glyf.p);
    free(loca.p);
    free(hmtx.p);
    if (!ok || o.err) {
        free(o.p);
        return -1;
    }
    *out = o.p;
    *out_len = o.len;
    return 0;
}

#endif
