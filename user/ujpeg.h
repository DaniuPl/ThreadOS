/* ujpeg - a baseline JPEG, unpacked
 *
 * The third of the formats, and the one with real work in it. A
 * baseline JPEG is four things stacked:
 *
 *   - Huffman codes, as in deflate but written differently: sixteen
 *     counts (how many codes of each length) and then the symbols.
 *   - Coefficients: the DC one as a difference from the block before,
 *     the AC ones as (how many zeros to skip, how many bits follow).
 *     Both use the same trick - a symbol says how MANY bits the number
 *     takes, and the bits follow.
 *   - Quantisation: every coefficient was divided by a number from a
 *     table; multiplying it back is all that "dequantising" means.
 *   - The inverse cosine transform, which turns sixty-four numbers
 *     back into an eight by eight square of light. Done as eight
 *     columns and then eight rows, in fixed point: the separable
 *     version is sixteen small transforms instead of one of four
 *     thousand multiplications.
 *
 * Then the colours: JPEG stores brightness and two colour differences,
 * and the two are usually stored at half resolution, because an eye
 * that notices a smudged edge does not notice a smudged colour. Both
 * are put back at full size here, by repeating - the picture is going
 * to be squeezed into thirty-six colours anyway.
 *
 * A file that came apart on the way here must not take the decoder
 * with it, so every number out of the header that is used as an index
 * is checked, and the coefficients are held inside a range wide enough
 * that no real picture notices and narrow enough that the arithmetic
 * below cannot leave a 32-bit integer.
 *
 * What is NOT here, and says so rather than half-drawing: progressive
 * JPEG (a different arrangement of the same coefficients, in several
 * passes), arithmetic coding, twelve-bit samples and CMYK.
 */

#ifndef UJPEG_H
#define UJPEG_H

#ifndef HOST_TEST
#include "uapi.h"
#endif

#define JPG_MAX_COMP 3
/* 1920: a camera's 1080p frame; the height is bounded by the memory
   the caller gives */
#define JPG_MAX_W 1920

struct jpg_huff {
    unsigned char bits[17];        /* how many codes of each length 1..16 */
    unsigned char vals[256];
    int mincode[17], maxcode[18], valptr[17];
    int present;
};

struct jpg_comp {
    int id, hs, vs, tq;            /* sampling factors, quantisation table */
    int td, ta;                    /* the two Huffman tables */
    int dcpred;
    unsigned char *plane;          /* the component, at its own resolution */
    int pw, ph;                    /* and how big that is */
};

struct jpg {
    const unsigned char *in;
    unsigned int len, pos;
    unsigned int bitbuf;
    int bitcnt;
    unsigned short quant[4][64];
    struct jpg_huff hdc[4], hac[4];
    struct jpg_comp comp[JPG_MAX_COMP];
    int ncomp, width, height;
    int hmax, vmax, mcux, mcuy;
    int restart;                   /* how many MCUs between restart markers */
    int rgb;                       /* components are R, G, B, not Y, Cb, Cr */
    int error;
};

/* The order the coefficients were written in: a zigzag out from the
   corner, so that the numbers that matter come first and the run of
   zeros at the end can be skipped in one symbol. */
static const unsigned char jpg_zig[64] = {
     0,  1,  8, 16,  9,  2,  3, 10, 17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
};

static int jpg_u16(struct jpg *j)
{
    if (j->pos + 2 > j->len) {
        j->error = 1;
        return 0;
    }
    int v = (j->in[j->pos] << 8) | j->in[j->pos + 1];
    j->pos += 2;
    return v;
}

/* --- the bit stream inside the scan ------------------------------- */

/* A 0xFF byte in the entropy stream is followed by 0x00 to say "this
   is data, not a marker" - the zero is not part of the picture. */
static int jpg_bit(struct jpg *j)
{
    if (!j->bitcnt) {
        if (j->pos >= j->len) {
            j->error = 1;
            return 0;
        }
        unsigned char b = j->in[j->pos++];
        if (b == 0xFF) {
            unsigned char nxt = j->pos < j->len ? j->in[j->pos] : 0;
            if (nxt == 0x00)
                j->pos++;
            else {
                j->error = 1;      /* a marker in the middle of the data */
                return 0;
            }
        }
        j->bitbuf = b;
        j->bitcnt = 8;
    }
    j->bitcnt--;
    return (int)((j->bitbuf >> j->bitcnt) & 1);
}

static int jpg_bits(struct jpg *j, int n)
{
    int v = 0;
    for (int i = 0; i < n; i++)
        v = (v << 1) | jpg_bit(j);
    return v;
}

/* A number written as "how many bits, then the bits": the top half of
   the range is positive, the bottom half negative. */
static int jpg_extend(int v, int n)
{
    if (!n)
        return 0;
    return v < (1 << (n - 1)) ? v - (1 << n) + 1 : v;
}

static void jpg_build(struct jpg_huff *h)
{
    int code = 0, k = 0;
    for (int l = 1; l <= 16; l++) {
        h->valptr[l] = k;
        h->mincode[l] = code;
        code += h->bits[l];
        k += h->bits[l];
        h->maxcode[l] = code - 1;
        code <<= 1;
        if (!h->bits[l])
            h->maxcode[l] = -1;
    }
    h->maxcode[17] = 0x7FFFFFFF;
}

static int jpg_decode(struct jpg *j, const struct jpg_huff *h)
{
    int code = jpg_bit(j);
    int l = 1;
    while (l <= 16 && (h->maxcode[l] < 0 || code > h->maxcode[l])) {
        code = (code << 1) | jpg_bit(j);
        l++;
        if (j->error)
            return 0;
    }
    if (l > 16) {
        j->error = 1;
        return 0;
    }
    return h->vals[h->valptr[l] + code - h->mincode[l]];
}

/* --- the inverse transform ---------------------------------------- */
/* Fixed point, twelve bits after the point. The constants are the
   cosines the transform is made of. */
#define JPG_F(x) ((int)((x) * 4096 + 0.5))

static void jpg_idct_1d(int *s0, int *s1, int *s2, int *s3,
                        int *s4, int *s5, int *s6, int *s7)
{
    int p2 = *s2, p3 = *s6;
    int p1 = (p2 + p3) * JPG_F(0.5411961);
    int t2 = p1 + p3 * JPG_F(-1.847759065);
    int t3 = p1 + p2 * JPG_F(0.765366865);
    p2 = *s0;
    p3 = *s4;
    int t0 = (p2 + p3) * 4096;
    int t1 = (p2 - p3) * 4096;
    int x0 = t0 + t3, x3 = t0 - t3, x1 = t1 + t2, x2 = t1 - t2;

    t0 = *s7;
    t1 = *s5;
    t2 = *s3;
    t3 = *s1;
    p3 = t0 + t2;
    int p4 = t1 + t3;
    p1 = t0 + t3;
    p2 = t1 + t2;
    int p5 = (p3 + p4) * JPG_F(1.175875602);
    t0 *= JPG_F(0.298631336);
    t1 *= JPG_F(2.053119869);
    t2 *= JPG_F(3.072711026);
    t3 *= JPG_F(1.501321110);
    p1 = p5 + p1 * JPG_F(-0.899976223);
    p2 = p5 + p2 * JPG_F(-2.562915447);
    p3 = p3 * JPG_F(-1.961570560);
    p4 = p4 * JPG_F(-0.390180644);
    t3 += p1 + p4;
    t2 += p2 + p3;
    t1 += p2 + p4;
    t0 += p1 + p3;

    *s0 = x0 + t3;
    *s7 = x0 - t3;
    *s1 = x1 + t2;
    *s6 = x1 - t2;
    *s2 = x2 + t1;
    *s5 = x2 - t1;
    *s3 = x3 + t0;
    *s4 = x3 - t0;
}

/* A real picture never comes near these: the largest coefficient an
   eight bit block can produce is about 2040, and the largest number
   between the two passes about 4200 (measured over a corpus). Ten
   times that is still small enough that the multiplications below
   cannot leave a 32-bit integer, so a damaged file gives nonsense
   pixels rather than undefined behaviour. */
#define JPG_COEF_MAX 16384
#define JPG_MID_MAX  32768

static int jpg_bound(int v, int max)
{
    if (v < -max)
        return -max;
    if (v > max)
        return max;
    return v;
}

static unsigned char jpg_clamp(int v)
{
    if (v < 0)
        return 0;
    if (v > 255)
        return 255;
    return (unsigned char)v;
}

static void jpg_idct(const int *in, unsigned char *out, int stride)
{
    int v[64];
    for (int i = 0; i < 64; i++)
        v[i] = jpg_bound(in[i], JPG_COEF_MAX);

    for (int i = 0; i < 8; i++) {          /* the columns */
        int *c = v + i;
        jpg_idct_1d(c, c + 8, c + 16, c + 24, c + 32, c + 40, c + 48,
                    c + 56);
        for (int k = 0; k < 8; k++)
            c[k * 8] = jpg_bound((c[k * 8] + (1 << 9)) >> 10,
                                 JPG_MID_MAX);
    }
    for (int i = 0; i < 8; i++) {          /* and the rows */
        int *r = v + i * 8;
        jpg_idct_1d(r, r + 1, r + 2, r + 3, r + 4, r + 5, r + 6, r + 7);
        for (int k = 0; k < 8; k++)
            out[i * stride + k] = jpg_clamp(((r[k] + (1 << 16)) >> 17) + 128);
    }
}

/* --- one block ----------------------------------------------------- */

static void jpg_block(struct jpg *j, struct jpg_comp *c, int bx, int by)
{
    int blk[64];
    for (int i = 0; i < 64; i++)
        blk[i] = 0;

    int t = jpg_decode(j, &j->hdc[c->td]);
    int diff = t ? jpg_extend(jpg_bits(j, t), t) : 0;
    c->dcpred += diff;
    blk[0] = c->dcpred * (int)j->quant[c->tq][0];

    for (int k = 1; k < 64;) {
        int rs = jpg_decode(j, &j->hac[c->ta]);
        int r = rs >> 4, sz = rs & 15;
        if (!sz) {
            if (r != 15)
                break;                     /* the rest of the block is zeros */
            k += 16;
            continue;
        }
        k += r;
        if (k > 63)
            break;
        int val = jpg_extend(jpg_bits(j, sz), sz);
        blk[jpg_zig[k]] = val * (int)j->quant[c->tq][k];
        k++;
    }

    if (bx + 8 <= c->pw && by + 8 <= c->ph) {
        jpg_idct(blk, c->plane + (long)by * c->pw + bx, c->pw);
    } else {
        unsigned char tmp[64];
        jpg_idct(blk, tmp, 8);
        for (int y = 0; y < 8 && by + y < c->ph; y++)
            for (int x = 0; x < 8 && bx + x < c->pw; x++)
                c->plane[(long)(by + y) * c->pw + bx + x] = tmp[y * 8 + x];
    }
}

/* --- the whole file ------------------------------------------------ */

/* Decodes into three planes at full size. 'mem' is where the planes go
   (three times width*height), so the caller decides where the memory
   comes from. Returns 0, or -1 with 'why' set to something a person
   can read. */
/* One decoder's worth of state. It lives here rather than on a stack
   because it holds the tables and the component planes' whereabouts,
   and because the caller asks for pixels after the call has
   returned. */
/* The typical Huffman tables of ITU-T T.81, Annex K.3 (tables K.3 to
   K.6): DC and AC, for luminance (table 0) and chrominance (table 1).
   The counts of codes of each length 1..16, then the values. */
static const unsigned char jpg_k_bits[4][16] = {
    { 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0 },          /* DC 0 */
    { 0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0 },          /* DC 1 */
    { 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d },       /* AC 0 */
    { 0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77 },       /* AC 1 */
};
static const unsigned char jpg_k_dc_vals[12] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
static const unsigned char jpg_k_ac0_vals[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06,
    0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08,
    0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72,
    0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45,
    0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
    0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75,
    0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3,
    0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6,
    0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9,
    0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4,
    0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa };
static const unsigned char jpg_k_ac1_vals[162] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41,
    0x51, 0x07, 0x61, 0x71, 0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91,
    0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0, 0x15, 0x62, 0x72, 0xd1,
    0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
    0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44,
    0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
    0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74,
    0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a,
    0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4,
    0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7,
    0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
    0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4,
    0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa };

/* A table a scan uses and the file never gave: the standard's, for
   tables 0 and 1 - the only ones it has */
static void jpg_default_tables(struct jpg_huff *h, int ac, int index)
{
    if (h->present || index > 1)
        return;
    const unsigned char *bits = jpg_k_bits[ac * 2 + index];
    const unsigned char *vals = !ac ? jpg_k_dc_vals :
                                index ? jpg_k_ac1_vals : jpg_k_ac0_vals;
    int total = 0;
    h->bits[0] = 0;
    for (int i = 1; i <= 16; i++) {
        h->bits[i] = bits[i - 1];
        total += bits[i - 1];
    }
    for (int i = 0; i < total; i++)
        h->vals[i] = vals[i];
    jpg_build(h);
    h->present = 1;
}

static struct jpg jpeg_j;

static int jpeg_decode(const unsigned char *data, unsigned int len,
                       unsigned char *mem, unsigned int mem_len,
                       int *out_w, int *out_h, const char **why)
{
    struct jpg j_copy;
    (void)j_copy;
#define j jpeg_j
    j.in = data;
    j.len = len;
    j.pos = 0;
    j.error = 0;
    j.restart = 0;
    j.ncomp = 0;
    j.rgb = 0;
    for (int i = 0; i < 4; i++) {
        j.hdc[i].present = 0;
        j.hac[i].present = 0;
    }

    if (len < 4 || data[0] != 0xFF || data[1] != 0xD8) {
        *why = "this is not a JPEG";
        return -1;
    }
    j.pos = 2;

    while (j.pos + 4 <= j.len) {
        if (j.in[j.pos] != 0xFF) {
            j.pos++;
            continue;
        }
        int marker = j.in[j.pos + 1];
        j.pos += 2;
        if (marker == 0xD8 || marker == 0x01 ||
            (marker >= 0xD0 && marker <= 0xD7))
            continue;
        if (marker == 0xD9)
            break;

        int seg = jpg_u16(&j);
        unsigned int end = j.pos + (unsigned int)seg - 2;
        if (end > j.len) {
            *why = "the file is cut short";
            return -1;
        }

        switch (marker) {
        case 0xC0:                          /* baseline */
        case 0xC1: {
            j.pos++;                        /* precision, always 8 here */
            j.height = jpg_u16(&j);
            j.width = jpg_u16(&j);
            j.ncomp = j.in[j.pos++];
            if (j.ncomp > JPG_MAX_COMP || j.width <= 0 ||
                j.width > JPG_MAX_W || j.height <= 0) {
                *why = "odd dimensions, or too many components";
                return -1;
            }
            for (int i = 0; i < j.ncomp; i++) {
                j.comp[i].id = j.in[j.pos];
                j.comp[i].hs = j.in[j.pos + 1] >> 4;
                j.comp[i].vs = j.in[j.pos + 1] & 15;
                j.comp[i].tq = j.in[j.pos + 2];
                j.pos += 3;
                if (j.comp[i].tq > 3) {
                    *why = "a bad quantisation table";
                    return -1;
                }
                if (j.comp[i].hs < 1 || j.comp[i].hs > 4 ||
                    j.comp[i].vs < 1 || j.comp[i].vs > 4) {
                    *why = "unusual sampling";
                    return -1;
                }
            }
            /* A single component is always its own MCU - a data unit -
               whatever sampling factors the header claims, so grey
               pictures written with 2x2 factors still read straight. */
            if (j.ncomp == 1) {
                j.comp[0].hs = 1;
                j.comp[0].vs = 1;
            }
            /* Three components named R, G and B carry the colours
               already separated; only the usual Y, Cb, Cr needs the
               conversion below. */
            j.rgb = j.ncomp == 3 && j.comp[0].id == 'R' &&
                    j.comp[1].id == 'G' && j.comp[2].id == 'B';
            break;
        }
        case 0xC2:
            *why = "a progressive JPEG - I cannot read it";
            return -1;
        case 0xC4:                          /* Huffman tables */
            while (j.pos < end) {
                int tc = j.in[j.pos] >> 4, th = j.in[j.pos] & 15;
                j.pos++;
                if (th > 3) {
                    *why = "a bad Huffman table";
                    return -1;
                }
                struct jpg_huff *h = tc ? &j.hac[th] : &j.hdc[th];
                int total = 0;
                h->bits[0] = 0;
                for (int i = 1; i <= 16; i++) {
                    h->bits[i] = j.in[j.pos++];
                    total += h->bits[i];
                }
                if (total > 256 || j.pos + (unsigned)total > j.len) {
                    *why = "a bad Huffman table";
                    return -1;
                }
                for (int i = 0; i < total; i++)
                    h->vals[i] = j.in[j.pos++];
                jpg_build(h);
                h->present = 1;
            }
            break;
        case 0xDB:                          /* quantisation tables */
            while (j.pos < end) {
                int pq = j.in[j.pos] >> 4, tq = j.in[j.pos] & 15;
                j.pos++;
                if (tq > 3) {
                    *why = "a bad quantisation table";
                    return -1;
                }
                for (int i = 0; i < 64; i++) {
                    if (pq) {
                        j.quant[tq][i] = (unsigned short)
                            ((j.in[j.pos] << 8) | j.in[j.pos + 1]);
                        j.pos += 2;
                    } else {
                        j.quant[tq][i] = j.in[j.pos++];
                    }
                }
            }
            break;
        case 0xDD:                          /* restart interval */
            j.restart = jpg_u16(&j);
            j.pos = end;
            break;
        case 0xDA: {                        /* the scan itself */
            int ns = j.in[j.pos++];
            if (ns < 1 || ns != j.ncomp) {
                *why = "the scan does not cover the whole picture";
                return -1;
            }
            for (int i = 0; i < ns; i++) {
                int id = j.in[j.pos], tt = j.in[j.pos + 1];
                j.pos += 2;
                if ((tt >> 4) > 3 || (tt & 15) > 3) {
                    *why = "a bad Huffman table";
                    return -1;
                }
                for (int k = 0; k < j.ncomp; k++)
                    if (j.comp[k].id == id) {
                        j.comp[k].td = tt >> 4;
                        j.comp[k].ta = tt & 15;
                    }
            }
            for (int i = 0; i < j.ncomp; i++) {
                /* A webcam's MJPEG frame leaves its Huffman tables out:
                   the ones in the standard are implied (the AVI1 rule,
                   which everything that reads such frames follows) */
                jpg_default_tables(&j.hdc[j.comp[i].td], 0, j.comp[i].td);
                jpg_default_tables(&j.hac[j.comp[i].ta], 1, j.comp[i].ta);
                if (!j.hdc[j.comp[i].td].present ||
                    !j.hac[j.comp[i].ta].present) {
                    *why = "no Huffman table for a component";
                    return -1;
                }
            }
            j.pos += 3;                     /* spectral selection, baseline */

            /* Room for the components, at their own resolutions */
            j.hmax = 1;
            j.vmax = 1;
            for (int i = 0; i < j.ncomp; i++) {
                if (j.comp[i].hs > j.hmax) j.hmax = j.comp[i].hs;
                if (j.comp[i].vs > j.vmax) j.vmax = j.comp[i].vs;
            }
            j.mcux = (j.width + 8 * j.hmax - 1) / (8 * j.hmax);
            j.mcuy = (j.height + 8 * j.vmax - 1) / (8 * j.vmax);

            unsigned int need = 0;
            for (int i = 0; i < j.ncomp; i++) {
                j.comp[i].pw = j.mcux * 8 * j.comp[i].hs;
                j.comp[i].ph = j.mcuy * 8 * j.comp[i].vs;
                need += (unsigned int)j.comp[i].pw * j.comp[i].ph;
            }
            if (need > mem_len) {
                *why = "the picture is too big for memory";
                return -1;
            }
            unsigned int at = 0;
            for (int i = 0; i < j.ncomp; i++) {
                j.comp[i].plane = mem + at;
                at += (unsigned int)j.comp[i].pw * j.comp[i].ph;
                j.comp[i].dcpred = 0;
            }

            j.bitcnt = 0;
            int since = 0;
            for (int my = 0; my < j.mcuy && !j.error; my++) {
                for (int mx = 0; mx < j.mcux && !j.error; mx++) {
                    for (int i = 0; i < j.ncomp; i++)
                        for (int v = 0; v < j.comp[i].vs; v++)
                            for (int hh = 0; hh < j.comp[i].hs; hh++)
                                jpg_block(&j, &j.comp[i],
                                          (mx * j.comp[i].hs + hh) * 8,
                                          (my * j.comp[i].vs + v) * 8);
                    /* Restart markers: the stream starts again from a
                       byte boundary and the DC differences begin from
                       zero, which is how a damaged file stays readable
                       past the damage. */
                    if (j.restart && ++since == j.restart) {
                        since = 0;
                        j.bitcnt = 0;
                        while (j.pos + 1 < j.len &&
                               !(j.in[j.pos] == 0xFF &&
                                 j.in[j.pos + 1] >= 0xD0 &&
                                 j.in[j.pos + 1] <= 0xD7))
                            j.pos++;
                        if (j.pos + 1 < j.len)
                            j.pos += 2;
                        for (int i = 0; i < j.ncomp; i++)
                            j.comp[i].dcpred = 0;
                    }
                }
            }
            *out_w = j.width;
            *out_h = j.height;
            if (j.error) {
                *why = "the JPEG stream does not add up";
                return -1;
            }
            return 0;
        }
        default:
            j.pos = end;                    /* APPn, COM and the rest */
            break;
        }
        if (j.pos < end)
            j.pos = end;
    }
    *why = "I found no picture in the file";
    return -1;
#undef j
}

/* One pixel of the decoded picture as RGB. The components are at their
   own resolutions, so a colour plane at half size is read at half the
   coordinates - which is the "putting it back" the eye does not
   notice. */
static void jpeg_pixel(int x, int y, int *r, int *g, int *b)
{
    const struct jpg *j = &jpeg_j;
    const struct jpg_comp *c0 = &j->comp[0];
    int yy = c0->plane[(long)(y * c0->vs / j->vmax) * c0->pw +
                       (x * c0->hs / j->hmax)];
    if (j->ncomp < 3) {
        *r = *g = *b = yy;
        return;
    }
    const struct jpg_comp *c1 = &j->comp[1], *c2 = &j->comp[2];
    if (j->rgb) {
        *r = yy;
        *g = c1->plane[(long)(y * c1->vs / j->vmax) * c1->pw +
                       (x * c1->hs / j->hmax)];
        *b = c2->plane[(long)(y * c2->vs / j->vmax) * c2->pw +
                       (x * c2->hs / j->hmax)];
        return;
    }
    int cb = c1->plane[(long)(y * c1->vs / j->vmax) * c1->pw +
                       (x * c1->hs / j->hmax)] - 128;
    int cr = c2->plane[(long)(y * c2->vs / j->vmax) * c2->pw +
                       (x * c2->hs / j->hmax)] - 128;
    int rr = yy + (91881 * cr >> 16);
    int gg = yy - ((22554 * cb + 46802 * cr) >> 16);
    int bb = yy + (116130 * cb >> 16);
    *r = rr < 0 ? 0 : rr > 255 ? 255 : rr;
    *g = gg < 0 ? 0 : gg > 255 ? 255 : gg;
    *b = bb < 0 ? 0 : bb > 255 ? 255 : bb;
}

#endif
