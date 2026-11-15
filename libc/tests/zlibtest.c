/* zlibtest - zlib on ThreadOS
 *
 * Plain C against zlib. The test builds the same program with the same
 * zlib on the host and compares the outputs line for line; Python's own
 * zlib and gzip (a different implementation) read the file this program
 * writes and make the one it reads.
 *
 *   zlibtest [FILE.gz]    FILE.gz: a gzip file made elsewhere, to read
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define SIZE 300000

static unsigned char *make_data(void)
{
    unsigned char *d = malloc(SIZE);
    static const char *words[] = { "thread", "kernel", "futex", "socket",
                                   "inode", "window", "glyph", "page" };
    size_t n = 0;
    unsigned x = 12345;
    while (n < SIZE) {
        x = x * 1103515245u + 12345u;
        if ((x >> 16) % 5) {          /* text, which compresses */
            int k = snprintf((char *)d + n, SIZE - n, "%s %u, ",
                             words[(x >> 8) % 8], (x >> 12) % 1000);
            n += k > 0 ? (size_t)k : 0;
        } else {                       /* noise, which does not */
            for (int i = 0; i < 40 && n < SIZE; i++) {
                x = x * 1103515245u + 12345u;
                d[n++] = (unsigned char)(x >> 16);
            }
        }
    }
    return d;
}

/* deflate and inflate through small buffers, as a stream would come */
static int stream_round(const unsigned char *data, size_t len, int bits,
                        unsigned long *packed_len, unsigned long *packed_crc)
{
    z_stream z;
    memset(&z, 0, sizeof(z));
    if (deflateInit2(&z, 6, Z_DEFLATED, bits, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        return -1;
    size_t cap = compressBound(len) + 64;
    unsigned char *out = malloc(cap);
    size_t in_at = 0;
    int r;
    do {
        size_t chunk = len - in_at < 1000 ? len - in_at : 1000;
        z.next_in = (unsigned char *)data + in_at;
        z.avail_in = chunk;
        in_at += chunk;
        int flush = in_at == len ? Z_FINISH : Z_NO_FLUSH;
        do {
            z.next_out = out + z.total_out;
            z.avail_out = cap - z.total_out < 777 ? cap - z.total_out : 777;
            r = deflate(&z, flush);
        } while (z.avail_out == 0 && r != Z_STREAM_END);
    } while (r != Z_STREAM_END);
    size_t plen = z.total_out;
    deflateEnd(&z);
    *packed_len = plen;
    *packed_crc = crc32(0, out, plen);

    unsigned char *back = malloc(len + 1);
    memset(&z, 0, sizeof(z));
    if (inflateInit2(&z, bits) != Z_OK)
        return -1;
    size_t at = 0;
    do {
        size_t chunk = plen - at < 333 ? plen - at : 333;
        z.next_in = out + at;
        z.avail_in = chunk;
        at += chunk;
        z.next_out = back + z.total_out;
        z.avail_out = len + 1 - z.total_out;
        r = inflate(&z, Z_NO_FLUSH);
    } while (r == Z_OK && at < plen);
    int ok = r == Z_STREAM_END && z.total_out == len && !memcmp(back, data, len);
    inflateEnd(&z);
    free(out);
    free(back);
    return ok;
}

int main(int argc, char **argv)
{
    printf("zlib %s\n", zlibVersion());
    printf("crc32 of 123456789: %08lx, adler32: %08lx\n",
           crc32(0, (const Bytef *)"123456789", 9),
           adler32(1, (const Bytef *)"123456789", 9));

    unsigned char *data = make_data();
    printf("data: %d bytes, crc32 %08lx\n", SIZE, crc32(0, data, SIZE));

    for (int level = 1; level <= 9; level += 4) {
        uLongf plen = compressBound(SIZE);
        unsigned char *packed = malloc(plen);
        int r = compress2(packed, &plen, data, SIZE, level);
        uLongf blen = SIZE;
        unsigned char *back = malloc(SIZE);
        int r2 = uncompress(back, &blen, packed, plen);
        printf("level %d: %d, %lu bytes, crc32 %08lx; back %d %s\n", level, r,
               (unsigned long)plen, crc32(0, packed, plen), r2,
               blen == SIZE && !memcmp(back, data, SIZE) ? "same" : "DIFFERENT");
        free(packed);
        free(back);
    }

    static const struct { const char *name; int bits; } kinds[] = {
        { "zlib", 15 }, { "raw", -15 }, { "gzip", 31 } };
    for (int k = 0; k < 3; k++) {
        unsigned long plen = 0, pcrc = 0;
        int ok = stream_round(data, SIZE, kinds[k].bits, &plen, &pcrc);
        printf("stream %s: %lu bytes, crc32 %08lx, back %s\n", kinds[k].name,
               plen, pcrc, ok == 1 ? "whole" : "DAMAGED");
    }

    /* A file: written with gzprintf and gzwrite, read back */
    gzFile g = gzopen("zt.gz", "wb9");
    for (int i = 0; i < 2000; i++)
        gzprintf(g, "line %d of a file a different gzip will read\n", i);
    gzwrite(g, data, 50000);
    printf("gzclose (write): %d\n", gzclose(g));
    g = gzopen("zt.gz", "rb");
    char line[100];
    int lines = 0;
    for (int i = 0; i < 2000 && gzgets(g, line, sizeof(line)); i++)
        lines++;
    unsigned char *tail = malloc(50000);
    int got = gzread(g, tail, 50000);
    printf("read back: %d lines, then %d bytes %s, at the end %d\n", lines, got,
           got == 50000 && !memcmp(tail, data, 50000) ? "same" : "DIFFERENT",
           gzeof(g));
    gzclose(g);

    if (argc > 1) {
        g = gzopen(argv[1], "rb");
        if (!g) {
            printf("~%s: cannot open\n", argv[1]);
            return 1;
        }
        unsigned long crc = crc32(0, NULL, 0), total = 0;
        unsigned char buf[4096];
        int n;
        while ((n = gzread(g, buf, sizeof(buf))) > 0) {
            crc = crc32(crc, buf, n);
            total += n;
        }
        gzclose(g);
        printf("~%s: %lu bytes, crc32 %08lx\n", argv[1], total, crc);
    }
    printf("done\n");
    return 0;
}
