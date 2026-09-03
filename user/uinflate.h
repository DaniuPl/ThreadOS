/* uinflate - deflate (RFC 1951) unpacked, for whoever needs it
 *
 * PNG needs it, and PNG is the reason it was written; gzip and zlib
 * streams are the same thing with a different wrapper, so this takes
 * the raw deflate stream and leaves the wrapper to the caller.
 *
 * The whole of deflate is three kinds of block:
 *   - stored: a length and that many bytes, unchanged;
 *   - fixed:  the code lengths are written in the standard, so the
 *             two trees are built from a table;
 *   - dynamic: the trees are written in the block itself, in a third
 *             tree that describes them.
 * and then a stream of symbols: a symbol under 256 is a byte, 256 ends
 * the block, and above that it means "copy N bytes from D back" - the
 * repetition that does the compressing.
 *
 * The output has to stay reachable, because a copy reaches back into
 * what was already written - up to 32 KiB back. The caller hands over
 * one buffer for the whole result, which for a picture is what it
 * wanted anyway.
 *
 * Canonical Huffman decoding is done the plain way: walk the bits, one
 * at a time, counting how many codes of each length there are. It is
 * slower than a lookup table and it is thirty lines instead of a
 * hundred and fifty; a picture takes a moment either way.
 */

#ifndef UINFLATE_H
#define UINFLATE_H

#include "uapi.h"

struct inf_tree {
    /* Canonical Huffman: how many codes of each length, and the
       symbols in the order the lengths put them in. */
    unsigned short count[16];
    unsigned short symbol[288];
};

struct inf_state {
    const unsigned char *in;
    unsigned int in_len, in_pos;
    unsigned int bitbuf, bitcnt;
    unsigned char *out;
    unsigned int out_len, out_pos;
    int error;                        /* 1 = the stream is broken */
};

static int inf_bit(struct inf_state *s)
{
    if (!s->bitcnt) {
        if (s->in_pos >= s->in_len) {
            s->error = 1;
            return 0;
        }
        s->bitbuf = s->in[s->in_pos++];
        s->bitcnt = 8;
    }
    int b = (int)(s->bitbuf & 1);
    s->bitbuf >>= 1;
    s->bitcnt--;
    return b;
}

static int inf_bits(struct inf_state *s, int n)
{
    int v = 0;
    for (int i = 0; i < n; i++)
        v |= inf_bit(s) << i;
    return v;
}

/* Builds the canonical tree from the code lengths - the only shape a
   deflate tree ever has, so the lengths are the whole description. */
static void inf_build(struct inf_tree *t, const unsigned char *len, int n)
{
    for (int i = 0; i < 16; i++)
        t->count[i] = 0;
    for (int i = 0; i < n; i++)
        t->count[len[i]]++;
    t->count[0] = 0;

    unsigned short offs[16];
    offs[0] = 0;
    offs[1] = 0;
    for (int i = 1; i < 15; i++)
        offs[i + 1] = (unsigned short)(offs[i] + t->count[i]);
    for (int i = 0; i < n; i++)
        if (len[i])
            t->symbol[offs[len[i]]++] = (unsigned short)i;
}

static int inf_decode(struct inf_state *s, const struct inf_tree *t)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        code |= inf_bit(s);
        int count = t->count[len];
        if (code - first < count)
            return t->symbol[index + (code - first)];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
        if (s->error)
            break;
    }
    s->error = 1;
    return -1;
}

/* The lengths and distances a symbol above 256 stands for */
static const unsigned short inf_len_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51,
    59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const unsigned char inf_len_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4,
    4, 5, 5, 5, 5, 0
};
static const unsigned short inf_dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385,
    513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
static const unsigned char inf_dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9,
    10, 10, 11, 11, 12, 12, 13, 13
};

static void inf_block(struct inf_state *s, const struct inf_tree *lt,
                      const struct inf_tree *dt)
{
    for (;;) {
        int sym = inf_decode(s, lt);
        if (s->error)
            return;
        if (sym < 256) {
            if (s->out_pos >= s->out_len) {
                s->error = 1;
                return;
            }
            s->out[s->out_pos++] = (unsigned char)sym;
            continue;
        }
        if (sym == 256)
            return;                    /* the end of this block */

        sym -= 257;
        if (sym >= 29) {
            s->error = 1;
            return;
        }
        int length = inf_len_base[sym] + inf_bits(s, inf_len_extra[sym]);
        int dsym = inf_decode(s, dt);
        if (s->error || dsym < 0 || dsym >= 30) {
            s->error = 1;
            return;
        }
        unsigned int dist = inf_dist_base[dsym] +
                            (unsigned int)inf_bits(s, inf_dist_extra[dsym]);
        if (dist > s->out_pos || s->out_pos + (unsigned int)length >
                                 s->out_len) {
            s->error = 1;
            return;
        }
        /* Byte by byte, and on purpose: the copy may overlap itself -
           "three bytes from one back" means the same byte three times,
           which is how a run is written. */
        unsigned int from = s->out_pos - dist;
        for (int i = 0; i < length; i++)
            s->out[s->out_pos++] = s->out[from + i];
    }
}

/* The two trees the standard writes down, for a block that says "use
   the usual ones" */
static void inf_fixed(struct inf_tree *lt, struct inf_tree *dt)
{
    static unsigned char len[288];
    for (int i = 0; i < 144; i++) len[i] = 8;
    for (int i = 144; i < 256; i++) len[i] = 9;
    for (int i = 256; i < 280; i++) len[i] = 7;
    for (int i = 280; i < 288; i++) len[i] = 8;
    inf_build(lt, len, 288);

    static unsigned char dlen[30];
    for (int i = 0; i < 30; i++) dlen[i] = 5;
    inf_build(dt, dlen, 30);
}

/* The trees written into the block itself, described by a third tree */
static void inf_dynamic(struct inf_state *s, struct inf_tree *lt,
                        struct inf_tree *dt)
{
    static const unsigned char order[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };
    int nlen = inf_bits(s, 5) + 257;
    int ndist = inf_bits(s, 5) + 1;
    int ncode = inf_bits(s, 4) + 4;
    if (nlen > 288 || ndist > 30) {
        s->error = 1;
        return;
    }

    static unsigned char lengths[288 + 30];
    for (int i = 0; i < 19; i++)
        lengths[i] = 0;
    for (int i = 0; i < ncode; i++)
        lengths[order[i]] = (unsigned char)inf_bits(s, 3);

    struct inf_tree code_tree;
    inf_build(&code_tree, lengths, 19);

    int n = 0;
    while (n < nlen + ndist) {
        int sym = inf_decode(s, &code_tree);
        if (s->error)
            return;
        if (sym < 16) {
            lengths[n++] = (unsigned char)sym;
        } else if (sym == 16) {          /* the previous one, 3..6 times */
            if (!n) {
                s->error = 1;
                return;
            }
            unsigned char prev = lengths[n - 1];
            int rep = 3 + inf_bits(s, 2);
            while (rep-- && n < nlen + ndist)
                lengths[n++] = prev;
        } else if (sym == 17) {          /* zeros, 3..10 */
            int rep = 3 + inf_bits(s, 3);
            while (rep-- && n < nlen + ndist)
                lengths[n++] = 0;
        } else {                         /* zeros, 11..138 */
            int rep = 11 + inf_bits(s, 7);
            while (rep-- && n < nlen + ndist)
                lengths[n++] = 0;
        }
    }

    inf_build(lt, lengths, nlen);
    inf_build(dt, lengths + nlen, ndist);
}

/* Unpacks a raw deflate stream into out. Returns how many bytes came
   out, or -1 when the stream does not hold together. */
static int inflate_raw(const unsigned char *in, unsigned int in_len,
                       unsigned char *out, unsigned int out_len)
{
    static struct inf_state s;
    s.in = in;
    s.in_len = in_len;
    s.in_pos = 0;
    s.bitbuf = 0;
    s.bitcnt = 0;
    s.out = out;
    s.out_len = out_len;
    s.out_pos = 0;
    s.error = 0;

    static struct inf_tree lt, dt;
    int last = 0;
    while (!last && !s.error) {
        last = inf_bit(&s);
        int type = inf_bits(&s, 2);
        if (type == 0) {                 /* stored */
            s.bitcnt = 0;                /* the length is byte-aligned */
            if (s.in_pos + 4 > s.in_len) {
                s.error = 1;
                break;
            }
            unsigned int len = (unsigned int)s.in[s.in_pos] |
                               ((unsigned int)s.in[s.in_pos + 1] << 8);
            s.in_pos += 4;               /* the length and its complement */
            if (s.in_pos + len > s.in_len ||
                s.out_pos + len > s.out_len) {
                s.error = 1;
                break;
            }
            for (unsigned int i = 0; i < len; i++)
                s.out[s.out_pos++] = s.in[s.in_pos++];
            continue;
        }
        if (type == 1)
            inf_fixed(&lt, &dt);
        else if (type == 2)
            inf_dynamic(&s, &lt, &dt);
        else {
            s.error = 1;
            break;
        }
        if (!s.error)
            inf_block(&s, &lt, &dt);
    }
    return s.error ? -1 : (int)s.out_pos;
}

/* A zlib stream (RFC 1950) is deflate with two bytes in front and a
   checksum behind; PNG uses that wrapper. */
static int inflate_zlib(const unsigned char *in, unsigned int in_len,
                        unsigned char *out, unsigned int out_len)
{
    if (in_len < 2 || (in[0] & 0x0F) != 8)
        return -1;                       /* not deflate */
    if (in[1] & 0x20)
        return -1;                       /* a preset dictionary - not here */
    return inflate_raw(in + 2, in_len - 2, out, out_len);
}

#endif
