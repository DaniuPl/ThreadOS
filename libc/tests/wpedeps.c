/* wpedeps - the libraries WPE's port of WebKit needs, on ThreadOS
 *
 * libgcrypt (with libgpg-error under it), libtasn1, libwebp, libxkbcommon,
 * HarfBuzz with its ICU part, and libepoxy: each built from its release
 * into BUILD/libs64 (the Makefile's 'What WPE's port of WebKit needs').
 * scripts/tests/t_wpedeps.py writes the questions into a file, one a
 * line, and works every answer out with something else - Python's
 * hashlib, the cryptography package (OpenSSL), Pillow's libwebp, the
 * keymap's and the font's own definitions, Python's unicodedata - so
 * what is checked is what these libraries do here, built as they are
 * here and running on this system's threads, memory and files.
 *
 * A question is "word arguments...", bytes in hex; its answer is
 * "@N answer", N the line's number. What a line asks for:
 *
 *   version                      libgcrypt's and libgpg-error's versions
 *   digest ALG HEX               a digest of the bytes
 *   digestn ALG N SEED           of N bytes made by gen() below
 *   digestt ALG N SEED THREADS   the same in THREADS threads at once
 *   hmac ALG KEY MSG
 *   aes-cbc KEY IV PT            (and decrypted back), aes-ctr KEY CTR PT
 *   aes-gcm KEY IV AAD PT        ciphertext and tag; a wrong tag refused
 *   aes-kw KEK KEY
 *   pbkdf2 ALG ITER PASS SALT LEN, hkdf ALG IKM SALT INFO LEN
 *   ecdsa-verify CURVE Q ALG MSG R S, ecdsa-sign CURVE D ALG MSG
 *   ecdsa-keygen CURVE ALG MSG   a key made here, and a signature by it
 *   ecdh CURVE D PEERQ, x25519 SCALAR POINT
 *   ed25519-sign SEED MSG, ed25519-verify PUB MSG SIG
 *   rsa-verify pkcs1|pss N E ALG MSG SIG
 *   spki DER                     what libtasn1 reads out of a public key
 *   spki-ec CURVEOID POINT       a public key libtasn1 writes
 *   webp FILE, webp-anim FILE
 *   xkb-load FILE                a keymap, the one the next lines use
 *   xkb KEY...                   keys pressed (+KEY), let go (-KEY) or
 *                                tapped (KEY), and what each tap gives
 *   xkb-sym NAME...              keysyms by name and their UTF-8
 *   hb FILE ucd|icu FEATURES CP,CP...   text shaped with a font
 *   hb-props CP...               Unicode properties by ICU and HarfBuzz
 *   epoxy-ext LIST NAME, epoxy-egl
 */

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gcrypt.h>
#include <libtasn1.h>
#include <webp/decode.h>
#include <webp/demux.h>
#include <xkbcommon/xkbcommon.h>
#include <hb.h>
#include <hb-ot.h>
#include <hb-icu.h>
#include <epoxy/egl.h>

#define MAX_ARGS 40

/* ---- bytes ------------------------------------------------------------ */

static unsigned char *unhex(const char *s, size_t *n)
{
    size_t len = strlen(s);
    if (len == 1 && s[0] == '-')
        len = 0;                    /* "-" is no bytes at all */
    unsigned char *out = malloc(len / 2 + 1);
    for (size_t i = 0; i + 1 < len; i += 2) {
        unsigned v;
        sscanf(s + i, "%2x", &v);
        out[i / 2] = (unsigned char)v;
    }
    *n = len / 2;
    return out;
}

static void put_hex(const unsigned char *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        printf("%02x", p[i]);
}

/* Bytes both sides can make without sending them: t_wpedeps.py's
   gen() is the same arithmetic */
static unsigned char *gen(size_t n, unsigned seed)
{
    unsigned char *p = malloc(n ? n : 1);
    uint32_t x = seed * 2654435761u + 1;
    for (size_t i = 0; i < n; i++) {
        x = x * 1103515245u + 12345u;
        p[i] = (unsigned char)(x >> 16);
    }
    return p;
}

static unsigned char *slurp(const char *name, size_t *n)
{
    FILE *f = fopen(name, "rb");
    if (!f)
        return NULL;
    size_t cap = 1 << 16, len = 0;
    unsigned char *p = malloc(cap);
    size_t got;
    while ((got = fread(p + len, 1, cap - len, f)) > 0) {
        len += got;
        if (len == cap)
            p = realloc(p, cap *= 2);
    }
    fclose(f);
    *n = len;
    return p;
}

/* ---- libgcrypt ---------------------------------------------------------- */

static int md_algo(const char *name)
{
    static const struct { const char *name; int algo; } t[] = {
        {"sha1", GCRY_MD_SHA1}, {"sha224", GCRY_MD_SHA224},
        {"sha256", GCRY_MD_SHA256}, {"sha384", GCRY_MD_SHA384},
        {"sha512", GCRY_MD_SHA512}, {"sha3-256", GCRY_MD_SHA3_256},
        {"md5", GCRY_MD_MD5},
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++)
        if (!strcmp(name, t[i].name))
            return t[i].algo;
    return 0;
}

static int mac_algo(const char *name)
{
    if (!strcmp(name, "sha1")) return GCRY_MAC_HMAC_SHA1;
    if (!strcmp(name, "sha256")) return GCRY_MAC_HMAC_SHA256;
    if (!strcmp(name, "sha384")) return GCRY_MAC_HMAC_SHA384;
    if (!strcmp(name, "sha512")) return GCRY_MAC_HMAC_SHA512;
    return 0;
}

static void put_digest(int algo, const void *p, size_t n)
{
    unsigned char out[64];
    gcry_md_hash_buffer(algo, out, p, n);
    put_hex(out, gcry_md_get_algo_dlen(algo));
}

struct hash_job {
    int algo;
    const unsigned char *p;
    size_t n;
    unsigned char out[64];
};

/* In pieces of uneven size, so that each thread's context goes through
   its block boundaries at its own places */
static void *hash_thread(void *arg)
{
    struct hash_job *j = arg;
    gcry_md_hd_t h;
    if (gcry_md_open(&h, j->algo, 0))
        return NULL;
    for (size_t at = 0, step = 1; at < j->n; at += step, step = step * 3 % 1021 + 1)
        gcry_md_write(h, j->p + at, at + step > j->n ? j->n - at : step);
    memcpy(j->out, gcry_md_read(h, j->algo), gcry_md_get_algo_dlen(j->algo));
    gcry_md_close(h);
    return NULL;
}

static int aes_algo(size_t keylen)
{
    return keylen == 16 ? GCRY_CIPHER_AES128 :
           keylen == 24 ? GCRY_CIPHER_AES192 : GCRY_CIPHER_AES256;
}

static gcry_cipher_hd_t aes_open(int mode, const unsigned char *key, size_t klen)
{
    gcry_cipher_hd_t h;
    if (gcry_cipher_open(&h, aes_algo(klen), mode, 0))
        return NULL;
    if (gcry_cipher_setkey(h, key, klen)) {
        gcry_cipher_close(h);
        return NULL;
    }
    return h;
}

/* A number as an MPI from unsigned big-endian bytes: WebKit's keys are
   given to libgcrypt so (%m), never as signed buffers */
static gcry_mpi_t mpi(const unsigned char *p, size_t n)
{
    gcry_mpi_t m = NULL;
    gcry_mpi_scan(&m, GCRYMPI_FMT_USG, p, n, NULL);
    return m;
}

/* A token's bytes in an S-expression, left-padded to 'width' */
static size_t sexp_bytes(gcry_sexp_t s, const char *token, unsigned char *out,
                         size_t width)
{
    gcry_sexp_t t = gcry_sexp_find_token(s, token, 0);
    if (!t)
        return 0;
    size_t n;
    const char *d = gcry_sexp_nth_data(t, 1, &n);
    size_t total = n < width ? width : n;
    memset(out, 0, total);
    memcpy(out + total - n, d, n);
    gcry_sexp_release(t);
    return total;
}

static size_t curve_bytes(const char *curve)
{
    return !strcmp(curve, "NIST P-384") ? 48 : !strcmp(curve, "NIST P-521") ? 66 : 32;
}

static const char *curve_name(const char *arg)
{
    return !strcmp(arg, "P-256") ? "NIST P-256" :
           !strcmp(arg, "P-384") ? "NIST P-384" :
           !strcmp(arg, "P-521") ? "NIST P-521" : arg;
}

/* The digest of a message as the data ECDSA signs: the hash, raw, as
   WebKit's CryptoAlgorithmECDSAGCrypt gives it */
static gcry_sexp_t ecdsa_data(const char *alg, const unsigned char *msg, size_t n)
{
    int algo = md_algo(alg);
    unsigned char h[64];
    gcry_md_hash_buffer(algo, h, msg, n);
    gcry_sexp_t data = NULL;
    gcry_sexp_build(&data, NULL, "(data(flags raw)(value %b))",
                    (int)gcry_md_get_algo_dlen(algo), h);
    return data;
}

static int do_crypto(int argc, char **argv)
{
    const char *op = argv[0];
    size_t n1, n2, n3, n4, n5;

    if (!strcmp(op, "version") && argc == 1) {
        printf("libgcrypt %s libgpg-error %s", gcry_check_version(NULL),
               gpg_error_check_version(NULL));
        return 1;
    }
    if (!strcmp(op, "digest") && argc == 3) {
        unsigned char *m = unhex(argv[2], &n1);
        put_digest(md_algo(argv[1]), m, n1);
        free(m);
        return 1;
    }
    if (!strcmp(op, "digestn") && argc == 4) {
        size_t n = strtoul(argv[2], NULL, 10);
        unsigned char *m = gen(n, strtoul(argv[3], NULL, 10));
        put_digest(md_algo(argv[1]), m, n);
        free(m);
        return 1;
    }
    if (!strcmp(op, "digestt") && argc == 5) {
        size_t n = strtoul(argv[2], NULL, 10);
        int threads = atoi(argv[4]);
        unsigned char *m = gen(n, strtoul(argv[3], NULL, 10));
        struct hash_job jobs[16];
        pthread_t tid[16];
        if (threads > 16)
            threads = 16;
        for (int i = 0; i < threads; i++) {
            jobs[i] = (struct hash_job){ md_algo(argv[1]), m, n, {0} };
            pthread_create(&tid[i], NULL, hash_thread, &jobs[i]);
        }
        int same = 1;
        size_t dl = gcry_md_get_algo_dlen(md_algo(argv[1]));
        for (int i = 0; i < threads; i++) {
            pthread_join(tid[i], NULL);
            same &= !memcmp(jobs[i].out, jobs[0].out, dl);
        }
        put_hex(jobs[0].out, dl);
        printf(" all %d threads alike: %s", threads, same ? "yes" : "no");
        free(m);
        return 1;
    }
    if (!strcmp(op, "hmac") && argc == 4) {
        unsigned char *k = unhex(argv[2], &n1), *m = unhex(argv[3], &n2);
        unsigned char out[64];
        size_t len = sizeof out;
        gcry_mac_hd_t h;
        if (gcry_mac_open(&h, mac_algo(argv[1]), 0, NULL) ||
            gcry_mac_setkey(h, k, n1) || gcry_mac_write(h, m, n2) ||
            gcry_mac_read(h, out, &len))
            printf("failed");
        else
            put_hex(out, len);
        gcry_mac_close(h);
        free(k);
        free(m);
        return 1;
    }
    if ((!strcmp(op, "aes-cbc") || !strcmp(op, "aes-ctr")) && argc == 4) {
        int cbc = op[4] == 'c' && op[5] == 'b';
        unsigned char *k = unhex(argv[1], &n1), *iv = unhex(argv[2], &n2),
                      *pt = unhex(argv[3], &n3);
        unsigned char *ct = malloc(n3 + 1), *back = malloc(n3 + 1);
        gcry_cipher_hd_t h = aes_open(cbc ? GCRY_CIPHER_MODE_CBC : GCRY_CIPHER_MODE_CTR,
                                      k, n1);
        int bad = !h || (cbc ? gcry_cipher_setiv(h, iv, n2) : gcry_cipher_setctr(h, iv, n2)) ||
                  gcry_cipher_encrypt(h, ct, n3, pt, n3);
        bad = bad || (cbc ? gcry_cipher_setiv(h, iv, n2) : gcry_cipher_setctr(h, iv, n2)) ||
              gcry_cipher_decrypt(h, back, n3, ct, n3);
        if (bad)
            printf("failed");
        else {
            put_hex(ct, n3);
            printf(" back: %s", memcmp(back, pt, n3) ? "no" : "yes");
        }
        gcry_cipher_close(h);
        free(k); free(iv); free(pt); free(ct); free(back);
        return 1;
    }
    if (!strcmp(op, "aes-gcm") && argc == 5) {
        unsigned char *k = unhex(argv[1], &n1), *iv = unhex(argv[2], &n2),
                      *aad = unhex(argv[3], &n3), *pt = unhex(argv[4], &n4);
        unsigned char *ct = malloc(n4 + 1), *back = malloc(n4 + 1), tag[16];
        gcry_cipher_hd_t h = aes_open(GCRY_CIPHER_MODE_GCM, k, n1);
        if (!h || gcry_cipher_setiv(h, iv, n2) ||
            gcry_cipher_authenticate(h, aad, n3) ||
            gcry_cipher_encrypt(h, ct, n4, pt, n4) ||
            gcry_cipher_gettag(h, tag, 16)) {
            printf("failed");
        } else {
            put_hex(ct, n4);
            printf(" ");
            put_hex(tag, 16);
            /* The same ciphertext with one bit of its tag wrong must not
               be taken: Web Crypto's decrypt rejects it */
            tag[5] ^= 0x10;
            gcry_cipher_reset(h);
            gcry_cipher_setiv(h, iv, n2);
            gcry_cipher_authenticate(h, aad, n3);
            gcry_cipher_decrypt(h, back, n4, ct, n4);
            gcry_error_t e = gcry_cipher_checktag(h, tag, 16);
            printf(" a wrong tag: %s",
                   gcry_err_code(e) == GPG_ERR_CHECKSUM ? "refused" : "taken");
        }
        gcry_cipher_close(h);
        free(k); free(iv); free(aad); free(pt); free(ct); free(back);
        return 1;
    }
    if (!strcmp(op, "aes-kw") && argc == 3) {
        unsigned char *kek = unhex(argv[1], &n1), *key = unhex(argv[2], &n2);
        unsigned char *out = malloc(n2 + 8);
        gcry_cipher_hd_t h = aes_open(GCRY_CIPHER_MODE_AESWRAP, kek, n1);
        if (!h || gcry_cipher_encrypt(h, out, n2 + 8, key, n2))
            printf("failed");
        else
            put_hex(out, n2 + 8);
        gcry_cipher_close(h);
        free(kek); free(key); free(out);
        return 1;
    }
    if (!strcmp(op, "pbkdf2") && argc == 6) {
        unsigned char *pw = unhex(argv[3], &n1), *salt = unhex(argv[4], &n2);
        size_t len = strtoul(argv[5], NULL, 10);
        unsigned char *out = malloc(len);
        if (gcry_kdf_derive(pw, n1, GCRY_KDF_PBKDF2, md_algo(argv[1]), salt, n2,
                            strtoul(argv[2], NULL, 10), len, out))
            printf("failed");
        else
            put_hex(out, len);
        free(pw); free(salt); free(out);
        return 1;
    }
    if (!strcmp(op, "hkdf") && argc == 6) {
        unsigned char *ikm = unhex(argv[2], &n1), *salt = unhex(argv[3], &n2),
                      *info = unhex(argv[4], &n3);
        unsigned long len = strtoul(argv[5], NULL, 10);
        unsigned char *out = malloc(len);
        gcry_kdf_hd_t h;
        /* libgcrypt's HKDF takes the salt where the other KDFs take a
           key, and the info as their 'additional data' (cipher/kdf.c) */
        if (gcry_kdf_open(&h, GCRY_KDF_HKDF, mac_algo(argv[1]), &len, 1,
                          ikm, n1, NULL, 0, salt, n2, info, n3) ||
            gcry_kdf_compute(h, NULL) || gcry_kdf_final(h, len, out))
            printf("failed");
        else {
            put_hex(out, len);
            gcry_kdf_close(h);
        }
        free(ikm); free(salt); free(info); free(out);
        return 1;
    }
    if (!strcmp(op, "ecdsa-verify") && argc == 7) {
        const char *curve = curve_name(argv[1]);
        unsigned char *q = unhex(argv[2], &n1), *m = unhex(argv[4], &n2),
                      *r = unhex(argv[5], &n3), *s = unhex(argv[6], &n4);
        gcry_sexp_t key = NULL, sig = NULL, data = ecdsa_data(argv[3], m, n2);
        gcry_mpi_t mr = mpi(r, n3), ms = mpi(s, n4);
        gcry_sexp_build(&key, NULL, "(public-key(ecc(curve %s)(q %b)))", curve, (int)n1, q);
        gcry_sexp_build(&sig, NULL, "(sig-val(ecdsa(r %m)(s %m)))", mr, ms);
        printf("%s", gcry_pk_verify(sig, data, key) ? "bad" : "good");
        gcry_sexp_release(key); gcry_sexp_release(sig); gcry_sexp_release(data);
        gcry_mpi_release(mr); gcry_mpi_release(ms);
        free(q); free(m); free(r); free(s);
        return 1;
    }
    if ((!strcmp(op, "ecdsa-sign") && argc == 5) ||
        (!strcmp(op, "ecdsa-keygen") && argc == 4)) {
        int keygen = op[6] == 'k';
        const char *curve = curve_name(argv[1]);
        size_t w = curve_bytes(curve);
        gcry_sexp_t key = NULL, sig = NULL, pub = NULL;
        unsigned char *m;
        if (keygen) {
            gcry_sexp_t parms = NULL, pair = NULL;
            gcry_sexp_build(&parms, NULL, "(genkey(ecc(curve %s)))", curve);
            if (gcry_pk_genkey(&pair, parms)) {
                printf("no key made");
                return 1;
            }
            key = gcry_sexp_find_token(pair, "private-key", 0);
            pub = gcry_sexp_find_token(pair, "public-key", 0);
            gcry_sexp_release(parms);
            gcry_sexp_release(pair);
            m = unhex(argv[3], &n2);
        } else {
            unsigned char *d = unhex(argv[2], &n1);
            gcry_mpi_t md = mpi(d, n1);
            gcry_sexp_build(&key, NULL, "(private-key(ecc(curve %s)(d %m)))", curve, md);
            gcry_mpi_release(md);
            free(d);
            m = unhex(argv[4], &n2);
        }
        gcry_sexp_t data = ecdsa_data(argv[keygen ? 2 : 3], m, n2);
        if (gcry_pk_sign(&sig, data, key)) {
            printf("failed");
        } else {
            unsigned char buf[200];
            if (keygen) {
                size_t qn = sexp_bytes(pub, "q", buf, 0);
                put_hex(buf, qn);
                printf(" ");
            }
            sexp_bytes(sig, "r", buf, w);
            put_hex(buf, w);
            printf(" ");
            sexp_bytes(sig, "s", buf, w);
            put_hex(buf, w);
        }
        gcry_sexp_release(key); gcry_sexp_release(sig); gcry_sexp_release(pub);
        gcry_sexp_release(data);
        free(m);
        return 1;
    }
    if (!strcmp(op, "ecdh") && argc == 4) {
        /* As WebKit derives ECDH bits: the peer's point times our
           scalar, through the encryption primitive; the secret is x */
        const char *curve = curve_name(argv[1]);
        size_t w = curve_bytes(curve);
        unsigned char *d = unhex(argv[2], &n1), *q = unhex(argv[3], &n2);
        gcry_mpi_t md = mpi(d, n1);
        gcry_sexp_t key = NULL, data = NULL, enc = NULL;
        gcry_sexp_build(&key, NULL, "(public-key(ecc(curve %s)(q %b)))", curve, (int)n2, q);
        gcry_sexp_build(&data, NULL, "(data(flags raw)(value %m))", md);
        unsigned char pt[200];
        if (gcry_pk_encrypt(&enc, data, key) || sexp_bytes(enc, "s", pt, 0) != 1 + 2 * w)
            printf("failed");
        else
            put_hex(pt + 1, w);
        gcry_sexp_release(key); gcry_sexp_release(data); gcry_sexp_release(enc);
        gcry_mpi_release(md);
        free(d); free(q);
        return 1;
    }
    if (!strcmp(op, "x25519") && argc == 3) {
        unsigned char *k = unhex(argv[1], &n1), *u = unhex(argv[2], &n2), out[32];
        if (n1 != 32 || n2 != 32 || gcry_ecc_mul_point(GCRY_ECC_CURVE25519, out, k, u))
            printf("failed");
        else
            put_hex(out, 32);
        free(k); free(u);
        return 1;
    }
    if (!strcmp(op, "ed25519-sign") && argc == 3) {
        unsigned char *seed = unhex(argv[1], &n1), *m = unhex(argv[2], &n2);
        gcry_sexp_t key = NULL, data = NULL, sig = NULL;
        gcry_sexp_build(&key, NULL,
                        "(private-key(ecc(curve Ed25519)(flags eddsa)(d %b)))",
                        (int)n1, seed);
        gcry_sexp_build(&data, NULL, "(data(flags eddsa)(hash-algo sha512)(value %b))",
                        (int)n2, m);
        unsigned char r[32], s[32];
        if (gcry_pk_sign(&sig, data, key) || sexp_bytes(sig, "r", r, 32) != 32 ||
            sexp_bytes(sig, "s", s, 32) != 32)
            printf("failed");
        else {
            put_hex(r, 32);
            put_hex(s, 32);
        }
        gcry_sexp_release(key); gcry_sexp_release(data); gcry_sexp_release(sig);
        free(seed); free(m);
        return 1;
    }
    if (!strcmp(op, "ed25519-verify") && argc == 4) {
        unsigned char *pub = unhex(argv[1], &n1), *m = unhex(argv[2], &n2),
                      *sig = unhex(argv[3], &n3);
        gcry_sexp_t key = NULL, data = NULL, s = NULL;
        gcry_sexp_build(&key, NULL,
                        "(public-key(ecc(curve Ed25519)(flags eddsa)(q %b)))",
                        (int)n1, pub);
        gcry_sexp_build(&data, NULL, "(data(flags eddsa)(hash-algo sha512)(value %b))",
                        (int)n2, m);
        gcry_sexp_build(&s, NULL, "(sig-val(eddsa(r %b)(s %b)))", 32, sig, 32, sig + 32);
        printf("%s", n3 == 64 && !gcry_pk_verify(s, data, key) ? "good" : "bad");
        gcry_sexp_release(key); gcry_sexp_release(data); gcry_sexp_release(s);
        free(pub); free(m); free(sig);
        return 1;
    }
    if (!strcmp(op, "rsa-verify") && argc == 7) {
        int pss = !strcmp(argv[1], "pss");
        int algo = md_algo(argv[4]);
        unsigned char *nb = unhex(argv[2], &n1), *eb = unhex(argv[3], &n2),
                      *m = unhex(argv[5], &n3), *sb = unhex(argv[6], &n4);
        unsigned char h[64];
        size_t hl = gcry_md_get_algo_dlen(algo);
        gcry_md_hash_buffer(algo, h, m, n3);
        gcry_mpi_t mn = mpi(nb, n1), me = mpi(eb, n2), msig = mpi(sb, n4);
        gcry_sexp_t key = NULL, data = NULL, sig = NULL;
        gcry_sexp_build(&key, NULL, "(public-key(rsa(n %m)(e %m)))", mn, me);
        if (pss)
            gcry_sexp_build(&data, NULL, "(data(flags pss)(salt-length %u)(hash %s %b))",
                            (unsigned)hl, argv[4], (int)hl, h);
        else
            gcry_sexp_build(&data, NULL, "(data(flags pkcs1)(hash %s %b))",
                            argv[4], (int)hl, h);
        gcry_sexp_build(&sig, NULL, "(sig-val(rsa(s %m)))", msig);
        printf("%s", gcry_pk_verify(sig, data, key) ? "bad" : "good");
        gcry_sexp_release(key); gcry_sexp_release(data); gcry_sexp_release(sig);
        gcry_mpi_release(mn); gcry_mpi_release(me); gcry_mpi_release(msig);
        free(nb); free(eb); free(m); free(sb);
        return 1;
    }
    (void)n5;
    return 0;
}

/* ---- libtasn1 ------------------------------------------------------------ */

/* The few definitions of RFC 5280 and RFC 5480 a public key needs */
static const char ASN_DEFS[] =
    "WPEDeps { 1 3 6 1 4 1 0 }\n"
    "DEFINITIONS IMPLICIT TAGS ::=\n"
    "BEGIN\n"
    "AlgorithmIdentifier ::= SEQUENCE {\n"
    "  algorithm OBJECT IDENTIFIER,\n"
    "  parameters ANY DEFINED BY algorithm OPTIONAL }\n"
    "SubjectPublicKeyInfo ::= SEQUENCE {\n"
    "  algorithm AlgorithmIdentifier,\n"
    "  subjectPublicKey BIT STRING }\n"
    "RSAPublicKey ::= SEQUENCE {\n"
    "  modulus INTEGER,\n"
    "  publicExponent INTEGER }\n"
    "ECParameters ::= CHOICE {\n"
    "  namedCurve OBJECT IDENTIFIER }\n"
    "END\n";

static asn1_node asn_defs;

static int asn_ready(void)
{
    if (asn_defs)
        return 1;
    FILE *f = fopen("wpedeps.asn", "w");
    if (!f)
        return 0;
    fputs(ASN_DEFS, f);
    fclose(f);
    char why[ASN1_MAX_ERROR_DESCRIPTION_SIZE];
    int r = asn1_parser2tree("wpedeps.asn", &asn_defs, why);
    if (r != ASN1_SUCCESS)
        printf("(definitions refused: %s) ", why);
    return r == ASN1_SUCCESS;
}

static int read_value(asn1_node n, const char *name, unsigned char *buf, int cap)
{
    int len = cap;
    return asn1_read_value(n, name, buf, &len) == ASN1_SUCCESS ? len : -1;
}

static int do_asn(int argc, char **argv)
{
    const char *op = argv[0];
    size_t n1, n2;
    if (!strcmp(op, "spki") && argc == 2) {
        if (!asn_ready())
            return 1;
        unsigned char *der = unhex(argv[1], &n1);
        asn1_node spki = NULL;
        char why[ASN1_MAX_ERROR_DESCRIPTION_SIZE];
        unsigned char buf[2048];
        asn1_create_element(asn_defs, "WPEDeps.SubjectPublicKeyInfo", &spki);
        int r = asn1_der_decoding(&spki, der, (int)n1, why);
        if (r != ASN1_SUCCESS) {
            printf("refused (%s)", asn1_strerror(r));
            asn1_delete_structure(&spki);
            free(der);
            return 1;
        }
        int len = read_value(spki, "algorithm.algorithm", buf, sizeof buf);
        printf("algorithm %.*s", len > 0 ? len - 1 : 0, buf);
        int bits = sizeof buf * 8;
        unsigned char key[1024];
        int keybytes = -1;
        if (asn1_read_value(spki, "subjectPublicKey", key, &bits) == ASN1_SUCCESS)
            keybytes = bits / 8;
        if (!strcmp((char *)buf, "1.2.840.113549.1.1.1") && keybytes > 0) {
            asn1_node rsa = NULL;
            asn1_create_element(asn_defs, "WPEDeps.RSAPublicKey", &rsa);
            if (asn1_der_decoding(&rsa, key, keybytes, why) == ASN1_SUCCESS) {
                len = read_value(rsa, "modulus", buf, sizeof buf);
                printf(" modulus ");
                put_hex(buf, len);
                len = read_value(rsa, "publicExponent", buf, sizeof buf);
                printf(" exponent ");
                put_hex(buf, len);
            }
            asn1_delete_structure(&rsa);
        } else if (keybytes > 0) {
            len = read_value(spki, "algorithm.parameters", buf, sizeof buf);
            asn1_node par = NULL;
            asn1_create_element(asn_defs, "WPEDeps.ECParameters", &par);
            if (len > 0 && asn1_der_decoding(&par, buf, len, why) == ASN1_SUCCESS) {
                len = read_value(par, "namedCurve", buf, sizeof buf);
                printf(" curve %.*s", len > 0 ? len - 1 : 0, buf);
            }
            asn1_delete_structure(&par);
            printf(" point ");
            put_hex(key, keybytes);
        }
        asn1_delete_structure(&spki);
        free(der);
        return 1;
    }
    if (!strcmp(op, "spki-ec") && argc == 3) {
        if (!asn_ready())
            return 1;
        unsigned char *point = unhex(argv[2], &n2);
        asn1_node spki = NULL, par = NULL;
        unsigned char pder[64], der[512];
        int plen = sizeof pder, dlen = sizeof der;
        char why[ASN1_MAX_ERROR_DESCRIPTION_SIZE];
        asn1_create_element(asn_defs, "WPEDeps.ECParameters", &par);
        asn1_write_value(par, "", "namedCurve", 1);
        asn1_write_value(par, "namedCurve", argv[1], 1);
        asn1_create_element(asn_defs, "WPEDeps.SubjectPublicKeyInfo", &spki);
        asn1_write_value(spki, "algorithm.algorithm", "1.2.840.10045.2.1", 1);
        int r = asn1_der_coding(par, "", pder, &plen, why);
        if (r == ASN1_SUCCESS)
            r = asn1_write_value(spki, "algorithm.parameters", pder, plen);
        if (r == ASN1_SUCCESS)
            r = asn1_write_value(spki, "subjectPublicKey", point, (int)n2 * 8);
        if (r == ASN1_SUCCESS)
            r = asn1_der_coding(spki, "", der, &dlen, why);
        if (r != ASN1_SUCCESS)
            printf("failed (%s: %s)", asn1_strerror(r), why);
        else
            put_hex(der, dlen);
        asn1_delete_structure(&spki);
        asn1_delete_structure(&par);
        free(point);
        (void)n1;
        return 1;
    }
    return 0;
}

/* ---- libwebp ----------------------------------------------------------- */

static int do_webp(int argc, char **argv)
{
    const char *op = argv[0];
    if (argc != 2 || (strcmp(op, "webp") && strcmp(op, "webp-anim")))
        return 0;
    size_t n;
    unsigned char *data = slurp(argv[1], &n);
    if (!data) {
        printf("no file %s", argv[1]);
        return 1;
    }
    if (!strcmp(op, "webp")) {
        WebPBitstreamFeatures f;
        int w, h;
        unsigned char *px;
        if (WebPGetFeatures(data, n, &f) != VP8_STATUS_OK ||
            !(px = WebPDecodeRGBA(data, n, &w, &h))) {
            printf("refused");
        } else {
            printf("%dx%d alpha %s ", w, h, f.has_alpha ? "yes" : "no");
            put_digest(GCRY_MD_SHA256, px, (size_t)w * h * 4);
            WebPFree(px);
        }
    } else {
        WebPData wd = { data, n };
        WebPAnimDecoderOptions opt;
        WebPAnimDecoderOptionsInit(&opt);
        opt.color_mode = MODE_RGBA;
        WebPAnimDecoder *dec = WebPAnimDecoderNew(&wd, &opt);
        WebPAnimInfo info;
        if (!dec || !WebPAnimDecoderGetInfo(dec, &info)) {
            printf("refused");
        } else {
            printf("%ux%u frames %u loop %u", info.canvas_width, info.canvas_height,
                   info.frame_count, info.loop_count);
            uint8_t *buf;
            int ts;
            while (WebPAnimDecoderHasMoreFrames(dec) &&
                   WebPAnimDecoderGetNext(dec, &buf, &ts)) {
                printf(" %d:", ts);
                put_digest(GCRY_MD_SHA256, buf,
                           (size_t)info.canvas_width * info.canvas_height * 4);
            }
        }
        WebPAnimDecoderDelete(dec);
    }
    free(data);
    return 1;
}

/* ---- libxkbcommon -------------------------------------------------------- */

static struct xkb_context *xkb_ctx;
static struct xkb_keymap *keymap;

static void put_sym(xkb_keysym_t sym, const char *utf8)
{
    char name[64];
    if (xkb_keysym_get_name(sym, name, sizeof name) < 0)
        strcpy(name, "?");
    printf("%s/", name);
    put_hex((const unsigned char *)utf8, strlen(utf8));
}

static int do_xkb(int argc, char **argv)
{
    const char *op = argv[0];
    if (!xkb_ctx)
        xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_DEFAULT_INCLUDES |
                                  XKB_CONTEXT_NO_ENVIRONMENT_NAMES);
    if (!strcmp(op, "xkb-load") && argc == 2) {
        size_t n;
        char *text = (char *)slurp(argv[1], &n);
        if (keymap)
            xkb_keymap_unref(keymap);
        keymap = NULL;
        if (text) {
            text = realloc(text, n + 1);
            text[n] = 0;
            keymap = xkb_keymap_new_from_string(xkb_ctx, text, XKB_KEYMAP_FORMAT_TEXT_V1,
                                                XKB_KEYMAP_COMPILE_NO_FLAGS);
            free(text);
        }
        if (!keymap) {
            printf("refused");
            return 1;
        }
        /* Written out and read in again, the keymap must be the same
           one: what WPE's platform layer hands across processes */
        char *again = xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
        struct xkb_keymap *k2 = again ?
            xkb_keymap_new_from_string(xkb_ctx, again, XKB_KEYMAP_FORMAT_TEXT_V1,
                                       XKB_KEYMAP_COMPILE_NO_FLAGS) : NULL;
        char *twice = k2 ? xkb_keymap_get_as_string(k2, XKB_KEYMAP_FORMAT_TEXT_V1) : NULL;
        printf("loaded, %u layout, written out and read again: %s",
               xkb_keymap_num_layouts(keymap),
               twice && !strcmp(again, twice) ? "the same" : "different");
        free(again);
        free(twice);
        xkb_keymap_unref(k2);
        return 1;
    }
    if (!strcmp(op, "xkb") && argc >= 2) {
        if (!keymap) {
            printf("no keymap");
            return 1;
        }
        struct xkb_state *st = xkb_state_new(keymap);
        int taps = 0;
        for (int i = 1; i < argc; i++) {
            const char *a = argv[i];
            int press = a[0] == '+', release = a[0] == '-';
            xkb_keycode_t kc = xkb_keymap_key_by_name(keymap, a + (press || release));
            if (kc == XKB_KEYCODE_INVALID) {
                printf("%s? ", a);
                continue;
            }
            if (!release)
                xkb_state_update_key(st, kc, XKB_KEY_DOWN);
            if (!press && !release) {
                char utf8[16];
                xkb_state_key_get_utf8(st, kc, utf8, sizeof utf8);
                if (taps++)
                    printf(" ");
                put_sym(xkb_state_key_get_one_sym(st, kc), utf8);
            }
            if (!press)
                xkb_state_update_key(st, kc, XKB_KEY_UP);
        }
        xkb_state_unref(st);
        return 1;
    }
    if (!strcmp(op, "xkb-sym") && argc >= 2) {
        for (int i = 1; i < argc; i++) {
            xkb_keysym_t sym = xkb_keysym_from_name(argv[i], XKB_KEYSYM_NO_FLAGS);
            char utf8[16];
            int r = xkb_keysym_to_utf8(sym, utf8, sizeof utf8);
            printf("%s%x/", i > 1 ? " " : "", (unsigned)sym);
            put_hex((const unsigned char *)utf8, r > 0 ? strlen(utf8) : 0);
        }
        return 1;
    }
    return 0;
}

/* ---- HarfBuzz ----------------------------------------------------------- */

static int do_hb(int argc, char **argv)
{
    const char *op = argv[0];
    if (!strcmp(op, "hb") && argc == 5) {
        hb_blob_t *blob = hb_blob_create_from_file_or_fail(argv[1]);
        if (!blob) {
            printf("no file %s", argv[1]);
            return 1;
        }
        hb_face_t *face = hb_face_create(blob, 0);
        hb_font_t *font = hb_font_create(face);
        hb_buffer_t *buf = hb_buffer_create();
        if (!strcmp(argv[2], "icu"))
            hb_buffer_set_unicode_funcs(buf, hb_icu_get_unicode_funcs());
        for (char *p = argv[4]; *p;) {
            hb_codepoint_t cp = strtoul(p, &p, 16);
            hb_buffer_add(buf, cp, hb_buffer_get_length(buf));
            if (*p == ',')
                p++;
        }
        hb_buffer_set_content_type(buf, HB_BUFFER_CONTENT_TYPE_UNICODE);
        hb_buffer_guess_segment_properties(buf);
        hb_feature_t feats[8];
        unsigned nf = 0;
        if (strcmp(argv[3], "default"))
            for (char *f = strtok(argv[3], ","); f && nf < 8; f = strtok(NULL, ","))
                nf += hb_feature_from_string(f, -1, &feats[nf]);
        hb_shape(font, buf, feats, nf);
        char script[5] = {0};
        hb_tag_to_string(hb_script_to_iso15924_tag(hb_buffer_get_script(buf)), script);
        printf("%s %s", hb_direction_to_string(hb_buffer_get_direction(buf)), script);
        unsigned len;
        hb_glyph_info_t *gi = hb_buffer_get_glyph_infos(buf, &len);
        hb_glyph_position_t *gp = hb_buffer_get_glyph_positions(buf, &len);
        for (unsigned i = 0; i < len; i++)
            printf(" %u@%u+%d,%d<%d,%d>", gi[i].codepoint, gi[i].cluster,
                   gp[i].x_advance, gp[i].y_advance, gp[i].x_offset, gp[i].y_offset);
        hb_buffer_destroy(buf);
        hb_font_destroy(font);
        hb_face_destroy(face);
        hb_blob_destroy(blob);
        return 1;
    }
    if (!strcmp(op, "hb-props") && argc >= 2) {
        /* Unicode's two letters for each of HarfBuzz's categories, in
           the order of hb_unicode_general_category_t */
        static const char cats[][3] = {
            "Cc", "Cf", "Cn", "Co", "Cs", "Ll", "Lm", "Lo", "Lt", "Lu",
            "Mc", "Me", "Mn", "Nd", "Nl", "No", "Pc", "Pd", "Pe", "Pf",
            "Pi", "Po", "Ps", "Sc", "Sk", "Sm", "So", "Zl", "Zp", "Zs",
        };
        hb_unicode_funcs_t *icu = hb_icu_get_unicode_funcs();
        hb_unicode_funcs_t *ucd = hb_unicode_funcs_get_default();
        for (int i = 1; i < argc; i++) {
            hb_codepoint_t cp = strtoul(argv[i], NULL, 16);
            unsigned ci = hb_unicode_general_category(icu, cp);
            unsigned cu = hb_unicode_general_category(ucd, cp);
            printf("%s%s/%u %s/%u", i > 1 ? " " : "",
                   ci < 30 ? cats[ci] : "??", hb_unicode_combining_class(icu, cp),
                   cu < 30 ? cats[cu] : "??", hb_unicode_combining_class(ucd, cp));
        }
        return 1;
    }
    return 0;
}

/* ---- libepoxy ---------------------------------------------------------- */

static int do_epoxy(int argc, char **argv)
{
    if (!strcmp(argv[0], "epoxy-ext") && argc == 3) {
        for (char *p = argv[1]; *p; p++)
            if (*p == ',')
                *p = ' ';
        printf("%s", epoxy_extension_in_string(argv[1], argv[2]) ? "yes" : "no");
        return 1;
    }
    if (!strcmp(argv[0], "epoxy-egl") && argc == 1) {
        /* There is no libEGL to open on ThreadOS: epoxy has to say so,
           not end the program, as it does when a GL call finds nothing */
        printf("EGL here: %s", epoxy_has_egl() ? "yes" : "no");
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: wpedeps QUESTIONS-FILE\n");
        return 2;
    }
    /* As WebKit starts libgcrypt (PAL::GCrypt): no secure memory, which
       wants mlock, and nothing changed after this */
    gcry_check_version(NULL);
    gcry_control(GCRYCTL_DISABLE_SECMEM, 0);
    gcry_control(GCRYCTL_INITIALIZATION_FINISHED, 0);

    FILE *f = fopen(argv[1], "r");
    if (!f) {
        printf("wpedeps: no file %s\n", argv[1]);
        return 1;
    }
    static char line[1 << 16];
    int no = 0, unknown = 0;
    while (fgets(line, sizeof line, f)) {
        char *args[MAX_ARGS];
        int n = 0;
        for (char *t = strtok(line, " \t\r\n"); t && n < MAX_ARGS; t = strtok(NULL, " \t\r\n"))
            args[n++] = t;
        if (!n)
            continue;
        no++;
        printf("@%d ", no);
        if (!(do_crypto(n, args) || do_asn(n, args) || do_webp(n, args) ||
              do_xkb(n, args) || do_hb(n, args) || do_epoxy(n, args))) {
            printf("what is %s?", args[0]);
            unknown++;
        }
        printf("\n");
        fflush(stdout);
    }
    fclose(f);
    printf("wpedeps: %d questions answered\n", no - unknown);
    return unknown ? 1 : 0;
}
