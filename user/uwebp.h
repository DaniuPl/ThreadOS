/* uwebp - a WebP picture, unpacked
 *
 * A WebP file is a RIFF container around one of two quite different
 * codecs, and the browser needs both:
 *
 *   VP8 (lossy, RFC 6386): a key frame of the VP8 video codec. The
 *   picture is Y, U and V planes in macroblocks of 16x16; each is
 *   predicted from the pixels above and to the left of it, and what the
 *   prediction got wrong is sent as 4x4 transforms whose coefficients
 *   are coded with a boolean arithmetic coder, under probabilities that
 *   depend on where in the block a coefficient is and on what its
 *   neighbours held. After all macroblocks a loop filter smooths their
 *   edges. The colour planes are half the size; they are brought up to
 *   full size by bilinear interpolation, as libwebp does, and turned to
 *   RGB by Rec. 601.
 *
 *   VP8L (lossless, RFC 9649): ARGB pixels coded with canonical prefix
 *   codes, LZ77 back references over a two-dimensional distance map, a
 *   cache of recent colours; under up to four transforms - spatial
 *   prediction, a colour decorrelation, green subtracted from red and
 *   blue, and a palette with pixels packed into bytes.
 *
 * Around them, the extended format: an ALPH chunk with the alpha of a
 * lossy picture (raw, or itself VP8L), filtered by a predictor; and
 * animations, of which the first frame is shown, as for GIFs.
 *
 * Needs a C library (malloc): the browser's. Out of a network, so
 * everything read is checked; a stream that ends early reads as zeros,
 * and what makes no sense ends the decoding with an error.
 *
 * The result is four bytes a pixel, red, green, blue and alpha.
 */

#ifndef UWEBP_H
#define UWEBP_H

#include <stdlib.h>
#include <string.h>
#include "uvp8tab.h"

#ifndef WEBP_MAX_PIXELS
#define WEBP_MAX_PIXELS (4096 * 4096)  /* (a program may ask for less) */
#endif

typedef unsigned char webp_u8;
typedef unsigned int webp_u32;

/* ======================================================================
 * VP8: the lossy picture
 * ==================================================================== */

/* The boolean decoder of section 7: 'value' holds the next two bytes'
   worth of bits, 'range' the width of the interval */
struct vp8_bool {
    const webp_u8 *p, *end;
    webp_u32 value, range;
    int bit;
};

static int vp8_byte(struct vp8_bool *b)
{
    return b->p < b->end ? *b->p++ : 0;
}

static void vp8_bool_init(struct vp8_bool *b, const webp_u8 *p, size_t n)
{
    b->p = p;
    b->end = p + n;
    b->value = (webp_u32)vp8_byte(b) << 8;
    b->value |= (webp_u32)vp8_byte(b);
    b->range = 255;
    b->bit = 0;
}

static int vp8_get(struct vp8_bool *b, int prob)
{
    webp_u32 split = 1 + (((b->range - 1) * (webp_u32)prob) >> 8);
    webp_u32 big = split << 8;
    int r;
    if (b->value >= big) {
        r = 1;
        b->range -= split;
        b->value -= big;
    } else {
        r = 0;
        b->range = split;
    }
    while (b->range < 128) {
        b->value <<= 1;
        b->range <<= 1;
        if (++b->bit == 8) {
            b->bit = 0;
            b->value |= (webp_u32)vp8_byte(b);
        }
    }
    return r;
}

static int vp8_literal(struct vp8_bool *b, int n)
{
    int v = 0;
    while (n-- > 0)
        v = (v << 1) | vp8_get(b, 128);
    return v;
}

/* A magnitude and then its sign, as the frame header writes deltas */
static int vp8_signed(struct vp8_bool *b, int n)
{
    int v = vp8_literal(b, n);
    return vp8_get(b, 128) ? -v : v;
}

/* An optional one: a flag, then the value */
static int vp8_maybe(struct vp8_bool *b, int n)
{
    return vp8_get(b, 128) ? vp8_signed(b, n) : 0;
}

enum { VP8_DC, VP8_V, VP8_H, VP8_TM, VP8_B };          /* 16x16 modes */
enum { B_DC, B_TM, B_VE, B_HE, B_LD, B_RD, B_VR, B_VL, B_HD, B_HU };

/* What each macroblock leaves for the loop filter */
struct vp8_mb {
    webp_u8 ymode, segment, coded;     /* coded: it had any coefficients */
};

struct vp8_dec {
    int w, h, mbw, mbh;
    webp_u8 *y, *u, *v;                /* the planes, whole macroblocks */
    int ys, uvs;
    int seg_on, seg_map, seg_abs;
    int seg_q[4], seg_lf[4];
    int seg_prob[3];
    int simple, level, sharpness;
    int delta_on, ref_delta[4], mode_delta[4];
    int nparts;
    struct vp8_bool part[8];
    int q, ydc, y2dc, y2ac, uvdc, uvac;
    webp_u8 coef[4][8][3][11];
    int skip_on, skip_prob;
    struct vp8_mb *mb;
    webp_u8 *above_nz;                 /* 9 a column of macroblocks */
    webp_u8 *above_b;                  /* the subblock modes along the top */
    int dq[4][3][2];                   /* [segment][Y1, Y2, UV][DC, AC] */
};

static const webp_u8 vp8_zigzag[16] = {
    0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15
};
static const webp_u8 vp8_bands[17] = {
    0, 1, 2, 3, 6, 4, 5, 6, 6, 6, 6, 6, 6, 6, 6, 7, 0
};
static const webp_u8 vp8_pcat3[] = { 173, 148, 140, 0 };
static const webp_u8 vp8_pcat4[] = { 176, 155, 140, 135, 0 };
static const webp_u8 vp8_pcat5[] = { 180, 157, 141, 134, 130, 0 };
static const webp_u8 vp8_pcat6[] = { 254, 254, 243, 230, 196, 177, 153,
                                     140, 133, 130, 129, 0 };

static int vp8_extra(struct vp8_bool *b, const webp_u8 *p)
{
    int v = 0;
    while (*p)
        v = v + v + vp8_get(b, *p++);
    return v;
}

static int vp8_clamp_q(int q)
{
    return q < 0 ? 0 : q > 127 ? 127 : q;
}

/* One block's coefficients (section 13), dequantized into place: 1 when
   it had any tokens, which is what its neighbours' context counts */
static int vp8_block(struct vp8_bool *b, webp_u8 (*probs)[3][11], int ctx,
                     int first, int dc, int ac, short *out)
{
    int c = first;
    const webp_u8 *p = probs[vp8_bands[c]][ctx];
    if (!vp8_get(b, p[0]))
        return 0;                      /* end of block at once: empty */
    for (;;) {
        int v, next;
        if (!vp8_get(b, p[1])) {       /* a zero: no end of block after it */
            if (++c == 16)
                return 1;
            p = probs[vp8_bands[c]][0];
            continue;
        }
        if (!vp8_get(b, p[2])) {
            v = 1;
            next = 1;
        } else {
            if (!vp8_get(b, p[3])) {
                if (!vp8_get(b, p[4]))
                    v = 2;
                else
                    v = 3 + vp8_get(b, p[5]);
            } else if (!vp8_get(b, p[6])) {
                if (!vp8_get(b, p[7]))
                    v = 5 + vp8_get(b, 159);
                else {
                    v = 7 + 2 * vp8_get(b, 165);
                    v += vp8_get(b, 145);
                }
            } else if (!vp8_get(b, p[8])) {
                v = !vp8_get(b, p[9]) ? 11 + vp8_extra(b, vp8_pcat3)
                                      : 19 + vp8_extra(b, vp8_pcat4);
            } else {
                v = !vp8_get(b, p[10]) ? 35 + vp8_extra(b, vp8_pcat5)
                                       : 67 + vp8_extra(b, vp8_pcat6);
            }
            next = 2;
        }
        if (vp8_get(b, 128))
            v = -v;
        /* (stored in 16 bits, as the specification says: a wild stream
           wraps rather than overflows) */
        out[vp8_zigzag[c]] = (short)(v * (c > 0 ? ac : dc));
        if (++c == 16)
            return 1;
        p = probs[vp8_bands[c]][next];
        if (!vp8_get(b, p[0]))
            return 1;
    }
}

/* The inverse Walsh-Hadamard transform of section 14.3: the DCs of the
   sixteen luma blocks out of the Y2 block */
static void vp8_iwht(const short *in, short *dc16)
{
    int t[16];
    for (int i = 0; i < 4; i++) {
        int a1 = in[i] + in[12 + i], b1 = in[4 + i] + in[8 + i];
        int c1 = in[4 + i] - in[8 + i], d1 = in[i] - in[12 + i];
        t[i] = a1 + b1;
        t[4 + i] = c1 + d1;
        t[8 + i] = a1 - b1;
        t[12 + i] = d1 - c1;
    }
    for (int i = 0; i < 4; i++) {
        const int *r = t + 4 * i;
        int a1 = r[0] + r[3], b1 = r[1] + r[2];
        int c1 = r[1] - r[2], d1 = r[0] - r[3];
        dc16[4 * i + 0] = (short)((a1 + b1 + 3) >> 3);
        dc16[4 * i + 1] = (short)((c1 + d1 + 3) >> 3);
        dc16[4 * i + 2] = (short)((a1 - b1 + 3) >> 3);
        dc16[4 * i + 3] = (short)((d1 - c1 + 3) >> 3);
    }
}

static webp_u8 vp8_clip(int v)
{
    return (webp_u8)(v < 0 ? 0 : v > 255 ? 255 : v);
}

/* The inverse DCT of section 14.4, its result added to the prediction
   at 'dst' */
static void vp8_idct_add(const short *in, webp_u8 *dst, int stride)
{
    int t[16];
    for (int i = 0; i < 4; i++) {
        int a1 = in[i] + in[8 + i], b1 = in[i] - in[8 + i];
        int c1 = ((in[4 + i] * 35468) >> 16) - (in[12 + i] + ((in[12 + i] * 20091) >> 16));
        int d1 = (in[4 + i] + ((in[4 + i] * 20091) >> 16)) + ((in[12 + i] * 35468) >> 16);
        t[i] = a1 + d1;
        t[12 + i] = a1 - d1;
        t[4 + i] = b1 + c1;
        t[8 + i] = b1 - c1;
    }
    for (int i = 0; i < 4; i++) {
        const int *r = t + 4 * i;
        int a1 = r[0] + r[2], b1 = r[0] - r[2];
        int c1 = ((r[1] * 35468) >> 16) - (r[3] + ((r[3] * 20091) >> 16));
        int d1 = (r[1] + ((r[1] * 20091) >> 16)) + ((r[3] * 35468) >> 16);
        webp_u8 *o = dst + i * stride;
        o[0] = vp8_clip(o[0] + ((a1 + d1 + 4) >> 3));
        o[3] = vp8_clip(o[3] + ((a1 - d1 + 4) >> 3));
        o[1] = vp8_clip(o[1] + ((b1 + c1 + 4) >> 3));
        o[2] = vp8_clip(o[2] + ((b1 - c1 + 4) >> 3));
    }
}

/* --- prediction (section 12) ----------------------------------------- */

/* A whole block, 16x16 or 8x8, from the row above (A), the column to the
   left (L) and the corner (P). Edges of the frame: 127 above, 129 to
   the left, and DC averages only what is there. */
static void vp8_predict_block(webp_u8 *dst, int stride, int n, int mode,
                              const webp_u8 *A, const webp_u8 *L, int P,
                              int have_above, int have_left)
{
    if (mode == VP8_DC) {
        int sum = 0, shift = n == 16 ? 3 : 2, v = 128;
        if (have_above && have_left) {
            for (int i = 0; i < n; i++)
                sum += A[i] + L[i];
            v = (sum + n) >> (shift + 2);
        } else if (have_above || have_left) {
            const webp_u8 *s = have_above ? A : L;
            for (int i = 0; i < n; i++)
                sum += s[i];
            v = (sum + n / 2) >> (shift + 1);
        }
        for (int y = 0; y < n; y++)
            memset(dst + y * stride, v, (size_t)n);
    } else if (mode == VP8_V) {
        for (int y = 0; y < n; y++)
            memcpy(dst + y * stride, A, (size_t)n);
    } else if (mode == VP8_H) {
        for (int y = 0; y < n; y++)
            memset(dst + y * stride, L[y], (size_t)n);
    } else {
        for (int y = 0; y < n; y++)
            for (int x = 0; x < n; x++)
                dst[y * stride + x] = vp8_clip(L[y] + A[x] - P);
    }
}

#define AVG3(x, y, z) ((webp_u8)(((x) + 2 * (y) + (z) + 2) >> 2))
#define AVG2(x, y) ((webp_u8)(((x) + (y) + 1) >> 1))

/* A 4x4 subblock: A[0..7] above (and to the right), A[-1] the corner,
   L[0..3] to the left - exactly as in section 12.3 */
static void vp8_predict_sub(webp_u8 *d, int s, int mode, const webp_u8 *A,
                            const webp_u8 *L)
{
    webp_u8 B[4][4];
    webp_u8 E[9];
    int P = A[-1];
    E[0] = L[3]; E[1] = L[2]; E[2] = L[1]; E[3] = L[0];
    E[4] = (webp_u8)P;
    E[5] = A[0]; E[6] = A[1]; E[7] = A[2]; E[8] = A[3];
    switch (mode) {
    case B_DC: {
        int v = 4;
        for (int i = 0; i < 4; i++)
            v += A[i] + L[i];
        v >>= 3;
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++)
                B[r][c] = (webp_u8)v;
        break;
    }
    case B_TM:
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++)
                B[r][c] = vp8_clip(L[r] + A[c] - P);
        break;
    case B_VE:
        for (int c = 0; c < 4; c++)
            B[0][c] = B[1][c] = B[2][c] = B[3][c] = AVG3(A[c - 1], A[c], A[c + 1]);
        break;
    case B_HE: {
        webp_u8 l[4];
        l[0] = AVG3(P, L[0], L[1]);
        l[1] = AVG3(L[0], L[1], L[2]);
        l[2] = AVG3(L[1], L[2], L[3]);
        l[3] = AVG3(L[2], L[3], L[3]);
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++)
                B[r][c] = l[r];
        break;
    }
    case B_LD:
        B[0][0] = AVG3(A[0], A[1], A[2]);
        B[0][1] = B[1][0] = AVG3(A[1], A[2], A[3]);
        B[0][2] = B[1][1] = B[2][0] = AVG3(A[2], A[3], A[4]);
        B[0][3] = B[1][2] = B[2][1] = B[3][0] = AVG3(A[3], A[4], A[5]);
        B[1][3] = B[2][2] = B[3][1] = AVG3(A[4], A[5], A[6]);
        B[2][3] = B[3][2] = AVG3(A[5], A[6], A[7]);
        B[3][3] = AVG3(A[6], A[7], A[7]);
        break;
    case B_RD:
        B[3][0] = AVG3(E[0], E[1], E[2]);
        B[3][1] = B[2][0] = AVG3(E[1], E[2], E[3]);
        B[3][2] = B[2][1] = B[1][0] = AVG3(E[2], E[3], E[4]);
        B[3][3] = B[2][2] = B[1][1] = B[0][0] = AVG3(E[3], E[4], E[5]);
        B[2][3] = B[1][2] = B[0][1] = AVG3(E[4], E[5], E[6]);
        B[1][3] = B[0][2] = AVG3(E[5], E[6], E[7]);
        B[0][3] = AVG3(E[6], E[7], E[8]);
        break;
    case B_VR:
        B[3][0] = AVG3(E[1], E[2], E[3]);
        B[2][0] = AVG3(E[2], E[3], E[4]);
        B[3][1] = B[1][0] = AVG3(E[3], E[4], E[5]);
        B[2][1] = B[0][0] = AVG2(E[4], E[5]);
        B[3][2] = B[1][1] = AVG3(E[4], E[5], E[6]);
        B[2][2] = B[0][1] = AVG2(E[5], E[6]);
        B[3][3] = B[1][2] = AVG3(E[5], E[6], E[7]);
        B[2][3] = B[0][2] = AVG2(E[6], E[7]);
        B[1][3] = AVG3(E[6], E[7], E[8]);
        B[0][3] = AVG2(E[7], E[8]);
        break;
    case B_VL:
        B[0][0] = AVG2(A[0], A[1]);
        B[1][0] = AVG3(A[0], A[1], A[2]);
        B[2][0] = B[0][1] = AVG2(A[1], A[2]);
        B[1][1] = B[3][0] = AVG3(A[1], A[2], A[3]);
        B[2][1] = B[0][2] = AVG2(A[2], A[3]);
        B[3][1] = B[1][2] = AVG3(A[2], A[3], A[4]);
        B[2][2] = B[0][3] = AVG2(A[3], A[4]);
        B[3][2] = B[1][3] = AVG3(A[3], A[4], A[5]);
        B[2][3] = AVG3(A[4], A[5], A[6]);
        B[3][3] = AVG3(A[5], A[6], A[7]);
        break;
    case B_HD:
        B[3][0] = AVG2(E[0], E[1]);
        B[3][1] = AVG3(E[0], E[1], E[2]);
        B[2][0] = B[3][2] = AVG2(E[1], E[2]);
        B[2][1] = B[3][3] = AVG3(E[1], E[2], E[3]);
        B[2][2] = B[1][0] = AVG2(E[2], E[3]);
        B[2][3] = B[1][1] = AVG3(E[2], E[3], E[4]);
        B[1][2] = B[0][0] = AVG2(E[3], E[4]);
        B[1][3] = B[0][1] = AVG3(E[3], E[4], E[5]);
        B[0][2] = AVG3(E[4], E[5], E[6]);
        B[0][3] = AVG3(E[5], E[6], E[7]);
        break;
    default:                           /* B_HU */
        B[0][0] = AVG2(L[0], L[1]);
        B[0][1] = AVG3(L[0], L[1], L[2]);
        B[0][2] = B[1][0] = AVG2(L[1], L[2]);
        B[0][3] = B[1][1] = AVG3(L[1], L[2], L[3]);
        B[1][2] = B[2][0] = AVG2(L[2], L[3]);
        B[1][3] = B[2][1] = AVG3(L[2], L[3], L[3]);
        B[2][2] = B[2][3] = B[3][0] = B[3][1] = B[3][2] = B[3][3] = L[3];
        break;
    }
    for (int r = 0; r < 4; r++)
        memcpy(d + r * s, B[r], 4);
}

/* The row above a block and its corner, the column to its left: from the
   plane, or what the edges of the frame stand for */
static void vp8_edges(const webp_u8 *plane, int stride, int x0, int y0,
                      int n, int extra, int plane_w, webp_u8 *A, webp_u8 *L)
{
    /* A[-1] is the corner; A[0..n+extra-1] the row above */
    if (y0 == 0) {
        memset(A - 1, 127, (size_t)(n + extra + 1));
    } else {
        const webp_u8 *row = plane + (size_t)(y0 - 1) * stride;
        A[-1] = x0 > 0 ? row[x0 - 1] : 129;
        for (int i = 0; i < n + extra; i++)
            A[i] = x0 + i < plane_w ? row[x0 + i] : row[plane_w - 1];
    }
    for (int i = 0; i < n; i++)
        L[i] = x0 > 0 ? plane[(size_t)(y0 + i) * stride + x0 - 1] : 129;
}

static int vp8_tree_ymode(struct vp8_bool *b)
{
    if (!vp8_get(b, 145))
        return VP8_B;
    if (!vp8_get(b, 156))
        return vp8_get(b, 163) ? VP8_V : VP8_DC;
    return vp8_get(b, 128) ? VP8_TM : VP8_H;
}

static int vp8_tree_bmode(struct vp8_bool *b, const webp_u8 *p)
{
    if (!vp8_get(b, p[0])) return B_DC;
    if (!vp8_get(b, p[1])) return B_TM;
    if (!vp8_get(b, p[2])) return B_VE;
    if (!vp8_get(b, p[3])) {
        if (!vp8_get(b, p[4])) return B_HE;
        return vp8_get(b, p[5]) ? B_VR : B_RD;
    }
    if (!vp8_get(b, p[6])) return B_LD;
    if (!vp8_get(b, p[7])) return B_VL;
    return vp8_get(b, p[8]) ? B_HU : B_HD;
}

static int vp8_tree_uvmode(struct vp8_bool *b)
{
    if (!vp8_get(b, 142)) return VP8_DC;
    if (!vp8_get(b, 114)) return VP8_V;
    return vp8_get(b, 183) ? VP8_TM : VP8_H;
}

/* One macroblock: its modes from the first partition, its coefficients
   from its row's, then prediction and residue */
static void vp8_macroblock(struct vp8_dec *d, struct vp8_bool *hdr,
                           struct vp8_bool *tok, int mx, int my,
                           webp_u8 *left_nz, webp_u8 *left_b)
{
    struct vp8_mb *m = &d->mb[my * d->mbw + mx];
    int seg = 0;
    if (d->seg_map)
        seg = !vp8_get(hdr, d->seg_prob[0]) ? vp8_get(hdr, d->seg_prob[1])
                                            : 2 + vp8_get(hdr, d->seg_prob[2]);
    int skip = d->skip_on ? vp8_get(hdr, d->skip_prob) : 0;
    int ymode = vp8_tree_ymode(hdr);
    webp_u8 bmodes[16];
    webp_u8 *above_b = d->above_b + mx * 4;
    if (ymode == VP8_B) {
        for (int i = 0; i < 16; i++) {
            int a = i < 4 ? above_b[i] : bmodes[i - 4];
            int l = i % 4 == 0 ? left_b[i / 4] : bmodes[i - 1];
            bmodes[i] = (webp_u8)vp8_tree_bmode(hdr, vp8_kf_bmode_prob[a][l]);
        }
        for (int i = 0; i < 4; i++) {
            above_b[i] = bmodes[12 + i];
            left_b[i] = bmodes[4 * i + 3];
        }
    } else {
        /* What a whole-block mode counts as for its neighbours' contexts */
        int implied = ymode == VP8_DC ? B_DC : ymode == VP8_V ? B_VE
                    : ymode == VP8_H ? B_HE : B_TM;
        memset(above_b, implied, 4);
        memset(left_b, implied, 4);
    }
    int uvmode = vp8_tree_uvmode(hdr);

    /* The coefficients: Y2 (when the mode has one), 16 Y, 4 U, 4 V */
    static short coeffs[25][16];
    memset(coeffs, 0, sizeof(coeffs));
    webp_u8 *above_nz = d->above_nz + mx * 9;
    const int *dq1 = d->dq[seg][0], *dq2 = d->dq[seg][1], *dquv = d->dq[seg][2];
    int has_y2 = ymode != VP8_B, coded = 0;
    if (!skip) {
        int first = 0;
        if (has_y2) {
            int nz = vp8_block(tok, d->coef[1], left_nz[8] + above_nz[8], 0,
                               dq2[0], dq2[1], coeffs[24]);
            left_nz[8] = above_nz[8] = (webp_u8)nz;
            coded |= nz;
            first = 1;
        }
        for (int i = 0; i < 16; i++) {
            int by = i / 4, bx = i % 4;
            int nz = vp8_block(tok, d->coef[has_y2 ? 0 : 3],
                               left_nz[by] + above_nz[bx], first, dq1[0], dq1[1],
                               coeffs[i]);
            left_nz[by] = above_nz[bx] = (webp_u8)nz;
            coded |= nz;
        }
        for (int i = 0; i < 8; i++) {
            int k = i < 4 ? 4 : 6, by = (i & 3) / 2, bx = i & 1;
            int nz = vp8_block(tok, d->coef[2], left_nz[k + by] + above_nz[k + bx],
                               0, dquv[0], dquv[1], coeffs[16 + i]);
            left_nz[k + by] = above_nz[k + bx] = (webp_u8)nz;
            coded |= nz;
        }
    } else {
        /* Nothing coded: the contexts empty - but Y2's only where this
           macroblock would have had one */
        memset(left_nz, 0, 8);
        memset(above_nz, 0, 8);
        if (has_y2)
            left_nz[8] = above_nz[8] = 0;
    }
    if (has_y2) {
        short dc[16];
        vp8_iwht(coeffs[24], dc);
        for (int i = 0; i < 16; i++)
            coeffs[i][0] = dc[i];
    }
    m->ymode = (webp_u8)ymode;
    m->segment = (webp_u8)seg;
    m->coded = (webp_u8)coded;

    /* Luma */
    int x0 = mx * 16, y0 = my * 16;
    webp_u8 abuf[1 + 16 + 4], L[16];
    webp_u8 *A = abuf + 1;
    vp8_edges(d->y, d->ys, x0, y0, 16, 4, d->ys, A, L);
    webp_u8 *dst = d->y + (size_t)y0 * d->ys + x0;
    if (ymode != VP8_B) {
        vp8_predict_block(dst, d->ys, 16, ymode, A, L, A[-1], y0 > 0, x0 > 0);
        for (int i = 0; i < 16; i++)
            vp8_idct_add(coeffs[i], dst + (i / 4) * 4 * d->ys + (i % 4) * 4, d->ys);
    } else {
        /* The rightmost subblocks all take the four pixels above and to
           the right of the macroblock: those to their right in rows 1-3
           are not decoded yet (section 12.3) */
        for (int i = 0; i < 16; i++) {
            int r = i / 4, c = i % 4;
            webp_u8 a[9], l[4];
            webp_u8 *sd = dst + r * 4 * d->ys + c * 4;
            if (r == 0) {
                memcpy(a, A + c * 4 - 1, 9);
            } else {
                const webp_u8 *row = sd - d->ys;
                a[0] = c > 0 ? row[-1] : L[r * 4 - 1];
                memcpy(a + 1, row, 4);
                if (c < 3)
                    memcpy(a + 5, row + 4, 4);
                else
                    memcpy(a + 5, A + 16, 4);
            }
            for (int k = 0; k < 4; k++)
                l[k] = c > 0 ? sd[k * d->ys - 1] : L[r * 4 + k];
            vp8_predict_sub(sd, d->ys, bmodes[i], a + 1, l);
            vp8_idct_add(coeffs[i], sd, d->ys);
        }
    }

    /* Chroma */
    for (int k = 0; k < 2; k++) {
        webp_u8 *plane = k ? d->v : d->u;
        webp_u8 cab[1 + 8], cl[8];
        webp_u8 *ca = cab + 1;
        int cx = mx * 8, cy = my * 8;
        vp8_edges(plane, d->uvs, cx, cy, 8, 0, d->uvs, ca, cl);
        webp_u8 *cd = plane + (size_t)cy * d->uvs + cx;
        vp8_predict_block(cd, d->uvs, 8, uvmode, ca, cl, ca[-1], cy > 0, cx > 0);
        for (int i = 0; i < 4; i++)
            vp8_idct_add(coeffs[16 + k * 4 + i], cd + (i / 2) * 4 * d->uvs + (i % 2) * 4,
                         d->uvs);
    }
}

/* --- the loop filter (section 15) ------------------------------------- */

static int vp8_s8(int v)
{
    return v < -128 ? -128 : v > 127 ? 127 : v;
}

#define VP8_ABS(x) ((x) < 0 ? -(x) : (x))

/* p[-4*s] .. p[3*s]: p3 p2 p1 p0 | q0 q1 q2 q3 */
static int vp8_simple_ok(const webp_u8 *p, int s, int limit)
{
    return VP8_ABS(p[-s] - p[0]) * 2 + (VP8_ABS(p[-2 * s] - p[s]) >> 1) <= limit;
}

static int vp8_normal_ok(const webp_u8 *p, int s, int e, int i)
{
    return vp8_simple_ok(p, s, 2 * e + i) &&
           VP8_ABS(p[-4 * s] - p[-3 * s]) <= i && VP8_ABS(p[-3 * s] - p[-2 * s]) <= i &&
           VP8_ABS(p[-2 * s] - p[-s]) <= i && VP8_ABS(p[3 * s] - p[2 * s]) <= i &&
           VP8_ABS(p[2 * s] - p[s]) <= i && VP8_ABS(p[s] - p[0]) <= i;
}

static int vp8_hev(const webp_u8 *p, int s, int t)
{
    return VP8_ABS(p[-2 * s] - p[-s]) > t || VP8_ABS(p[s] - p[0]) > t;
}

static void vp8_common(webp_u8 *p, int s, int outer)
{
    int p1 = p[-2 * s], p0 = p[-s], q0 = p[0], q1 = p[s];
    int a = 3 * (q0 - p0);
    if (outer)
        a += vp8_s8(p1 - q1);
    a = vp8_s8(a);
    int f1 = (a + 4 > 127 ? 127 : a + 4) >> 3;
    int f2 = (a + 3 > 127 ? 127 : a + 3) >> 3;
    p[-s] = vp8_clip(p0 + f2);
    p[0] = vp8_clip(q0 - f1);
    if (!outer) {
        a = (f1 + 1) >> 1;
        p[-2 * s] = vp8_clip(p1 + a);
        p[s] = vp8_clip(q1 - a);
    }
}

static void vp8_mbedge(webp_u8 *p, int s)
{
    int p2 = p[-3 * s], p1 = p[-2 * s], p0 = p[-s];
    int q0 = p[0], q1 = p[s], q2 = p[2 * s];
    int w = vp8_s8(vp8_s8(p1 - q1) + 3 * (q0 - p0));
    int a = (27 * w + 63) >> 7;
    p[-s] = vp8_clip(p0 + a);
    p[0] = vp8_clip(q0 - a);
    a = (18 * w + 63) >> 7;
    p[-2 * s] = vp8_clip(p1 + a);
    p[s] = vp8_clip(q1 - a);
    a = (9 * w + 63) >> 7;
    p[-3 * s] = vp8_clip(p2 + a);
    p[2 * s] = vp8_clip(q2 - a);
}

/* An edge 'n' pixels long: 'step' along it, 's' across it */
static void vp8_edge(webp_u8 *p, int step, int s, int n, int mb, int e, int i,
                     int hev)
{
    for (int k = 0; k < n; k++, p += step) {
        if (!vp8_normal_ok(p, s, e, i))
            continue;
        if (mb) {
            if (vp8_hev(p, s, hev))
                vp8_common(p, s, 1);
            else
                vp8_mbedge(p, s);
        } else {
            vp8_common(p, s, vp8_hev(p, s, hev));
        }
    }
}

static void vp8_edge_simple(webp_u8 *p, int step, int s, int limit)
{
    for (int k = 0; k < 16; k++, p += step)
        if (vp8_simple_ok(p, s, limit))
            vp8_common(p, s, 1);
}

static void vp8_loop_filter(struct vp8_dec *d)
{
    for (int my = 0; my < d->mbh; my++)
        for (int mx = 0; mx < d->mbw; mx++) {
            const struct vp8_mb *m = &d->mb[my * d->mbw + mx];
            int level = d->level;
            if (d->seg_on)
                level = d->seg_abs ? d->seg_lf[m->segment] : level + d->seg_lf[m->segment];
            level = level < 0 ? 0 : level > 63 ? 63 : level;
            if (d->delta_on) {
                level += d->ref_delta[0];      /* (a key frame: intra) */
                if (m->ymode == VP8_B)
                    level += d->mode_delta[0];
            }
            level = level < 0 ? 0 : level > 63 ? 63 : level;
            if (!level)
                continue;
            int interior = level;
            if (d->sharpness) {
                interior >>= d->sharpness > 4 ? 2 : 1;
                if (interior > 9 - d->sharpness)
                    interior = 9 - d->sharpness;
            }
            if (interior < 1)
                interior = 1;
            int hev = (level >= 15) + (level >= 40);
            int inner = m->coded || m->ymode == VP8_B;
            webp_u8 *y = d->y + (size_t)my * 16 * d->ys + mx * 16;
            webp_u8 *u = d->u + (size_t)my * 8 * d->uvs + mx * 8;
            webp_u8 *v = d->v + (size_t)my * 8 * d->uvs + mx * 8;
            int ys = d->ys, cs = d->uvs;
            if (d->simple) {
                int mbl = (level + 2) * 2 + interior, bl = level * 2 + interior;
                if (mx)
                    vp8_edge_simple(y, ys, 1, mbl);
                if (inner)
                    for (int k = 4; k < 16; k += 4)
                        vp8_edge_simple(y + k, ys, 1, bl);
                if (my)
                    vp8_edge_simple(y, 1, ys, mbl);
                if (inner)
                    for (int k = 4; k < 16; k += 4)
                        vp8_edge_simple(y + k * ys, 1, ys, bl);
                continue;
            }
            if (mx) {
                vp8_edge(y, ys, 1, 16, 1, level + 2, interior, hev);
                vp8_edge(u, cs, 1, 8, 1, level + 2, interior, hev);
                vp8_edge(v, cs, 1, 8, 1, level + 2, interior, hev);
            }
            if (inner) {
                for (int k = 4; k < 16; k += 4)
                    vp8_edge(y + k, ys, 1, 16, 0, level, interior, hev);
                vp8_edge(u + 4, cs, 1, 8, 0, level, interior, hev);
                vp8_edge(v + 4, cs, 1, 8, 0, level, interior, hev);
            }
            if (my) {
                vp8_edge(y, 1, ys, 16, 1, level + 2, interior, hev);
                vp8_edge(u, 1, cs, 8, 1, level + 2, interior, hev);
                vp8_edge(v, 1, cs, 8, 1, level + 2, interior, hev);
            }
            if (inner) {
                for (int k = 4; k < 16; k += 4)
                    vp8_edge(y + k * ys, 1, ys, 16, 0, level, interior, hev);
                vp8_edge(u + 4 * cs, 1, cs, 8, 0, level, interior, hev);
                vp8_edge(v + 4 * cs, 1, cs, 8, 0, level, interior, hev);
            }
        }
}

/* --- to RGB ------------------------------------------------------------ */

/* Rec. 601 in libwebp's fixed point: 14 bits */
static int vp8_mulhi(int v, int k)
{
    return (v * k) >> 8;
}

static webp_u8 vp8_clip6(int v)
{
    return (webp_u8)((v & ~16383) == 0 ? v >> 6 : v < 0 ? 0 : 255);
}

/* A chroma value at a full-size pixel: the four samples around it,
   9-3-3-1, the way libwebp's "fancy" upsampler rounds it */
static int vp8_chroma(const webp_u8 *plane, int stride, int cw, int ch, int x,
                      int y)
{
    /* (a chroma sample sits between two luma pixels: the nearest of them
       is at x / 2, the other one to the side x is on) */
    int nx = x >> 1, ny = y >> 1;
    int fx = x & 1 ? nx + 1 : nx - 1, fy = y & 1 ? ny + 1 : ny - 1;
    if (fx < 0) fx = 0;
    if (fy < 0) fy = 0;
    if (nx >= cw) nx = cw - 1;
    if (fx >= cw) fx = cw - 1;
    if (ny >= ch) ny = ch - 1;
    if (fy >= ch) fy = ch - 1;
    int a = plane[ny * stride + nx], b = plane[ny * stride + fx];
    int c = plane[fy * stride + nx], dd = plane[fy * stride + fx];
    int avg = a + b + c + dd + 8;
    int diag = (avg + 2 * (b + c)) >> 3;
    return (diag + a) >> 1;
}

static void vp8_to_rgba(const struct vp8_dec *d, webp_u8 *out)
{
    int cw = (d->w + 1) / 2, ch = (d->h + 1) / 2;
    for (int y = 0; y < d->h; y++)
        for (int x = 0; x < d->w; x++) {
            int Y = d->y[(size_t)y * d->ys + x];
            int U = vp8_chroma(d->u, d->uvs, cw, ch, x, y);
            int V = vp8_chroma(d->v, d->uvs, cw, ch, x, y);
            webp_u8 *o = out + ((size_t)y * d->w + x) * 4;
            int yy = vp8_mulhi(Y, 19077);
            o[0] = vp8_clip6(yy + vp8_mulhi(V, 26149) - 14234);
            o[1] = vp8_clip6(yy - vp8_mulhi(U, 6419) - vp8_mulhi(V, 13320) + 8708);
            o[2] = vp8_clip6(yy + vp8_mulhi(U, 33050) - 17685);
            o[3] = 255;
        }
}

static void vp8_free(struct vp8_dec *d)
{
    free(d->y);
    free(d->u);
    free(d->v);
    free(d->mb);
    free(d->above_nz);
    free(d->above_b);
}

/* A VP8 key frame into RGBA (w x h, alpha 255). 0, or -1 with 'why'. */
static int vp8_decode(const webp_u8 *data, size_t len, webp_u8 **rgba, int *ow,
                      int *oh, const char **why)
{
    struct vp8_dec d;
    memset(&d, 0, sizeof(d));
    *why = "not a VP8 key frame";
    if (len < 10)
        return -1;
    webp_u32 tag = data[0] | (webp_u32)data[1] << 8 | (webp_u32)data[2] << 16;
    size_t first = tag >> 5;
    if ((tag & 1) || data[3] != 0x9d || data[4] != 0x01 || data[5] != 0x2a)
        return -1;
    d.w = (data[6] | data[7] << 8) & 0x3FFF;
    d.h = (data[8] | data[9] << 8) & 0x3FFF;
    *why = "a size out of range";
    if (!d.w || !d.h || (long)d.w * d.h > WEBP_MAX_PIXELS)
        return -1;
    *why = "cut short";
    data += 10;
    len -= 10;
    if (first > len)
        return -1;
    d.mbw = (d.w + 15) / 16;
    d.mbh = (d.h + 15) / 16;
    d.ys = d.mbw * 16;
    d.uvs = d.mbw * 8;
    *why = "out of memory";
    d.y = (webp_u8 *)malloc((size_t)d.ys * d.mbh * 16);
    d.u = (webp_u8 *)malloc((size_t)d.uvs * d.mbh * 8);
    d.v = (webp_u8 *)malloc((size_t)d.uvs * d.mbh * 8);
    d.mb = (struct vp8_mb *)calloc((size_t)d.mbw * d.mbh, sizeof(struct vp8_mb));
    d.above_nz = (webp_u8 *)calloc((size_t)d.mbw, 9);
    d.above_b = (webp_u8 *)calloc((size_t)d.mbw, 4);
    *rgba = (webp_u8 *)malloc((size_t)d.w * d.h * 4);
    if (!d.y || !d.u || !d.v || !d.mb || !d.above_nz || !d.above_b || !*rgba) {
        vp8_free(&d);
        free(*rgba);
        *rgba = 0;
        return -1;
    }

    /* The frame header (section 9, 19.2) */
    struct vp8_bool hb;
    vp8_bool_init(&hb, data, first);
    vp8_literal(&hb, 2);               /* colour space, clamping */
    d.seg_on = vp8_get(&hb, 128);
    for (int i = 0; i < 3; i++)
        d.seg_prob[i] = 255;
    if (d.seg_on) {
        d.seg_map = vp8_get(&hb, 128);
        if (vp8_get(&hb, 128)) {       /* the segments' data */
            d.seg_abs = vp8_get(&hb, 128);
            for (int i = 0; i < 4; i++)
                d.seg_q[i] = vp8_maybe(&hb, 7);
            for (int i = 0; i < 4; i++)
                d.seg_lf[i] = vp8_maybe(&hb, 6);
        }
        if (d.seg_map)
            for (int i = 0; i < 3; i++)
                d.seg_prob[i] = vp8_get(&hb, 128) ? vp8_literal(&hb, 8) : 255;
    }
    d.simple = vp8_get(&hb, 128);
    d.level = vp8_literal(&hb, 6);
    d.sharpness = vp8_literal(&hb, 3);
    d.delta_on = vp8_get(&hb, 128);
    if (d.delta_on && vp8_get(&hb, 128)) {
        for (int i = 0; i < 4; i++)
            d.ref_delta[i] = vp8_maybe(&hb, 6);
        for (int i = 0; i < 4; i++)
            d.mode_delta[i] = vp8_maybe(&hb, 6);
    }
    d.nparts = 1 << vp8_literal(&hb, 2);
    d.q = vp8_literal(&hb, 7);
    d.ydc = vp8_maybe(&hb, 4);
    d.y2dc = vp8_maybe(&hb, 4);
    d.y2ac = vp8_maybe(&hb, 4);
    d.uvdc = vp8_maybe(&hb, 4);
    d.uvac = vp8_maybe(&hb, 4);
    vp8_get(&hb, 128);                 /* refresh_entropy_probs: one frame */
    memcpy(d.coef, vp8_default_coeff_probs, sizeof(d.coef));
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 8; j++)
            for (int k = 0; k < 3; k++)
                for (int t = 0; t < 11; t++)
                    if (vp8_get(&hb, vp8_coeff_update_probs[i][j][k][t]))
                        d.coef[i][j][k][t] = (webp_u8)vp8_literal(&hb, 8);
    d.skip_on = vp8_get(&hb, 128);
    if (d.skip_on)
        d.skip_prob = vp8_literal(&hb, 8);

    /* The token partitions: their sizes, three bytes each, after the
       first; the last takes what is left */
    const webp_u8 *pp = data + first;
    size_t left = len - first, sizes = (size_t)(d.nparts - 1) * 3;
    if (sizes > left) {
        vp8_free(&d);
        free(*rgba);
        *rgba = 0;
        return -1;
    }
    const webp_u8 *pd = pp + sizes;
    left -= sizes;
    for (int i = 0; i < d.nparts; i++) {
        size_t n = left;
        if (i < d.nparts - 1) {
            n = pp[i * 3] | (size_t)pp[i * 3 + 1] << 8 | (size_t)pp[i * 3 + 2] << 16;
            if (n > left)
                n = left;
        }
        vp8_bool_init(&d.part[i], pd, n);
        pd += n;
        left -= n;
    }

    /* The quantizers of each segment (section 14.1, dixie.c) */
    for (int s = 0; s < 4; s++) {
        int q = d.q;
        if (d.seg_on)
            q = d.seg_abs ? d.seg_q[s] : q + d.seg_q[s];
        d.dq[s][0][0] = vp8_dc_qlookup[vp8_clamp_q(q + d.ydc)];
        d.dq[s][0][1] = vp8_ac_qlookup[vp8_clamp_q(q)];
        d.dq[s][1][0] = vp8_dc_qlookup[vp8_clamp_q(q + d.y2dc)] * 2;
        d.dq[s][1][1] = vp8_ac_qlookup[vp8_clamp_q(q + d.y2ac)] * 155 / 100;
        if (d.dq[s][1][1] < 8)
            d.dq[s][1][1] = 8;
        d.dq[s][2][0] = vp8_dc_qlookup[vp8_clamp_q(q + d.uvdc)];
        if (d.dq[s][2][0] > 132)
            d.dq[s][2][0] = 132;
        d.dq[s][2][1] = vp8_ac_qlookup[vp8_clamp_q(q + d.uvac)];
    }

    for (int my = 0; my < d.mbh; my++) {
        webp_u8 left_nz[9] = {0}, left_b[4] = {0};
        for (int mx = 0; mx < d.mbw; mx++)
            vp8_macroblock(&d, &hb, &d.part[my % d.nparts], mx, my, left_nz, left_b);
    }
    vp8_loop_filter(&d);
    vp8_to_rgba(&d, *rgba);
    *ow = d.w;
    *oh = d.h;
    vp8_free(&d);
    return 0;
}

/* ======================================================================
 * VP8L: the lossless picture
 * ==================================================================== */

struct vp8l_br {
    const webp_u8 *p;
    size_t n, pos;
    unsigned long long val;
    int bits;
    int pad;                           /* zeros put in past the end */
};

/* Whether more was read than there is: the zeros the buffer was filled
   with past the end have reached its bottom */
static int vp8l_eos(const struct vp8l_br *b)
{
    return b->bits < b->pad;
}

static void vp8l_fill(struct vp8l_br *b)
{
    while (b->bits <= 56) {
        if (b->pos < b->n)
            b->val |= (unsigned long long)b->p[b->pos++] << b->bits;
        else
            b->pad += 8;
        b->bits += 8;
    }
}

static webp_u32 vp8l_read(struct vp8l_br *b, int n)
{
    if (b->bits < n)
        vp8l_fill(b);
    webp_u32 v = (webp_u32)(b->val & ((1ULL << n) - 1));
    b->val >>= n;
    b->bits -= n;
    return v;
}

/* A canonical prefix code: a table of 8 bits for the short codes, and
   for the long ones a walk through the counts, as in DEFLATE */
#define VP8L_LUT 8

struct vp8l_code {
    unsigned short lut[1 << VP8L_LUT];  /* symbol << 4 | length; 0: long */
    unsigned short count[16];
    unsigned short *sym;                /* by code, for the long walk */
    int single;                         /* the one symbol, or -1 */
};

static void vp8l_code_free(struct vp8l_code *c)
{
    free(c->sym);
    c->sym = 0;
}

static int vp8l_build(struct vp8l_code *c, const webp_u8 *len, int n)
{
    int nz = 0, last = 0;
    memset(c->count, 0, sizeof(c->count));
    memset(c->lut, 0, sizeof(c->lut));
    c->sym = 0;
    c->single = -1;
    for (int i = 0; i < n; i++)
        if (len[i]) {
            c->count[len[i]]++;
            nz++;
            last = i;
        }
    if (nz == 0)
        return -1;
    if (nz == 1) {                     /* one symbol: no bits at all */
        c->single = last;
        return 0;
    }
    int room = 1;                      /* the code must fill the tree */
    for (int l = 1; l < 16; l++) {
        room = room * 2 - c->count[l];
        if (room < 0)
            return -1;
    }
    if (room != 0)
        return -1;
    unsigned short offs[16];
    offs[1] = 0;
    for (int l = 1; l < 15; l++)
        offs[l + 1] = (unsigned short)(offs[l] + c->count[l]);
    c->sym = (unsigned short *)malloc(sizeof(unsigned short) * (size_t)nz);
    if (!c->sym)
        return -1;
    for (int i = 0; i < n; i++)
        if (len[i])
            c->sym[offs[len[i]]++] = (unsigned short)i;
    /* The short codes into the table, bit-reversed: the stream gives a
       code's first bit first, in its low bits */
    int code = 0, k = 0;
    for (int l = 1; l <= VP8L_LUT; l++) {
        for (int i = 0; i < c->count[l]; i++, k++, code++) {
            int rev = 0;
            for (int b = 0; b < l; b++)
                rev |= ((code >> b) & 1) << (l - 1 - b);
            for (int f = rev; f < (1 << VP8L_LUT); f += 1 << l)
                c->lut[f] = (unsigned short)(c->sym[k] << 4 | l);
        }
        code <<= 1;
    }
    return 0;
}

static int vp8l_decode(struct vp8l_br *b, const struct vp8l_code *c)
{
    if (c->single >= 0)
        return c->single;
    if (b->bits < 15)
        vp8l_fill(b);
    unsigned short e = c->lut[b->val & ((1 << VP8L_LUT) - 1)];
    if (e) {
        b->val >>= e & 15;
        b->bits -= e & 15;
        return e >> 4;
    }
    int code = 0, first = 0, index = 0;
    for (int l = 1; l < 16; l++) {
        code |= (int)vp8l_read(b, 1);
        int cnt = c->count[l];
        if (code - cnt < first)
            return c->sym[index + (code - first)];
        index += cnt;
        first = (first + cnt) << 1;
        code <<= 1;
    }
    return 0;                          /* (a complete code cannot get here) */
}

/* The code lengths of a prefix code (section 3.7.2.1), and the code */
static int vp8l_read_code(struct vp8l_br *b, struct vp8l_code *c, int alphabet)
{
    static webp_u8 len[256 + 24 + 2048];
    memset(len, 0, (size_t)alphabet);
    if (vp8l_read(b, 1)) {             /* simple: one or two symbols */
        int n = (int)vp8l_read(b, 1) + 1;
        int s0 = (int)vp8l_read(b, vp8l_read(b, 1) ? 8 : 1);
        if (s0 >= alphabet)
            return -1;
        len[s0] = 1;
        if (n == 2) {
            int s1 = (int)vp8l_read(b, 8);
            if (s1 >= alphabet)
                return -1;
            len[s1] = 1;
        }
    } else {
        static const webp_u8 order[19] = { 17, 18, 0, 1, 2, 3, 4, 5, 16, 6, 7,
                                           8, 9, 10, 11, 12, 13, 14, 15 };
        webp_u8 cl[19];
        memset(cl, 0, sizeof(cl));
        int ncl = 4 + (int)vp8l_read(b, 4);
        for (int i = 0; i < ncl; i++)
            cl[order[i]] = (webp_u8)vp8l_read(b, 3);
        struct vp8l_code lc;
        if (vp8l_build(&lc, cl, 19) < 0)
            return -1;
        int max = alphabet;
        if (vp8l_read(b, 1)) {
            int nbits = 2 + 2 * (int)vp8l_read(b, 3);
            max = 2 + (int)vp8l_read(b, nbits);
            if (max > alphabet) {
                vp8l_code_free(&lc);
                return -1;
            }
        }
        int sym = 0, prev = 8;
        while (sym < alphabet) {
            if (max-- == 0)
                break;
            int v = vp8l_decode(b, &lc);
            if (v < 16) {
                len[sym++] = (webp_u8)v;
                if (v)
                    prev = v;
                continue;
            }
            int extra = v == 16 ? 2 : v == 17 ? 3 : 7;
            int rep = (int)vp8l_read(b, extra) + (v == 18 ? 11 : 3);
            if (sym + rep > alphabet) {
                vp8l_code_free(&lc);
                return -1;
            }
            while (rep--)
                len[sym++] = (webp_u8)(v == 16 ? prev : 0);
        }
        vp8l_code_free(&lc);
    }
    return vp8l_eos(b) ? -1 : vp8l_build(c, len, alphabet);
}

struct vp8l_transform {
    int type, bits;
    int xsize;                         /* the width of the picture it undoes
                                          into: a palette's before packing */
    webp_u32 *data;                    /* its subresolution image, or palette */
    int ncolours;
};

struct vp8l {
    struct vp8l_br br;
    struct vp8l_transform tr[4];
    int ntr;
};

static int vp8l_div(int n, int bits)
{
    return (n + (1 << bits) - 1) >> bits;
}

static int vp8l_prefix_value(struct vp8l_br *b, int code)
{
    if (code < 4)
        return code + 1;
    int extra = (code - 2) >> 1;
    int offset = (2 + (code & 1)) << extra;
    return offset + (int)vp8l_read(b, extra) + 1;
}

static const signed char vp8l_dist_map[120][2] = {
    {0, 1}, {1, 0}, {1, 1}, {-1, 1}, {0, 2}, {2, 0}, {1, 2}, {-1, 2}, {2, 1}, {-2, 1},
    {2, 2}, {-2, 2}, {0, 3}, {3, 0}, {1, 3}, {-1, 3}, {3, 1}, {-3, 1}, {2, 3}, {-2, 3},
    {3, 2}, {-3, 2}, {0, 4}, {4, 0}, {1, 4}, {-1, 4}, {4, 1}, {-4, 1}, {3, 3}, {-3, 3},
    {2, 4}, {-2, 4}, {4, 2}, {-4, 2}, {0, 5}, {3, 4}, {-3, 4}, {4, 3}, {-4, 3}, {5, 0},
    {1, 5}, {-1, 5}, {5, 1}, {-5, 1}, {2, 5}, {-2, 5}, {5, 2}, {-5, 2}, {4, 4}, {-4, 4},
    {3, 5}, {-3, 5}, {5, 3}, {-5, 3}, {0, 6}, {6, 0}, {1, 6}, {-1, 6}, {6, 1}, {-6, 1},
    {2, 6}, {-2, 6}, {6, 2}, {-6, 2}, {4, 5}, {-4, 5}, {5, 4}, {-5, 4}, {3, 6}, {-3, 6},
    {6, 3}, {-6, 3}, {0, 7}, {7, 0}, {1, 7}, {-1, 7}, {5, 5}, {-5, 5}, {7, 1}, {-7, 1},
    {4, 6}, {-4, 6}, {6, 4}, {-6, 4}, {2, 7}, {-2, 7}, {7, 2}, {-7, 2}, {3, 7}, {-3, 7},
    {7, 3}, {-7, 3}, {5, 6}, {-5, 6}, {6, 5}, {-6, 5}, {8, 0}, {4, 7}, {-4, 7}, {7, 4},
    {-7, 4}, {8, 1}, {8, 2}, {6, 6}, {-6, 6}, {8, 3}, {5, 7}, {-5, 7}, {7, 5}, {-7, 5},
    {8, 4}, {6, 7}, {-6, 7}, {7, 6}, {-7, 6}, {8, 5}, {7, 7}, {-7, 7}, {8, 6}, {8, 7},
};

/* The pixels of one image (section 3.6, 3.7): the main one ('main', with
   meta prefix codes) or one of the small ones transforms carry */
static webp_u32 *vp8l_pixels(struct vp8l *d, int w, int h, int main)
{
    struct vp8l_br *b = &d->br;
    int cache_bits = 0;
    if (vp8l_read(b, 1)) {
        cache_bits = (int)vp8l_read(b, 4);
        if (cache_bits < 1 || cache_bits > 11)
            return 0;
    }
    int prefix_bits = 0, pw = 0, ngroups = 1;
    webp_u32 *meta = 0;
    if (main && vp8l_read(b, 1)) {
        prefix_bits = (int)vp8l_read(b, 3) + 2;
        pw = vp8l_div(w, prefix_bits);
        meta = vp8l_pixels(d, pw, vp8l_div(h, prefix_bits), 0);
        if (!meta)
            return 0;
        int ph = vp8l_div(h, prefix_bits);
        for (long i = 0; i < (long)pw * ph; i++) {
            int g = (int)((meta[i] >> 8) & 0xFFFF);
            if (g + 1 > ngroups)
                ngroups = g + 1;
        }
        if (ngroups > 4096) {          /* more than any encoder makes */
            free(meta);
            return 0;
        }
    }
    int cache_size = cache_bits ? 1 << cache_bits : 0;
    struct vp8l_code *codes =
        (struct vp8l_code *)calloc((size_t)ngroups * 5, sizeof(struct vp8l_code));
    webp_u32 *out = (webp_u32 *)malloc(sizeof(webp_u32) * (size_t)w * h);
    webp_u32 *cache = cache_size ? (webp_u32 *)calloc((size_t)cache_size, 4) : 0;
    int ok = codes && out && (!cache_size || cache);
    for (int g = 0; ok && g < ngroups; g++) {
        const int sizes[5] = { 256 + 24 + cache_size, 256, 256, 256, 40 };
        for (int k = 0; ok && k < 5; k++)
            ok = vp8l_read_code(b, &codes[g * 5 + k], sizes[k]) == 0;
    }
    long total = (long)w * h, pos = 0;
    int x = 0, y = 0;
    const struct vp8l_code *grp = codes;
    while (ok && pos < total) {
        if (meta && (x & ((1 << prefix_bits) - 1)) == 0)
            grp = codes + 5 * ((meta[(y >> prefix_bits) * pw + (x >> prefix_bits)] >> 8) & 0xFFFF);
        int s = vp8l_decode(b, &grp[0]);
        if (s < 256) {
            int r = vp8l_decode(b, &grp[1]);
            int bl = vp8l_decode(b, &grp[2]);
            int a = vp8l_decode(b, &grp[3]);
            webp_u32 px = (webp_u32)a << 24 | (webp_u32)r << 16 | (webp_u32)s << 8 | (webp_u32)bl;
            out[pos++] = px;
            if (cache)
                cache[(0x1e35a7bdu * px) >> (32 - cache_bits)] = px;
            if (++x == w) {
                x = 0;
                y++;
            }
        } else if (s < 256 + 24) {
            int n = vp8l_prefix_value(b, s - 256);
            int dc = vp8l_decode(b, &grp[4]);
            int dv = vp8l_prefix_value(b, dc);
            long dist;
            if (dv > 120) {
                dist = dv - 120;
            } else {
                dist = vp8l_dist_map[dv - 1][0] + (long)vp8l_dist_map[dv - 1][1] * w;
                if (dist < 1)
                    dist = 1;
            }
            if (dist > pos || pos + n > total) {
                ok = 0;
                break;
            }
            for (int i = 0; i < n; i++, pos++) {
                webp_u32 px = out[pos - dist];
                out[pos] = px;
                if (cache)
                    cache[(0x1e35a7bdu * px) >> (32 - cache_bits)] = px;
            }
            x += n;
            while (x >= w) {
                x -= w;
                y++;
            }
            if (meta && pos < total)
                grp = codes + 5 * ((meta[(y >> prefix_bits) * pw + (x >> prefix_bits)] >> 8) & 0xFFFF);
        } else {
            int i = s - 256 - 24;
            if (!cache || i >= cache_size) {
                ok = 0;
                break;
            }
            out[pos++] = cache[i];
            if (++x == w) {
                x = 0;
                y++;
            }
        }
        if (vp8l_eos(b))
            ok = 0;                    /* ran out: the picture is not all there */
    }
    if (codes)
        for (int i = 0; i < ngroups * 5; i++)
            vp8l_code_free(&codes[i]);
    free(codes);
    free(cache);
    free(meta);
    if (!ok) {
        free(out);
        return 0;
    }
    return out;
}

static webp_u32 vp8l_add(webp_u32 a, webp_u32 b)
{
    return (((a & 0xFF00FF00u) + (b & 0xFF00FF00u)) & 0xFF00FF00u) |
           (((a & 0x00FF00FFu) + (b & 0x00FF00FFu)) & 0x00FF00FFu);
}

static webp_u32 vp8l_avg(webp_u32 a, webp_u32 b)
{
    return (((a ^ b) & 0xFEFEFEFEu) >> 1) + (a & b);
}

static int vp8l_ch(webp_u32 p, int sh)
{
    return (int)(p >> sh & 255);
}

static webp_u32 vp8l_select(webp_u32 L, webp_u32 T, webp_u32 TL)
{
    int pl = 0, pt = 0;
    for (int sh = 0; sh < 32; sh += 8) {
        int p = vp8l_ch(L, sh) + vp8l_ch(T, sh) - vp8l_ch(TL, sh);
        pl += VP8_ABS(p - vp8l_ch(L, sh));
        pt += VP8_ABS(p - vp8l_ch(T, sh));
    }
    return pl < pt ? L : T;
}

static webp_u32 vp8l_clamp_full(webp_u32 a, webp_u32 b, webp_u32 c)
{
    webp_u32 r = 0;
    for (int sh = 0; sh < 32; sh += 8) {
        int v = vp8l_ch(a, sh) + vp8l_ch(b, sh) - vp8l_ch(c, sh);
        r |= (webp_u32)(v < 0 ? 0 : v > 255 ? 255 : v) << sh;
    }
    return r;
}

static webp_u32 vp8l_clamp_half(webp_u32 a, webp_u32 b)
{
    webp_u32 r = 0;
    for (int sh = 0; sh < 32; sh += 8) {
        int x = vp8l_ch(a, sh), v = x + (x - vp8l_ch(b, sh)) / 2;
        r |= (webp_u32)(v < 0 ? 0 : v > 255 ? 255 : v) << sh;
    }
    return r;
}

static webp_u32 vp8l_predict(int mode, const webp_u32 *p, int w)
{
    webp_u32 L = p[-1], T = p[-w], TL = p[-w - 1], TR = p[-w + 1];
    switch (mode) {
    case 1: return L;
    case 2: return T;
    case 3: return TR;
    case 4: return TL;
    case 5: return vp8l_avg(vp8l_avg(L, TR), T);
    case 6: return vp8l_avg(L, TL);
    case 7: return vp8l_avg(L, T);
    case 8: return vp8l_avg(TL, T);
    case 9: return vp8l_avg(T, TR);
    case 10: return vp8l_avg(vp8l_avg(L, TL), vp8l_avg(T, TR));
    case 11: return vp8l_select(L, T, TL);
    case 12: return vp8l_clamp_full(L, T, TL);
    case 13: return vp8l_clamp_half(vp8l_avg(L, T), TL);
    default: return 0xFF000000u;
    }
}

static int vp8l_delta(int t, int c)
{
    return ((signed char)t * (signed char)c) >> 5;
}

/* A transform undone, in place; the colour indexing one also widens the
   picture back from 'pix' (xsize wide) into 'out' (w wide) */
static void vp8l_inverse(const struct vp8l_transform *t, webp_u32 *pix, int w,
                         int h, webp_u32 *out)
{
    if (t->type == 0) {                /* predictor */
        int bw = vp8l_div(w, t->bits);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                webp_u32 *p = pix + (long)y * w + x, pred;
                if (!x && !y)
                    pred = 0xFF000000u;
                else if (!y)
                    pred = p[-1];
                else if (!x)
                    pred = p[-w];
                else
                    pred = vp8l_predict((int)(t->data[(y >> t->bits) * bw + (x >> t->bits)] >> 8 & 15),
                                        p, w);
                *p = vp8l_add(*p, pred);
            }
    } else if (t->type == 1) {         /* colour */
        int bw = vp8l_div(w, t->bits);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                webp_u32 e = t->data[(y >> t->bits) * bw + (x >> t->bits)];
                int g2r = (int)(e & 255), g2b = (int)(e >> 8 & 255), r2b = (int)(e >> 16 & 255);
                webp_u32 *p = pix + (long)y * w + x;
                int g = vp8l_ch(*p, 8), r = vp8l_ch(*p, 16), bl = vp8l_ch(*p, 0);
                r = (r + vp8l_delta(g2r, g)) & 255;
                bl = (bl + vp8l_delta(g2b, g)) & 255;
                bl = (bl + vp8l_delta(r2b, r)) & 255;
                *p = (*p & 0xFF00FF00u) | (webp_u32)r << 16 | (webp_u32)bl;
            }
    } else if (t->type == 2) {         /* subtract green */
        for (long i = 0; i < (long)w * h; i++) {
            webp_u32 g = pix[i] >> 8 & 255;
            webp_u32 rb = ((pix[i] & 0x00FF00FFu) + (g << 16 | g)) & 0x00FF00FFu;
            pix[i] = (pix[i] & 0xFF00FF00u) | rb;
        }
    } else {                           /* colour indexing */
        int bpp = 8 >> t->bits, mask = (1 << bpp) - 1, pw = vp8l_div(w, t->bits);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                webp_u32 packed = pix[(long)y * pw + (x >> t->bits)] >> 8 & 255;
                int idx = (int)(packed >> ((x & ((1 << t->bits) - 1)) * bpp)) & mask;
                out[(long)y * w + x] = idx < t->ncolours ? t->data[idx] : 0;
            }
    }
}

/* A VP8L image-stream (after the header): transforms, pixels, the
   transforms undone. NULL when it makes no sense. */
static webp_u32 *vp8l_stream(struct vp8l *d, int w, int h)
{
    struct vp8l_br *b = &d->br;
    int xsize = w;
    d->ntr = 0;
    webp_u32 *pix = 0;
    int seen = 0;
    while (vp8l_read(b, 1)) {
        int type = (int)vp8l_read(b, 2);
        if ((seen & (1 << type)) || d->ntr == 4 || vp8l_eos(b))
            goto fail;                 /* each transform at most once */
        seen |= 1 << type;
        struct vp8l_transform *t = &d->tr[d->ntr++];
        t->type = type;
        t->xsize = xsize;
        t->data = 0;
        t->bits = 0;
        if (type == 0 || type == 1) {
            t->bits = (int)vp8l_read(b, 3) + 2;
            t->data = vp8l_pixels(d, vp8l_div(xsize, t->bits), vp8l_div(h, t->bits), 0);
            if (!t->data)
                goto fail;
        } else if (type == 3) {
            int n = (int)vp8l_read(b, 8) + 1;
            t->ncolours = n;
            webp_u32 *pal = vp8l_pixels(d, n, 1, 0);
            if (!pal)
                goto fail;
            for (int i = 1; i < n; i++)  /* the palette is sent as deltas */
                pal[i] = vp8l_add(pal[i], pal[i - 1]);
            t->data = pal;
            t->bits = n <= 2 ? 3 : n <= 4 ? 2 : n <= 16 ? 1 : 0;
            xsize = vp8l_div(xsize, t->bits);   /* packed from here on */
        }
    }
    pix = vp8l_pixels(d, xsize, h, 1);
    if (!pix)
        goto fail;
    /* Undone last one first; a palette widens the picture back */
    for (int i = d->ntr - 1; i >= 0; i--) {
        struct vp8l_transform *t = &d->tr[i];
        if (t->type == 3) {
            webp_u32 *out = (webp_u32 *)malloc(sizeof(webp_u32) * (size_t)t->xsize * h);
            if (!out)
                goto fail;
            vp8l_inverse(t, pix, t->xsize, h, out);
            free(pix);
            pix = out;
        } else {
            vp8l_inverse(t, pix, t->xsize, h, 0);
        }
    }
    for (int i = 0; i < d->ntr; i++)
        free(d->tr[i].data);
    return pix;
fail:
    for (int i = 0; i < d->ntr; i++)
        free(d->tr[i].data);
    free(pix);
    return 0;
}

static int vp8l_decode_image(const webp_u8 *data, size_t len, webp_u8 **rgba,
                             int *ow, int *oh, const char **why)
{
    struct vp8l d;
    memset(&d, 0, sizeof(d));
    *why = "not a lossless WebP";
    if (len < 5 || data[0] != 0x2f)
        return -1;
    d.br.p = data + 1;
    d.br.n = len - 1;
    int w = (int)vp8l_read(&d.br, 14) + 1, h = (int)vp8l_read(&d.br, 14) + 1;
    vp8l_read(&d.br, 1);               /* alpha_is_used: a hint */
    if (vp8l_read(&d.br, 3) != 0)
        return -1;
    *why = "a size out of range";
    if ((long)w * h > WEBP_MAX_PIXELS)
        return -1;
    *why = "damaged";
    webp_u32 *pix = vp8l_stream(&d, w, h);
    if (!pix)
        return -1;
    *rgba = (webp_u8 *)malloc((size_t)w * h * 4);
    if (!*rgba) {
        free(pix);
        return -1;
    }
    for (long i = 0; i < (long)w * h; i++) {
        webp_u8 *o = *rgba + i * 4;
        o[0] = (webp_u8)(pix[i] >> 16);
        o[1] = (webp_u8)(pix[i] >> 8);
        o[2] = (webp_u8)pix[i];
        o[3] = (webp_u8)(pix[i] >> 24);
    }
    free(pix);
    *ow = w;
    *oh = h;
    return 0;
}

/* ======================================================================
 * The container
 * ==================================================================== */

static webp_u32 webp_le32(const webp_u8 *p)
{
    return p[0] | (webp_u32)p[1] << 8 | (webp_u32)p[2] << 16 | (webp_u32)p[3] << 24;
}

static webp_u32 webp_le24(const webp_u8 *p)
{
    return p[0] | (webp_u32)p[1] << 8 | (webp_u32)p[2] << 16;
}

/* The ALPH chunk onto a decoded picture's alpha (section 2.7.1.2) */
static int webp_alpha(const webp_u8 *a, size_t n, webp_u8 *rgba, int w, int h)
{
    if (n < 1)
        return -1;
    int method = a[0] & 3, filter = a[0] >> 2 & 3;
    webp_u8 *al = (webp_u8 *)malloc((size_t)w * h);
    if (!al)
        return -1;
    if (method == 0) {
        if (n - 1 < (size_t)w * h) {
            free(al);
            return -1;
        }
        memcpy(al, a + 1, (size_t)w * h);
    } else if (method == 1) {
        struct vp8l d;
        memset(&d, 0, sizeof(d));
        d.br.p = a + 1;
        d.br.n = n - 1;
        webp_u32 *pix = vp8l_stream(&d, w, h);
        if (!pix) {
            free(al);
            return -1;
        }
        for (long i = 0; i < (long)w * h; i++)
            al[i] = (webp_u8)(pix[i] >> 8);   /* in the green */
        free(pix);
    } else {
        free(al);
        return -1;
    }
    /* The filter undone: each value is the difference from a prediction */
    for (int y = 0; y < h && filter; y++)
        for (int x = 0; x < w; x++) {
            webp_u8 *p = al + (long)y * w + x;
            int pred;
            if (!x && !y)
                pred = 0;
            else if (!y)
                pred = p[-1];
            else if (!x)
                pred = p[-w];
            else if (filter == 1)
                pred = p[-1];
            else if (filter == 2)
                pred = p[-w];
            else {
                int g = p[-1] + p[-w] - p[-w - 1];
                pred = g < 0 ? 0 : g > 255 ? 255 : g;
            }
            *p = (webp_u8)(*p + pred);
        }
    for (long i = 0; i < (long)w * h; i++)
        rgba[i * 4 + 3] = al[i];
    free(al);
    return 0;
}

/* A frame's chunks - an optional ALPH, then VP8 or VP8L - into RGBA */
static int webp_frame(const webp_u8 *p, size_t n, webp_u8 **rgba, int *w, int *h,
                      const char **why)
{
    const webp_u8 *alph = 0;
    size_t alph_n = 0;
    *why = "no picture in it";
    while (n >= 8) {
        webp_u32 size = webp_le32(p + 4);
        if (size > n - 8) {
            *why = "cut short";
            return -1;
        }
        if (!memcmp(p, "ALPH", 4)) {
            alph = p + 8;
            alph_n = size;
        } else if (!memcmp(p, "VP8 ", 4)) {
            if (vp8_decode(p + 8, size, rgba, w, h, why) < 0)
                return -1;
            if (alph && webp_alpha(alph, alph_n, *rgba, *w, *h) < 0) {
                /* (a damaged alpha: the picture, opaque, rather than nothing) */
            }
            return 0;
        } else if (!memcmp(p, "VP8L", 4)) {
            return vp8l_decode_image(p + 8, size, rgba, w, h, why);
        }
        size_t step = 8 + (size_t)size + (size & 1);
        if (step > n)
            break;
        p += step;
        n -= step;
    }
    return -1;
}

/* Whether these bytes are a WebP file */
static int webp_sniff(const webp_u8 *d, size_t n)
{
    return n >= 16 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WEBP", 4);
}

/* A WebP file into a new RGBA buffer (to be freed with free()): 0, or -1
   with 'why'. An animation gives its first frame, on its canvas. */
static int webp_decode(const webp_u8 *d, size_t n, webp_u8 **rgba, int *w, int *h,
                       const char **why)
{
    *rgba = 0;
    *why = "not a WebP file";
    if (!webp_sniff(d, n))
        return -1;
    webp_u32 riff = webp_le32(d + 4);
    if (riff >= 4 && (size_t)riff + 8 < n)
        n = (size_t)riff + 8;          /* (bytes after the file are not its) */
    const webp_u8 *p = d + 12;
    size_t left = n - 12;
    if (left >= 18 && !memcmp(p, "VP8X", 4)) {
        int cw = (int)webp_le24(p + 12) + 1, ch = (int)webp_le24(p + 15) + 1;
        webp_u32 size = webp_le32(p + 4);
        *why = "a size out of range";
        if ((long)cw * ch > WEBP_MAX_PIXELS)
            return -1;
        if (size > left - 8)
            return -1;
        size_t step = 8 + (size_t)size + (size & 1);
        if (step > left)
            return -1;
        p += step;
        left -= step;
        /* An animation: the first ANMF frame, at its place on the canvas */
        const webp_u8 *q = p;
        size_t ql = left;
        while (ql >= 8) {
            webp_u32 sz = webp_le32(q + 4);
            if (sz > ql - 8)
                break;
            if (!memcmp(q, "ANMF", 4) && sz >= 16) {
                int fx = (int)webp_le24(q + 8) * 2, fy = (int)webp_le24(q + 11) * 2;
                webp_u8 *f;
                int fw, fh;
                if (webp_frame(q + 24, sz - 16, &f, &fw, &fh, why) < 0)
                    return -1;
                *rgba = (webp_u8 *)calloc((size_t)cw * ch, 4);
                if (!*rgba) {
                    free(f);
                    return -1;
                }
                for (int y = 0; y < fh && fy + y < ch; y++)
                    for (int x = 0; x < fw && fx + x < cw; x++)
                        memcpy(*rgba + ((size_t)(fy + y) * cw + fx + x) * 4,
                               f + ((size_t)y * fw + x) * 4, 4);
                free(f);
                *w = cw;
                *h = ch;
                return 0;
            }
            size_t st = 8 + (size_t)sz + (sz & 1);
            if (st > ql)
                break;
            q += st;
            ql -= st;
        }
    }
    return webp_frame(p, left, rgba, w, h, why);
}

#endif
