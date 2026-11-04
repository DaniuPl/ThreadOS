/* uwoff - a WOFF font unpacked into the TrueType file it wraps
 *
 * WOFF (W3C, 2012) is how sites serve fonts: the tables of an sfnt
 * (TrueType or OpenType) file, each compressed with zlib on its own,
 * behind a header of their own. Unpacking it is building that file
 * again: the 12-byte offset table, a 16-byte record for each table, and
 * the tables, each padded to four bytes - what ttf_open() reads.
 *
 * Every length and offset is checked against what there is; a font is
 * a file off the network.
 */

#ifndef UWOFF_H
#define UWOFF_H

#include <stdint.h>
#include "uinflate.h"

static inline uint32_t woff_u32(const unsigned char *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static inline uint16_t woff_u16(const unsigned char *p)
{
    return (uint16_t)(p[0] << 8 | p[1]);
}

static inline void woff_put32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

static inline void woff_put16(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v >> 8);
    p[1] = (unsigned char)v;
}

/* Is this a WOFF file at all */
static inline int woff_is(const unsigned char *in, uint32_t len)
{
    return len >= 44 && in[0] == 'w' && in[1] == 'O' && in[2] == 'F' && in[3] == 'F';
}

/* The size of the sfnt file it holds - what woff_unpack() needs room
   for; 0 when the header does not hold together */
static inline uint32_t woff_sfnt_size(const unsigned char *in, uint32_t len)
{
    if (!woff_is(in, len))
        return 0;
    uint32_t n = woff_u16(in + 12), total = woff_u32(in + 16);
    if (!n || n > 512 || 44 + 20 * n > len || total > (64u << 20) ||
        total < 12 + 16 * n)
        return 0;
    return total;
}

/* Unpacks it into out (woff_sfnt_size() bytes). The size of the file
   made, or 0 when the WOFF is damaged. */
static inline uint32_t woff_unpack(const unsigned char *in, uint32_t len,
                                   unsigned char *out, uint32_t cap)
{
    uint32_t total = woff_sfnt_size(in, len);
    if (!total || cap < total)
        return 0;
    uint32_t n = woff_u16(in + 12);
    uint32_t pos = 12 + 16 * n;          /* where the first table goes */
    int sel = 0;
    while ((2u << sel) <= n)
        sel++;
    woff_put32(out, woff_u32(in + 4));   /* the flavour: TrueType's or CFF's */
    woff_put16(out + 4, n);
    woff_put16(out + 6, 16u << sel);
    woff_put16(out + 8, (uint32_t)sel);
    woff_put16(out + 10, n * 16 - (16u << sel));
    for (uint32_t i = 0; i < n; i++) {
        const unsigned char *e = in + 44 + 20 * i;
        uint32_t off = woff_u32(e + 4), clen = woff_u32(e + 8);
        uint32_t olen = woff_u32(e + 12);
        if (off > len || clen > len - off || olen > total || pos > total - olen ||
            clen > olen)
            return 0;
        unsigned char *rec = out + 12 + 16 * i;
        for (int k = 0; k < 4; k++)
            rec[k] = e[k];               /* the tag */
        woff_put32(rec + 4, woff_u32(e + 16));
        woff_put32(rec + 8, pos);
        woff_put32(rec + 12, olen);
        if (clen == olen) {
            for (uint32_t k = 0; k < olen; k++)
                out[pos + k] = in[off + k];
        } else if (inflate_zlib(in + off, clen, out + pos, olen) != (int)olen) {
            return 0;
        }
        pos += olen;
        while (pos & 3) {
            if (pos >= total)
                break;
            out[pos++] = 0;
        }
    }
    return total;
}

#endif
