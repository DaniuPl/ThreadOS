/* Putting ThreadOS's parsers through damaged data
 *
 * The certificate and HTML parsers get their data straight off the
 * network, so they have to stand any rubbish at all. This program runs
 * them on the host - with the address and undefined-behaviour
 * sanitizers - on input given in a file or on stdin. The script
 * scripts/fuzz_parsers.py feeds it thousands of cases made by damaging
 * data that was correct.
 *
 * Usage: harness <x509|html|http|ecdsa|inflate|jpeg> <file>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ux509.h"
#include "uecdsa.h"
#include "uhtml.h"
#include "uinflate.h"
#include "ujpeg.h"

#define MAX_INPUT (1 << 20)

/* The input goes into a buffer of exactly the length of the data. If
   it sat in a big static array, a read a few bytes past the end of the
   data would fit inside it and the sanitizer would notice nothing - and
   those are exactly the bugs we are here to catch. */
static unsigned char scratch[MAX_INPUT];

/* --- HTTP: taken out of uhttp.h, so as not to pull all of TLS in --- */

static int http_line_scan(const char *hdr, int hlen)
{
    int status = 0, found = 0;

    if (hlen > 12)
        status = (hdr[9] - '0') * 100 + (hdr[10] - '0') * 10 + (hdr[11] - '0');

    for (int p = 0; p < hlen;) {
        int q = p;
        while (q < hlen && hdr[q] != '\r')
            q++;
        if (q + 1 >= hlen)
            break;
        found++;
        p = q + 2;
    }
    return status + found;
}

static void run_x509(const unsigned char *data, size_t len)
{
    struct x509_cert c;
    if (x509_parse(&c, data, (uint32_t)len) < 0)
        return;

    /* Touch every field the rest of the system reads */
    volatile int sink = c.sig_alg + c.key_type + c.is_ca + c.ec_bits;
    (void)sink;
    x509_matches_host(&c, "threados.test");
    x509_matches_host(&c, "*.example.com");
    if (c.tbs && c.tbs_len)
        (void)c.tbs[c.tbs_len - 1];
    if (c.sig && c.sig_len)
        (void)c.sig[c.sig_len - 1];
    if (c.mod && c.mod_len)
        (void)c.mod[c.mod_len - 1];
    if (c.ec_point && c.ec_point_len)
        (void)c.ec_point[c.ec_point_len - 1];
}

static void run_html(const unsigned char *data, size_t len)
{
    struct html_doc d;
    int cap = (int)(len < MAX_INPUT / 2 ? len + 2 : MAX_INPUT);
    char *outbuf = malloc(cap);     /* to the byte as well, with nothing spare */

    memset(&d, 0, sizeof(d));
    d.src = (const char *)data;
    d.src_len = (int)len;
    d.text = outbuf;
    d.text_max = cap;

    html_extract(&d);
    html_layout(&d, 38);

    for (int i = 0; i < d.line_count; i++) {
        struct html_line *l = &d.lines[i];
        if (l->start < 0 || l->len < 0 || l->start + l->len > d.text_len) {
            fprintf(stderr, "a line outside the text: %d+%d > %d\n",
                    l->start, l->len, d.text_len);
            abort();
        }
        for (int k = 0; k < l->len; k++)
            (void)html_link_at(&d, l->start + k);
    }
    for (int i = 0; i < d.link_count; i++) {
        struct html_link *k = &d.links[i];
        if (k->start < 0 || k->end > d.text_len || k->start > k->end) {
            fprintf(stderr, "odnosnik poza tekstem: %d..%d (tekst %d)\n",
                    k->start, k->end, d.text_len);
            abort();
        }
    }
    free(outbuf);
}

static void run_http(const unsigned char *data, size_t len)
{
    volatile int r = http_line_scan((const char *)data, (int)len);
    (void)r;
}

static void run_ecdsa(const unsigned char *data, size_t len)
{
    /* The public key and the hash are fixed - it is the signature itself
       that gets damaged */
    static uint8_t pub[97];
    static uint8_t hash[48];
    for (int i = 0; i < 97; i++)
        pub[i] = (uint8_t)(i * 7 + 1);
    pub[0] = 0x04;
    for (int i = 0; i < 48; i++)
        hash[i] = (uint8_t)i;

    ecdsa_verify_der(&ec_p256, pub, 65, hash, 32, data, (uint32_t)len);
    ecdsa_verify_der(&ec_p384, pub, 97, hash, 48, data, (uint32_t)len);
}

/* The picture decoders. A picture arrives from the network exactly as
   a certificate does, so it gets the same treatment - and it is the
   one place where the input decides how much is written, which is the
   kind of arithmetic a corrupted file is best at breaking. */
static void run_inflate(const unsigned char *data, size_t len)
{
    unsigned char *out = malloc(1 << 20);
    if (!out)
        return;
    inflate_zlib(data, (unsigned)len, out, 1 << 20);
    inflate_raw(data, (unsigned)len, out, 1 << 20);
    free(out);
}

static void run_jpeg(const unsigned char *data, size_t len)
{
    unsigned char *mem = malloc(4 << 20);
    if (!mem)
        return;
    int w = 0, h = 0;
    const char *why = "?";
    if (jpeg_decode(data, (unsigned)len, mem, 4 << 20, &w, &h, &why) == 0) {
        /* Every pixel, because the planes are read through a
           calculation that a strange set of sampling factors could
           push outside them */
        volatile int sink = 0;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int r, g, b;
                jpeg_pixel(x, y, &r, &g, &b);
                sink += r + g + b;
            }
        (void)sink;
    }
    free(mem);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr,
                "usage: %s <x509|html|http|ecdsa|inflate|jpeg> <file>\n",
                argv[0]);
        return 2;
    }

    FILE *f = strcmp(argv[2], "-") ? fopen(argv[2], "rb") : stdin;
    if (!f) {
        perror(argv[2]);
        return 2;
    }
    size_t len = fread(scratch, 1, sizeof(scratch), f);
    if (f != stdin)
        fclose(f);

    unsigned char *input = malloc(len ? len : 1);
    if (!input)
        return 2;
    memcpy(input, scratch, len);

    if (!strcmp(argv[1], "x509"))
        run_x509(input, len);
    else if (!strcmp(argv[1], "html"))
        run_html(input, len);
    else if (!strcmp(argv[1], "http"))
        run_http(input, len);
    else if (!strcmp(argv[1], "ecdsa"))
        run_ecdsa(input, len);
    else if (!strcmp(argv[1], "inflate"))
        run_inflate(input, len);
    else if (!strcmp(argv[1], "jpeg"))
        run_jpeg(input, len);
    else {
        fprintf(stderr, "nieznany parser: %s\n", argv[1]);
        free(input);
        return 2;
    }

    free(input);
    return 0;
}
