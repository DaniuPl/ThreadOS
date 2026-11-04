/* ubrotli - Brotli (RFC 7932) unpacked, for whoever needs it
 *
 * WOFF2 fonts are Brotli streams, and that is why this was written. A
 * stream is a window size and then meta-blocks; a compressed meta-block
 * is, after a header of prefix codes, a list of commands, each "insert
 * these literals, then copy this many bytes from that far back" - like
 * deflate, but with more of everything:
 *
 *   - three alphabets (literals, insert-and-copy lengths, distances),
 *     each split into blocks of a "type", every type with its own codes;
 *   - a literal's code chosen also by the two bytes before it (its
 *     context), through a context map;
 *   - distances that can name one of the last four again, or a word of
 *     a built-in dictionary, changed by one of 121 transforms - the
 *     dictionary is data from the RFC (third_party/brotli-rfc7932).
 *
 * The whole output stays in one buffer the caller gives, so a copy just
 * reaches back into it. Prefix codes are decoded a bit at a time, as in
 * uinflate.h: slower than tables, and a font is small.
 *
 * Every read is checked: a stream comes off the network. A bad one is
 * refused (-1), never followed past the buffers' ends.
 */

#ifndef UBROTLI_H
#define UBROTLI_H

#include <stdint.h>
#include "ubrotlidata.h"     /* made at build time: scripts/mkbrotlidata.py */

struct br_in {
    const unsigned char *p;
    uint32_t len, pos;       /* pos in bits */
    int err;
};

static uint32_t br_bits(struct br_in *b, int n)
{
    uint32_t v = 0;
    for (int i = 0; i < n; i++) {
        uint32_t byte = b->pos >> 3;
        if (byte >= b->len) {
            b->err = 1;
            return 0;
        }
        v |= (uint32_t)(b->p[byte] >> (b->pos & 7) & 1) << i;
        b->pos++;
    }
    return v;
}

/* --- prefix codes ------------------------------------------------------ */

#define BR_MAXSYM 704
struct br_code {
    uint16_t count[16];          /* how many codes of each length */
    uint16_t sym[BR_MAXSYM];     /* the symbols, by length, then value */
    int single;                  /* one symbol, no bits: it, or -1 */
};

static int br_build(struct br_code *c, const uint8_t *lens, int n)
{
    uint16_t offs[16];
    int nonzero = 0, last = 0;
    for (int i = 0; i < 16; i++)
        c->count[i] = 0;
    for (int i = 0; i < n; i++) {
        c->count[lens[i]]++;
        if (lens[i]) {
            nonzero++;
            last = i;
        }
    }
    c->count[0] = 0;
    c->single = nonzero == 1 ? last : -1;
    offs[1] = 0;
    for (int i = 1; i < 15; i++)
        offs[i + 1] = (uint16_t)(offs[i] + c->count[i]);
    for (int i = 0; i < n; i++)
        if (lens[i])
            c->sym[offs[lens[i]]++] = (uint16_t)i;
    return nonzero ? 0 : -1;
}

static int br_decode(struct br_in *b, const struct br_code *c)
{
    if (c->single >= 0)
        return c->single;
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        code |= (int)br_bits(b, 1);
        int count = c->count[len];
        if (code - count < first)
            return c->sym[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
        if (b->err)
            return -1;
    }
    b->err = 1;
    return -1;
}

static int br_bitlen(int n)
{
    int bits = 0;
    while ((1 << bits) < n)
        bits++;
    return bits;
}

/* A prefix code over 'alpha' symbols, as RFC 7932 3.4 and 3.5 write it */
static int br_read_code(struct br_in *b, struct br_code *c, int alpha)
{
    static uint8_t lens[BR_MAXSYM];
    for (int i = 0; i < alpha; i++)
        lens[i] = 0;
    uint32_t hskip = br_bits(b, 2);
    if (hskip == 1) {
        /* simple: up to four symbols, their lengths fixed by how many */
        int nsym = (int)br_bits(b, 2) + 1, bits = br_bitlen(alpha), s[4];
        for (int i = 0; i < nsym; i++) {
            s[i] = (int)br_bits(b, bits);
            if (s[i] >= alpha)
                return -1;
            for (int j = 0; j < i; j++)
                if (s[j] == s[i])
                    return -1;
        }
        static const uint8_t l2[2] = {1, 1}, l3[3] = {1, 2, 2}, l4a[4] = {2, 2, 2, 2},
                             l4b[4] = {1, 2, 3, 3};
        const uint8_t *l = nsym == 2 ? l2 : nsym == 3 ? l3 : l4a;
        if (nsym == 4 && br_bits(b, 1))
            l = l4b;
        if (nsym == 1) {
            lens[s[0]] = 1;              /* (one symbol: no bits read for it) */
        } else {
            for (int i = 0; i < nsym; i++)
                lens[s[i]] = l[i];
        }
        return b->err ? -1 : br_build(c, lens, alpha);
    }
    /* complex: the lengths of the code-length code first, in a fixed
       order, each in a small code of its own (RFC 7932, 3.5) */
    static const uint8_t order[18] = {1, 2, 3, 4, 0, 5, 17, 6, 16, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    static const uint8_t pre_len[16] = {2, 2, 2, 3, 2, 2, 2, 4, 2, 2, 2, 3, 2, 2, 2, 4};
    static const uint8_t pre_val[16] = {0, 4, 3, 2, 0, 4, 3, 1, 0, 4, 3, 2, 0, 4, 3, 5};
    uint8_t cl[18] = {0};
    int space = 32, num = 0;
    for (int i = (int)hskip; i < 18 && space > 0; i++) {
        uint32_t save = b->pos;
        uint32_t peek = br_bits(b, 4);
        if (b->err) {
            /* fewer than four bits left: peek what there is */
            b->err = 0;
            b->pos = save;
            peek = 0;
            for (int k = 0; k < 4 && (b->pos >> 3) < b->len; k++)
                peek |= br_bits(b, 1) << k;
            b->pos = save;
        } else {
            b->pos = save;
        }
        b->pos += pre_len[peek & 15];
        int v = pre_val[peek & 15];
        cl[order[i]] = (uint8_t)v;
        if (v) {
            space -= 32 >> v;
            num++;
        }
    }
    if (b->err || (num != 1 && space != 0))
        return -1;
    struct br_code clc;
    if (br_build(&clc, cl, 18) < 0)
        return -1;
    int sym = 0, prev = 8, repeat = 0, repeat_len = 0;
    int space2 = 32768;
    while (sym < alpha && space2 > 0) {
        int v = br_decode(b, &clc);
        if (v < 0 || b->err)
            return -1;
        if (v < 16) {
            repeat = 0;
            lens[sym++] = (uint8_t)v;
            if (v) {
                prev = v;
                space2 -= 32768 >> v;
            }
            continue;
        }
        int extra = v == 16 ? 2 : 3;
        int newlen = v == 16 ? prev : 0;
        if (repeat_len != newlen) {
            repeat = 0;
            repeat_len = newlen;
        }
        int old = repeat;
        if (repeat > 0)
            repeat = (repeat - 2) << extra;
        repeat += (int)br_bits(b, extra) + 3;
        int delta = repeat - old;
        if (sym + delta > alpha)
            return -1;
        for (int k = 0; k < delta; k++)
            lens[sym++] = (uint8_t)newlen;
        if (newlen)
            space2 -= delta << (15 - newlen);
    }
    if (space2 != 0 || b->err)
        return -1;
    return br_build(c, lens, alpha);
}

/* --- the parts of a meta-block's header -------------------------------- */

static uint32_t br_varlen8(struct br_in *b)
{
    if (!br_bits(b, 1))
        return 0;
    uint32_t n = br_bits(b, 3);
    return n ? br_bits(b, (int)n) + (1u << n) : 1;
}

static const uint16_t br_blen_base[26] = {1, 5, 9, 13, 17, 25, 33, 41, 49, 65, 81, 97, 113,
                                         145, 177, 209, 241, 305, 369, 497, 753, 1265,
                                         2289, 4337, 8433, 16625};
static const uint8_t br_blen_bits[26] = {2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5,
                                        5, 5, 5, 6, 6, 7, 8, 9, 10, 11, 12, 13, 24};

/* The blocks of one alphabet: how many types, the codes for switching,
   the current type and how much of its block is left */
struct br_blocks {
    uint32_t types;
    struct br_code type_code, len_code;
    uint32_t type, prev, left;
};

static int br_block_len(struct br_in *b, const struct br_code *c, uint32_t *out)
{
    int s = br_decode(b, c);
    if (s < 0 || s >= 26)
        return -1;
    *out = br_blen_base[s] + br_bits(b, br_blen_bits[s]);
    return b->err ? -1 : 0;
}

static int br_read_blocks(struct br_in *b, struct br_blocks *k)
{
    k->types = br_varlen8(b) + 1;
    k->type = 0;
    k->prev = 1;
    k->left = 1u << 24;
    if (k->types < 2)
        return b->err ? -1 : 0;
    if (br_read_code(b, &k->type_code, (int)k->types + 2) < 0 ||
        br_read_code(b, &k->len_code, 26) < 0)
        return -1;
    return br_block_len(b, &k->len_code, &k->left);
}

static int br_switch(struct br_in *b, struct br_blocks *k)
{
    int s = br_decode(b, &k->type_code);
    if (s < 0)
        return -1;
    uint32_t t = s == 0 ? k->prev : s == 1 ? (k->type + 1) % k->types : (uint32_t)s - 2;
    if (t >= k->types)
        return -1;
    k->prev = k->type;
    k->type = t;
    return br_block_len(b, &k->len_code, &k->left);
}

static int br_context_map(struct br_in *b, uint8_t *map, uint32_t size, uint32_t trees)
{
    uint32_t rlemax = br_bits(b, 1) ? br_bits(b, 4) + 1 : 0;
    static struct br_code c;
    if (br_read_code(b, &c, (int)(trees + rlemax)) < 0)
        return -1;
    for (uint32_t i = 0; i < size;) {
        int s = br_decode(b, &c);
        if (s < 0)
            return -1;
        if (s == 0) {
            map[i++] = 0;
        } else if ((uint32_t)s <= rlemax) {
            uint32_t n = (1u << s) + br_bits(b, s);
            if (i + n > size)
                return -1;
            while (n--)
                map[i++] = 0;
        } else {
            map[i++] = (uint8_t)(s - (int)rlemax);
        }
        if (b->err)
            return -1;
    }
    if (br_bits(b, 1)) {
        /* inverse move-to-front */
        uint8_t mtf[256];
        for (int i = 0; i < 256; i++)
            mtf[i] = (uint8_t)i;
        for (uint32_t i = 0; i < size; i++) {
            uint8_t idx = map[i], v = mtf[idx];
            map[i] = v;
            for (; idx; idx--)
                mtf[idx] = mtf[idx - 1];
            mtf[0] = v;
        }
    }
    for (uint32_t i = 0; i < size; i++)
        if (map[i] >= trees)
            return -1;
    return b->err ? -1 : 0;
}

/* --- the dictionary ---------------------------------------------------- */

static const uint8_t br_ndbits[25] = {0, 0, 0, 0, 10, 10, 11, 11, 10, 10, 10, 10, 10,
                                     9, 9, 8, 7, 7, 8, 7, 7, 6, 6, 5, 5};

static uint32_t br_dict_offset(int len)
{
    uint32_t off = 0;
    for (int l = 4; l < len; l++)
        off += (uint32_t)l << br_ndbits[l];
    return off;
}

/* RFC 7932, 8: upper case for one UTF-8 character, its length */
static int br_upper(unsigned char *p, int n)
{
    if (p[0] < 0xC0) {
        if (p[0] >= 'a' && p[0] <= 'z')
            p[0] ^= 32;
        return 1;
    }
    if (p[0] < 0xE0) {
        if (n > 1)
            p[1] ^= 32;
        return 2;
    }
    if (n > 2)
        p[2] ^= 5;
    return 3;
}

/* A dictionary word, transformed, onto the output. Its length, or -1 */
static int br_word(unsigned char *out, uint32_t room, int len, uint32_t word_id)
{
    int nbits = br_ndbits[len];
    uint32_t idx = word_id & ((1u << nbits) - 1), t = word_id >> nbits;
    if (t >= 121)
        return -1;
    const unsigned char *word = brotli_dictionary + br_dict_offset(len) + idx * (uint32_t)len;
    const unsigned char *pre = brotli_affixes + brotli_transforms[t * 3];
    int kind = brotli_transforms[t * 3 + 1];
    const unsigned char *suf = brotli_affixes + brotli_transforms[t * 3 + 2];
    uint32_t total = (uint32_t)pre[0] + (uint32_t)len + suf[0];
    if (total > room)
        return -1;
    int o = 0;
    for (int i = 0; i < pre[0]; i++)
        out[o++] = pre[1 + i];
    int skip = kind >= 12 ? kind - 11 : 0, cut = kind >= 1 && kind <= 9 ? kind : 0;
    if (skip > len)
        skip = len;
    int start = o, n = len - skip - cut;
    for (int i = 0; i < n; i++)
        out[o++] = word[skip + i];
    if (kind == 10 && n > 0) {
        br_upper(out + start, n);
    } else if (kind == 11) {
        for (int i = 0; i < n;)
            i += br_upper(out + start + i, n - i);
    }
    for (int i = 0; i < suf[0]; i++)
        out[o++] = suf[1 + i];
    return o;
}

/* --- the stream ---------------------------------------------------------- */

static const uint32_t br_ins_base[24] = {0, 1, 2, 3, 4, 5, 6, 8, 10, 14, 18, 26, 34, 50,
                                        66, 98, 130, 194, 322, 578, 1090, 2114, 6210, 22594};
static const uint8_t br_ins_bits[24] = {0, 0, 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4,
                                       5, 5, 6, 7, 8, 9, 10, 12, 14, 24};
static const uint32_t br_copy_base[24] = {2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 14, 18, 22, 30,
                                         38, 54, 70, 102, 134, 198, 326, 582, 1094, 2118};
static const uint8_t br_copy_bits[24] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 2, 2, 3, 3,
                                        4, 4, 5, 5, 6, 7, 8, 9, 10, 24};

#define BR_MAXTREES 256

/* Unpacks a Brotli stream into out. How many bytes came out, or -1. */
static int brotli_decode(const unsigned char *in, uint32_t in_len, unsigned char *out,
                         uint32_t out_cap)
{
    struct br_in b = {in, in_len, 0, 0};
    uint32_t wbits = 16;
    if (br_bits(&b, 1)) {
        uint32_t n = br_bits(&b, 3);
        if (n) {
            wbits = 17 + n;
        } else {
            n = br_bits(&b, 3);
            if (n == 1)
                return -1;               /* the large-window extension: not RFC 7932 */
            wbits = n ? 8 + n : 17;
        }
    }
    uint32_t window = (1u << wbits) - 16;
    uint32_t o = 0;
    int dist[4] = {16, 15, 11, 4};       /* the last distances; [3] the last */
    static struct br_blocks blk[3];
    static struct br_code lit[BR_MAXTREES], cmd[BR_MAXTREES], dcode[BR_MAXTREES];
    static uint8_t lmap[64 * BR_MAXTREES], dmap[4 * BR_MAXTREES], modes[BR_MAXTREES];

    for (;;) {
        uint32_t last = br_bits(&b, 1);
        if (last && br_bits(&b, 1))
            break;                       /* last, and empty */
        uint32_t nib = br_bits(&b, 2);
        if (nib == 3) {
            /* metadata: skipped */
            if (br_bits(&b, 1))
                return -1;
            uint32_t nbytes = br_bits(&b, 2), skip = 0;
            for (uint32_t i = 0; i < nbytes; i++)
                skip |= br_bits(&b, 8) << (8 * i);
            if (nbytes)
                skip++;
            b.pos = (b.pos + 7) & ~7u;
            b.pos += skip * 8;
            if (b.err || (b.pos >> 3) > b.len)
                return -1;
            if (last)
                break;
            continue;
        }
        uint32_t mlen = br_bits(&b, (int)(nib + 4) * 4) + 1;
        if (b.err || mlen > out_cap - o)
            return -1;
        if (!last && br_bits(&b, 1)) {
            /* uncompressed */
            b.pos = (b.pos + 7) & ~7u;
            if ((b.pos >> 3) + mlen > b.len)
                return -1;
            for (uint32_t i = 0; i < mlen; i++)
                out[o++] = in[(b.pos >> 3) + i];
            b.pos += mlen * 8;
            continue;
        }
        for (int k = 0; k < 3; k++)
            if (br_read_blocks(&b, &blk[k]) < 0)
                return -1;
        if (blk[0].types > BR_MAXTREES || blk[1].types > BR_MAXTREES ||
            blk[2].types > BR_MAXTREES)
            return -1;
        uint32_t npostfix = br_bits(&b, 2);
        uint32_t ndirect = br_bits(&b, 4) << npostfix;
        for (uint32_t i = 0; i < blk[0].types; i++)
            modes[i] = (uint8_t)br_bits(&b, 2);
        uint32_t ntl = br_varlen8(&b) + 1;
        if (ntl > BR_MAXTREES)
            return -1;
        if (ntl >= 2) {
            if (br_context_map(&b, lmap, 64 * blk[0].types, ntl) < 0)
                return -1;
        } else {
            for (uint32_t i = 0; i < 64 * blk[0].types; i++)
                lmap[i] = 0;
        }
        uint32_t ntd = br_varlen8(&b) + 1;
        if (ntd > BR_MAXTREES)
            return -1;
        if (ntd >= 2) {
            if (br_context_map(&b, dmap, 4 * blk[2].types, ntd) < 0)
                return -1;
        } else {
            for (uint32_t i = 0; i < 4 * blk[2].types; i++)
                dmap[i] = 0;
        }
        int dalpha = 16 + (int)ndirect + (48 << npostfix);
        if (dalpha > BR_MAXSYM)
            return -1;
        for (uint32_t i = 0; i < ntl; i++)
            if (br_read_code(&b, &lit[i], 256) < 0)
                return -1;
        for (uint32_t i = 0; i < blk[1].types; i++)
            if (br_read_code(&b, &cmd[i], 704) < 0)
                return -1;
        for (uint32_t i = 0; i < ntd; i++)
            if (br_read_code(&b, &dcode[i], dalpha) < 0)
                return -1;

        uint32_t end = o + mlen;
        while (o < end) {
            if (!blk[1].left && br_switch(&b, &blk[1]) < 0)
                return -1;
            blk[1].left--;
            int s = br_decode(&b, &cmd[blk[1].type]);
            if (s < 0)
                return -1;
            /* the insert-and-copy symbol's cell (RFC 7932, 5) */
            static const uint8_t ins_cell[11] = {0, 0, 0, 0, 8, 8, 0, 16, 8, 16, 16};
            static const uint8_t copy_cell[11] = {0, 8, 0, 8, 0, 8, 16, 0, 16, 8, 16};
            int cell = s >> 6, implicit = s < 128;
            int ic = ins_cell[cell] + ((s >> 3) & 7), cc = copy_cell[cell] + (s & 7);
            uint32_t ins = br_ins_base[ic] + br_bits(&b, br_ins_bits[ic]);
            uint32_t copy = br_copy_base[cc] + br_bits(&b, br_copy_bits[cc]);
            if (b.err || ins > end - o)
                return -1;
            for (uint32_t i = 0; i < ins; i++) {
                if (!blk[0].left && br_switch(&b, &blk[0]) < 0)
                    return -1;
                blk[0].left--;
                uint8_t p1 = o ? out[o - 1] : 0, p2 = o > 1 ? out[o - 2] : 0;
                const uint8_t *lut = brotli_context + (modes[blk[0].type] << 9);
                int ctx = lut[p1] | lut[256 + p2];
                int v = br_decode(&b, &lit[lmap[blk[0].type * 64 + ctx]]);
                if (v < 0)
                    return -1;
                out[o++] = (unsigned char)v;
            }
            if (o >= end)
                break;
            int d;
            int push = 1;
            if (implicit) {
                d = dist[3];
                push = 0;
            } else {
                if (!blk[2].left && br_switch(&b, &blk[2]) < 0)
                    return -1;
                blk[2].left--;
                int dctx = copy > 4 ? 3 : (int)copy - 2;
                int dc = br_decode(&b, &dcode[dmap[blk[2].type * 4 + dctx]]);
                if (dc < 0)
                    return -1;
                if (dc < 16) {
                    static const int8_t which[16] = {3, 2, 1, 0, 3, 3, 3, 3, 3, 3, 2, 2, 2, 2, 2, 2};
                    static const int8_t delta[16] = {0, 0, 0, 0, -1, 1, -2, 2, -3, 3,
                                                     -1, 1, -2, 2, -3, 3};
                    d = dist[which[dc]] + delta[dc];
                    if (dc == 0)
                        push = 0;
                    if (d <= 0)
                        return -1;
                } else if ((uint32_t)dc < 16 + ndirect) {
                    d = dc - 15;
                } else {
                    uint32_t x = (uint32_t)dc - ndirect - 16;
                    uint32_t ndist = 1 + (x >> (npostfix + 1));
                    uint32_t hcode = x >> npostfix, lcode = x & ((1u << npostfix) - 1);
                    uint32_t off = ((2 + (hcode & 1)) << ndist) - 4;
                    d = (int)(((off + br_bits(&b, (int)ndist)) << npostfix) + lcode + ndirect + 1);
                }
            }
            if (b.err)
                return -1;
            uint32_t max = o < window ? o : window;
            if ((uint32_t)d > max) {
                /* past what there is: a word of the dictionary */
                if (copy < 4 || copy > 24)
                    return -1;
                int n = br_word(out + o, end - o, (int)copy, (uint32_t)d - max - 1);
                if (n < 0)
                    return -1;
                o += (uint32_t)n;
                continue;                /* (and the last distances stay) */
            }
            if (push) {
                dist[0] = dist[1];
                dist[1] = dist[2];
                dist[2] = dist[3];
                dist[3] = d;
            }
            if (copy > end - o)
                return -1;
            for (uint32_t i = 0; i < copy; i++, o++)
                out[o] = out[o - (uint32_t)d];
        }
        if (last)
            break;
    }
    return b.err ? -1 : (int)o;
}

#endif
