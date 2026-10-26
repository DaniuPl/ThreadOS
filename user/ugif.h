/* ugif - a GIF, unpacked: its first frame
 *
 * A GIF is a palette and pictures drawn with it, each compressed with
 * LZW: a dictionary that starts as the palette's indices and grows by
 * one entry per code read - the string just made plus the first index
 * of the next - so the decoder rebuilds the very dictionary the encoder
 * had, without it ever being written down. Codes start one bit wider
 * than the palette needs and grow as the dictionary does, up to twelve
 * bits; a "clear" code starts it over.
 *
 * Around that: the screen the pictures are placed on, a global palette
 * and perhaps one of the picture's own, a graphic control block that
 * may name an index as transparent, and interlacing, which sends rows
 * 0, 8, 16..., then 4, 12..., then 2, 6..., then the odd ones, so that
 * a slow download showed something early.
 *
 * Only the first frame of an animation: a page shows a still of it.
 * Out of a network, so every index and length is checked before it is
 * used; a file cut short gives what was decoded so far rather than
 * nothing - which is what browsers do too.
 *
 * The result is four bytes a pixel, red, green, blue and alpha (0 where
 * the GIF is transparent or the frame does not reach).
 */

#ifndef UGIF_H
#define UGIF_H

#define GIF_MAX_SIDE 4096

struct gif_reader {
    const unsigned char *d;
    unsigned int n, at;
};

/* The next byte, or -1 past the end */
static int gif_byte(struct gif_reader *r)
{
    return r->at < r->n ? r->d[r->at++] : -1;
}

static int gif_u16(struct gif_reader *r)
{
    int lo = gif_byte(r), hi = gif_byte(r);
    return lo < 0 || hi < 0 ? -1 : lo | hi << 8;
}

/* Data sub-blocks: a length byte, that many bytes, until a zero length */
static void gif_skip_blocks(struct gif_reader *r)
{
    int len;
    while ((len = gif_byte(r)) > 0)
        r->at = r->at + (unsigned)len <= r->n ? r->at + (unsigned)len : r->n;
}

/* Decodes the first frame into 'out' (w * h * 4 bytes, out_max at most).
   Returns 0, or -1 with 'why' saying what was wrong. */
static int gif_decode(const unsigned char *data, unsigned int len,
                      unsigned char *out, unsigned int out_max,
                      int *out_w, int *out_h, const char **why)
{
    static unsigned short prefix[4096];
    static unsigned char suffix[4096], stack[4097];
    unsigned char pal[768];
    struct gif_reader r = { data, len, 0 };
    *why = "not a GIF";
    if (len < 13 || data[0] != 'G' || data[1] != 'I' || data[2] != 'F')
        return -1;
    r.at = 6;
    int w = gif_u16(&r), h = gif_u16(&r), flags = gif_byte(&r);
    gif_byte(&r);                      /* the background: transparent here */
    gif_byte(&r);                      /* the aspect ratio */
    if (w <= 0 || h <= 0 || w > GIF_MAX_SIDE || h > GIF_MAX_SIDE) {
        *why = "a size out of range";
        return -1;
    }
    if ((unsigned long)w * (unsigned long)h * 4 > out_max) {
        *why = "too big";
        return -1;
    }
    for (unsigned int i = 0; i < (unsigned)w * (unsigned)h * 4; i++)
        out[i] = 0;
    int gsize = 0;
    if (flags & 0x80) {
        gsize = 2 << (flags & 7);
        if (r.at + (unsigned)gsize * 3 > len) {
            *why = "cut short in its palette";
            return -1;
        }
        for (int i = 0; i < gsize * 3; i++)
            pal[i] = data[r.at + i];
        r.at += (unsigned)gsize * 3;
    }
    *out_w = w;
    *out_h = h;
    int transparent = -1;

    for (;;) {
        int b = gif_byte(&r);
        if (b == 0x21) {                       /* an extension */
            int label = gif_byte(&r);
            if (label == 0xF9) {               /* graphic control */
                int bl = gif_byte(&r);
                if (bl >= 4 && r.at + 4 <= len) {
                    if (data[r.at] & 1)
                        transparent = data[r.at + 3];
                    r.at += (unsigned)bl;
                }
            }
            gif_skip_blocks(&r);
            continue;
        }
        if (b != 0x2C) {                       /* the trailer, or the end */
            *why = "no picture in it";
            return -1;
        }
        int left = gif_u16(&r), top = gif_u16(&r);
        int fw = gif_u16(&r), fh = gif_u16(&r), f = gif_byte(&r);
        if (f < 0 || fw <= 0 || fh <= 0) {
            *why = "cut short in a picture's header";
            return -1;
        }
        int psize = gsize;
        const unsigned char *p = pal;
        unsigned char local[768];
        if (f & 0x80) {
            psize = 2 << (f & 7);
            if (r.at + (unsigned)psize * 3 > len) {
                *why = "cut short in a palette";
                return -1;
            }
            for (int i = 0; i < psize * 3; i++)
                local[i] = data[r.at + i];
            r.at += (unsigned)psize * 3;
            p = local;
        }
        int interlaced = f & 0x40;
        int min = gif_byte(&r);
        if (min < 2 || min > 8) {
            *why = "a code size out of range";
            return -1;
        }

        /* The LZW stream, read through the sub-blocks bit by bit */
        int clear = 1 << min, eoi = clear + 1;
        int size = min + 1, next = clear + 2, old = -1, first = 0;
        unsigned int bits = 0;
        int nbits = 0, block = 0;
        long pixel = 0, total = (long)fw * fh;
        int pass = 0, row = 0, col = 0;
        static const int start[4] = { 0, 4, 2, 1 }, step[4] = { 8, 8, 4, 2 };
        while (pixel < total) {
            while (nbits < size) {
                if (block == 0) {
                    block = gif_byte(&r);
                    if (block <= 0)
                        return 0;              /* cut short: what there is */
                }
                int c = gif_byte(&r);
                if (c < 0)
                    return 0;
                block--;
                bits |= (unsigned)c << nbits;
                nbits += 8;
            }
            int code = (int)(bits & ((1u << size) - 1));
            bits >>= size;
            nbits -= size;
            if (code == clear) {
                size = min + 1;
                next = clear + 2;
                old = -1;
                continue;
            }
            if (code == eoi)
                break;
            int sp = 0, in = code;
            if (old < 0) {
                if (code >= clear)
                    return 0;                  /* nonsense: stop here */
                stack[sp++] = (unsigned char)code;
                first = code;
            } else {
                if (code > next || (code == next && next >= 4096))
                    return 0;
                if (code == next) {            /* the string being made */
                    stack[sp++] = (unsigned char)first;
                    code = old;
                }
                while (code >= clear && sp < 4096) {
                    stack[sp++] = suffix[code];
                    code = prefix[code];
                }
                stack[sp++] = (unsigned char)code;
                first = code;
                if (next < 4096) {
                    prefix[next] = (unsigned short)old;
                    suffix[next] = (unsigned char)first;
                    next++;
                    if (next == (1 << size) && size < 12)
                        size++;
                }
            }
            old = in;
            while (sp > 0 && pixel < total) {
                int idx = stack[--sp];
                int x = left + col, y = top + (interlaced ? row : (int)(pixel / fw));
                if (!interlaced)
                    x = left + (int)(pixel % fw);
                if (x < w && y < h && idx < psize && idx != transparent) {
                    unsigned char *o = out + ((long)y * w + x) * 4;
                    o[0] = p[idx * 3];
                    o[1] = p[idx * 3 + 1];
                    o[2] = p[idx * 3 + 2];
                    o[3] = 255;
                }
                pixel++;
                if (++col == fw) {
                    col = 0;
                    if (interlaced) {
                        row += step[pass];
                        while (row >= fh && pass < 3)
                            row = start[++pass];
                    }
                }
            }
        }
        return 0;                              /* the first frame is enough */
    }
}

#endif
