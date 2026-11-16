/* upng - a PNG, unpacked
 *
 * A PNG is chunks: IHDR says the size and the kind of pixels, PLTE a
 * palette when there is one, and the pixels are one zlib stream spread
 * over as many IDAT chunks as the writer felt like - so they are joined
 * before the stream is inflated. Every row then starts with a filter
 * byte: the row is stored as its difference from the pixel to the left,
 * the one above, their average, or whichever of the three Paeth's
 * predictor picks; undoing it needs the row above, so rows are undone
 * in order.
 *
 * All five colour kinds - grey, grey with alpha, RGB, RGBA and a
 * palette - at every depth they come in: 1, 2 and 4 bits a pixel for a
 * palette or grey (small icons are written that way), 8, and 16, of
 * which the top byte is kept. Not interlaced: that says so instead of
 * showing a wrong picture.
 *
 * The result is four bytes a pixel, red, green, blue and alpha, as
 * ugif.h gives them. It was the picture viewer's (gimg) and is here so
 * the Gallery reads PNGs the same way.
 */

#ifndef UPNG_H
#define UPNG_H

#include "uinflate.h"

#define PNG_MAX_SIDE 8192

static int png_paeth(int a, int b, int c)
{
    int p = a + b - c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc)
        return a;
    return pb <= pc ? b : c;
}

static unsigned int png_u32(const unsigned char *p)
{
    return ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) |
           ((unsigned int)p[2] << 8) | p[3];
}

static int png_sniff(const unsigned char *d, unsigned int n)
{
    static const unsigned char sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A,
                                          0x1A, 0x0A };
    if (n < 8)
        return 0;
    for (int i = 0; i < 8; i++)
        if (d[i] != sig[i])
            return 0;
    return 1;
}

/* The size alone, from IHDR: 0, or -1 when it is not a PNG */
static int png_size(const unsigned char *d, unsigned int n, int *w, int *h)
{
    if (!png_sniff(d, n) || n < 24 || d[12] != 'I' || d[13] != 'H' ||
        d[14] != 'D' || d[15] != 'R')
        return -1;
    *w = (int)png_u32(d + 16);
    *h = (int)png_u32(d + 20);
    return 0;
}

/* The room png_decode() needs in 'work' for a picture w by h out of a
   file of n bytes: the rows inflated (at most eight bytes a pixel and a
   filter byte a row) and the compressed stream joined behind them */
static unsigned long png_work_size(int w, int h, unsigned int n)
{
    return (unsigned long)h * (1 + (unsigned long)w * 8) + n;
}

/* Decodes the picture into 'out' (w * h * 4 bytes, out_max at most).
   'work' is png_work_size() bytes, work_max of them. Returns 0, or -1
   with 'why' saying what was wrong. */
static int png_decode(const unsigned char *d, unsigned int n,
                      unsigned char *out, unsigned int out_max,
                      unsigned char *work, unsigned int work_max,
                      int *out_w, int *out_h, const char **why)
{
    int w = 0, h = 0, depth = 0, colour = 0, interlace = 0;
    unsigned char plte[256 * 3];
    unsigned char trns[256];
    int plte_n = 0, trns_n = 0;
    *why = "this is not a PNG";
    if (!png_sniff(d, n))
        return -1;

    /* The IDAT chunks, joined where they lie: each moved down over the
       chunk headers between them. The file is the caller's and stays
       as it was, so they are joined into the end of 'work' first. */
    unsigned int idat_n = 0, idat_at = work_max;
    unsigned int pos = 8;
    for (int pass = 0; pass < 2; pass++) {
        pos = 8;
        unsigned int k = 0;
        while (pos + 12 <= n) {
            unsigned int len = png_u32(d + pos);
            const unsigned char *name = d + pos + 4;
            const unsigned char *body = d + pos + 8;
            if (len > n - pos - 12)
                break;                  /* cut short: what came so far */
            if (!pass && name[0] == 'I' && name[1] == 'H' &&
                name[2] == 'D' && name[3] == 'R' && len >= 13) {
                w = (int)png_u32(body);
                h = (int)png_u32(body + 4);
                depth = body[8];
                colour = body[9];
                interlace = body[12];
            } else if (!pass && name[0] == 'P' && name[1] == 'L' &&
                       name[2] == 'T' && name[3] == 'E') {
                plte_n = (int)(len / 3) > 256 ? 256 : (int)(len / 3);
                for (int i = 0; i < plte_n * 3; i++)
                    plte[i] = body[i];
            } else if (!pass && name[0] == 't' && name[1] == 'R' &&
                       name[2] == 'N' && name[3] == 'S') {
                trns_n = len > 256 ? 256 : (int)len;
                for (int i = 0; i < trns_n; i++)
                    trns[i] = body[i];
            } else if (name[0] == 'I' && name[1] == 'D' && name[2] == 'A' &&
                       name[3] == 'T') {
                if (!pass) {
                    idat_n += len;
                } else {
                    for (unsigned int i = 0; i < len; i++)
                        work[idat_at + k + i] = body[i];
                    k += len;
                }
            } else if (name[0] == 'I' && name[1] == 'E' && name[2] == 'N' &&
                       name[3] == 'D') {
                break;
            }
            pos += 12 + len;
        }
        if (!pass) {
            if (w <= 0 || h <= 0 || w > PNG_MAX_SIDE || h > PNG_MAX_SIDE) {
                *why = "odd dimensions";
                return -1;
            }
            int ok = depth == 8 || depth == 16 ||
                     ((colour == 0 || colour == 3) &&
                      (depth == 1 || depth == 2 || depth == 4));
            if (!ok) {
                *why = "an unknown kind of PNG";
                return -1;
            }
            if (interlace) {
                *why = "I cannot read an interlaced PNG";
                return -1;
            }
            if ((unsigned long)w * (unsigned long)h * 4 > out_max) {
                *why = "too large";
                return -1;
            }
            if (idat_n > work_max) {
                *why = "too large";
                return -1;
            }
            idat_at = work_max - idat_n;
        }
    }

    int chan = colour == 2 ? 3 : colour == 6 ? 4 : colour == 4 ? 2 :
               colour == 0 || colour == 3 ? 1 : 0;
    if (!chan || (colour == 3 && !plte_n)) {
        *why = "an unknown kind of PNG";
        return -1;
    }
    /* Bits a pixel, bytes a row, and the distance the filters reach
       back: a whole pixel, or one byte when a pixel is less */
    unsigned long bits = (unsigned long)chan * (unsigned long)depth;
    unsigned long stride = ((unsigned long)w * bits + 7) / 8;
    unsigned long reach = bits >= 8 ? bits / 8 : 1;
    unsigned long raw_len = (unsigned long)h * (1 + stride);
    if (raw_len > idat_at) {
        *why = "too large";
        return -1;
    }
    /* Inflated into the front of 'work', the joined stream behind it */
    if (inflate_zlib(work + idat_at, idat_n, work, (unsigned int)raw_len) < 0) {
        *why = "the PNG stream does not add up";
        return -1;
    }

    for (int y = 0; y < h; y++) {
        unsigned char *line = work + (unsigned long)y * (1 + stride) + 1;
        const unsigned char *prev = y ? line - (1 + stride) : 0;
        int filter = line[-1];
        for (unsigned long i = 0; i < stride; i++) {
            int a = i >= reach ? line[i - reach] : 0;
            int b = prev ? prev[i] : 0;
            int c = (prev && i >= reach) ? prev[i - reach] : 0;
            switch (filter) {
            case 1: line[i] = (unsigned char)(line[i] + a); break;
            case 2: line[i] = (unsigned char)(line[i] + b); break;
            case 3: line[i] = (unsigned char)(line[i] + ((a + b) >> 1)); break;
            case 4: line[i] = (unsigned char)(line[i] + png_paeth(a, b, c));
                    break;
            default: break;             /* 0: as it stands */
            }
        }
        unsigned char *o = out + (unsigned long)y * (unsigned long)w * 4;
        for (int x = 0; x < w; x++, o += 4) {
            /* The pixel's channels as bytes: below eight bits it is one
               channel, taken out of its byte (the leftmost pixel in the
               highest bits); at sixteen, each channel's top byte */
            unsigned char v[4];
            if (depth < 8) {
                unsigned long at = (unsigned long)x * (unsigned long)depth;
                int shift = 8 - depth - (int)(at % 8);
                v[0] = (unsigned char)((line[at / 8] >> shift) &
                                       ((1 << depth) - 1));
            } else {
                int size = depth / 8;
                for (int k = 0; k < chan; k++)
                    v[k] = line[((unsigned long)x * chan + k) * size];
            }
            switch (colour) {
            case 2: o[0] = v[0]; o[1] = v[1]; o[2] = v[2]; o[3] = 255; break;
            case 6: o[0] = v[0]; o[1] = v[1]; o[2] = v[2]; o[3] = v[3]; break;
            case 4: o[0] = o[1] = o[2] = v[0]; o[3] = v[1]; break;
            case 3: {
                int i = v[0] < plte_n ? v[0] : 0;
                o[0] = plte[i * 3];
                o[1] = plte[i * 3 + 1];
                o[2] = plte[i * 3 + 2];
                o[3] = v[0] < trns_n ? trns[v[0]] : 255;
                break;
            }
            default: {
                /* grey below eight bits, spread over 0..255 */
                int g = depth < 8 ? v[0] * 255 / ((1 << depth) - 1) : v[0];
                o[0] = o[1] = o[2] = (unsigned char)g;
                o[3] = 255;
                break;
            }
            }
        }
    }
    *out_w = w;
    *out_h = h;
    return 0;
}

#endif
