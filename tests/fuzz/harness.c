/* Sprawdzanie parserow ProtonOS-a na uszkodzonych danych
 *
 * Parsery certyfikatow i HTML dostaja dane prosto z sieci, wiec musza
 * znosic dowolne smieci. Ten program uruchamia je na hoscie - z
 * sanitizerami adresow i zachowan nieokreslonych - na wejsciu podanym z
 * pliku albo na stdin. Skrypt scripts/fuzz_parsers.py karmi go tysiacami
 * przypadkow zrobionych przez psucie poprawnych danych.
 *
 * Uzycie: harness <x509|html|http|ecdsa> <plik>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ux509.h"
#include "uecdsa.h"
#include "uhtml.h"

#define MAX_INPUT (1 << 20)

/* Wejscie ladujemy do bufora dokladnie tej dlugosci, co dane. Gdyby
   siedzialo w wielkiej tablicy statycznej, odczyt kilka bajtow za
   koncem danych miescilby sie w niej i sanitizer nic by nie zauwazyl -
   a to wlasnie takie bledy chcemy tu lapac. */
static unsigned char scratch[MAX_INPUT];

/* --- HTTP: wydzielone z uhttp.h, zeby nie ciagnac tu calego TLS --- */

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

    /* Dotknij wszystkich pol, ktore czyta reszta systemu */
    volatile int sink = c.sig_alg + c.key_type + c.is_ca + c.ec_bits;
    (void)sink;
    x509_matches_host(&c, "protonos.test");
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
    char *outbuf = malloc(cap);     /* tez co do bajtu, nie z zapasem */

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
            fprintf(stderr, "wiersz poza tekstem: %d+%d > %d\n",
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
    /* Klucz publiczny i skrot biore stale - psuty jest sam podpis */
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

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "uzycie: %s <x509|html|http|ecdsa> <plik>\n", argv[0]);
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
    else {
        fprintf(stderr, "nieznany parser: %s\n", argv[1]);
        free(input);
        return 2;
    }

    free(input);
    return 0;
}
