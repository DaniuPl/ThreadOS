/* aestest - AES, GCM and CCM against the published vectors
 *
 * Three things in one program, because they check the same code from
 * three sides:
 *
 *   with no arguments  - the official vectors: FIPS-197 for the cipher
 *                        itself, SP 800-38A for the counter mode, the
 *                        GCM specification and RFC 3610 for the two
 *                        authenticated modes;
 *   <mode> <seed> ...  - it encrypts material derived from a seed and
 *                        PRINTS everything it used, so that the test on
 *                        the host can encrypt the same with OpenSSL and
 *                        compare byte for byte;
 *   plik <name>        - it reads a ciphertext prepared OUTSIDE the
 *                        system and decrypts it. That is the direction
 *                        the vectors cannot check: our own encryption
 *                        agreeing with our own decryption proves
 *                        nothing about either.
 */

#include "uapi.h"
#include "uaes.h"

static int failures;

static void print_hex(const uint8_t *p, uint32_t n)
{
    const char *hex = "0123456789abcdef";
    char buf[3] = {0, 0, 0};

    for (uint32_t i = 0; i < n; i++) {
        buf[0] = hex[p[i] >> 4];
        buf[1] = hex[p[i] & 15];
        print(buf);
    }
}

static void field(const char *name, const uint8_t *p, uint32_t n)
{
    print(name);
    print("=");
    print_hex(p, n);
    print("\n");
}

static int equal(const uint8_t *a, const uint8_t *b, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

static void check(const char *what, const uint8_t *got, const uint8_t *want,
                  uint32_t n)
{
    print(what);
    if (equal(got, want, n)) {
        print(": OK\n");
        return;
    }
    print(": BLAD\n  wyszlo:   ");
    print_hex(got, n);
    print("\n  a mialo:  ");
    print_hex(want, n);
    print("\n");
    failures++;
}

static void check_flag(const char *what, int ok)
{
    print(what);
    print(ok ? ": OK\n" : ": BLAD\n");
    if (!ok)
        failures++;
}

static uint32_t to_num(const char *s)
{
    uint32_t v = 0;

    while (*s >= '0' && *s <= '9')
        v = v * 10 + (uint32_t)(*s++ - '0');
    return v;
}

static uint32_t from_hex(const char *s)
{
    uint32_t v = 0;

    for (; *s; s++) {
        if (*s >= '0' && *s <= '9')
            v = v * 16 + (uint32_t)(*s - '0');
        else if (*s >= 'a' && *s <= 'f')
            v = v * 16 + (uint32_t)(*s - 'a' + 10);
        else if (*s >= 'A' && *s <= 'F')
            v = v * 16 + (uint32_t)(*s - 'A' + 10);
        else
            break;
    }
    return v;
}

static int same_word(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* --- The official vectors --------------------------------------- */

static void vectors(void)
{
    struct aes a;
    uint8_t out[64], back[16];

    print("[aes] wektory: FIPS-197, SP 800-38A, GCM, RFC 3610\n");

    /* FIPS-197 C.1, C.2, C.3: one block, three key lengths. The key is
       00 01 .. and the input 00 11 22 .. - the same in all three. */
    static const uint8_t key256[32] = {
        0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,
        0x0c,0x0d,0x0e,0x0f,0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,
        0x18,0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f };
    static const uint8_t plain[16] = {
        0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,
        0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff };
    static const uint8_t c128[16] = {
        0x69,0xc4,0xe0,0xd8,0x6a,0x7b,0x04,0x30,
        0xd8,0xcd,0xb7,0x80,0x70,0xb4,0xc5,0x5a };
    static const uint8_t c192[16] = {
        0xdd,0xa9,0x7c,0xa4,0x86,0x4c,0xdf,0xe0,
        0x6e,0xaf,0x70,0xa0,0xec,0x0d,0x71,0x91 };
    static const uint8_t c256[16] = {
        0x8e,0xa2,0xb7,0xca,0x51,0x67,0x45,0xbf,
        0xea,0xfc,0x49,0x90,0x4b,0x49,0x60,0x89 };

    aes_key(&a, key256, 128);
    aes_encrypt_block(&a, plain, out);
    check("AES-128 blok (FIPS-197 C.1)", out, c128, 16);
    aes_decrypt_block(&a, out, back);
    check("AES-128 odwrotnie", back, plain, 16);

    aes_key(&a, key256, 192);
    aes_encrypt_block(&a, plain, out);
    check("AES-192 blok (FIPS-197 C.2)", out, c192, 16);
    aes_decrypt_block(&a, out, back);
    check("AES-192 odwrotnie", back, plain, 16);

    aes_key(&a, key256, 256);
    aes_encrypt_block(&a, plain, out);
    check("AES-256 blok (FIPS-197 C.3)", out, c256, 16);
    aes_decrypt_block(&a, out, back);
    check("AES-256 odwrotnie", back, plain, 16);

    /* SP 800-38A F.5.1: CTR-AES128. Four blocks - long enough for the
       counter to have to be counted up correctly. */
    static const uint8_t ctrkey[16] = {
        0x2b,0x7e,0x15,0x16,0x28,0xae,0xd2,0xa6,
        0xab,0xf7,0x15,0x88,0x09,0xcf,0x4f,0x3c };
    static const uint8_t ctr0[16] = {
        0xf0,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,
        0xf8,0xf9,0xfa,0xfb,0xfc,0xfd,0xfe,0xff };
    static const uint8_t ctrpt[64] = {
        0x6b,0xc1,0xbe,0xe2,0x2e,0x40,0x9f,0x96,0xe9,0x3d,0x7e,0x11,
        0x73,0x93,0x17,0x2a,0xae,0x2d,0x8a,0x57,0x1e,0x03,0xac,0x9c,
        0x9e,0xb7,0x6f,0xac,0x45,0xaf,0x8e,0x51,0x30,0xc8,0x1c,0x46,
        0xa3,0x5c,0xe4,0x11,0xe5,0xfb,0xc1,0x19,0x1a,0x0a,0x52,0xef,
        0xf6,0x9f,0x24,0x45,0xdf,0x4f,0x9b,0x17,0xad,0x2b,0x41,0x7b,
        0xe6,0x6c,0x37,0x10 };
    static const uint8_t ctrct[64] = {
        0x87,0x4d,0x61,0x91,0xb6,0x20,0xe3,0x26,0x1b,0xef,0x68,0x64,
        0x99,0x0d,0xb6,0xce,0x98,0x06,0xf6,0x6b,0x79,0x70,0xfd,0xff,
        0x86,0x17,0x18,0x7b,0xb9,0xff,0xfd,0xff,0x5a,0xe4,0xdf,0x3e,
        0xdb,0xd5,0xd3,0x5e,0x5b,0x4f,0x09,0x02,0x0d,0xb0,0x3e,0xab,
        0x1e,0x03,0x1d,0xda,0x2f,0xbe,0x03,0xd1,0x79,0x21,0x70,0xa0,
        0xf3,0x00,0x9c,0xee };
    uint8_t ctr[16], stream[64];

    for (int i = 0; i < 16; i++)
        ctr[i] = ctr0[i];
    aes_key(&a, ctrkey, 128);
    aes_ctr(&a, ctr, ctrpt, stream, 64);
    check("CTR-AES128 (SP 800-38A F.5.1)", stream, ctrct, 64);

    /* The GCM specification, test case 4: sixty bytes of message and
       twenty of associated data - a length that is not a multiple of
       the block, which is where a wrong padding of GHASH shows up. */
    static const uint8_t gkey[16] = {
        0xfe,0xff,0xe9,0x92,0x86,0x65,0x73,0x1c,
        0x6d,0x6a,0x8f,0x94,0x67,0x30,0x83,0x08 };
    static const uint8_t giv[12] = {
        0xca,0xfe,0xba,0xbe,0xfa,0xce,0xdb,0xad,0xde,0xca,0xf8,0x88 };
    static const uint8_t gaad[20] = {
        0xfe,0xed,0xfa,0xce,0xde,0xad,0xbe,0xef,0xfe,0xed,0xfa,0xce,
        0xde,0xad,0xbe,0xef,0xab,0xad,0xda,0xd2 };
    static const uint8_t gpt[60] = {
        0xd9,0x31,0x32,0x25,0xf8,0x84,0x06,0xe5,0xa5,0x59,0x09,0xc5,
        0xaf,0xf5,0x26,0x9a,0x86,0xa7,0xa9,0x53,0x15,0x34,0xf7,0xda,
        0x2e,0x4c,0x30,0x3d,0x8a,0x31,0x8a,0x72,0x1c,0x3c,0x0c,0x95,
        0x95,0x68,0x09,0x53,0x2f,0xcf,0x0e,0x24,0x49,0xa6,0xb5,0x25,
        0xb1,0x6a,0xed,0xf5,0xaa,0x0d,0xe6,0x57,0xba,0x63,0x7b,0x39 };
    static const uint8_t gct[60] = {
        0x42,0x83,0x1e,0xc2,0x21,0x77,0x74,0x24,0x4b,0x72,0x21,0xb7,
        0x84,0xd0,0xd4,0x9c,0xe3,0xaa,0x21,0x2f,0x2c,0x02,0xa4,0xe0,
        0x35,0xc1,0x7e,0x23,0x29,0xac,0xa1,0x2e,0x21,0xd5,0x14,0xb2,
        0x54,0x66,0x93,0x1c,0x7d,0x8f,0x6a,0x5a,0xac,0x84,0xaa,0x05,
        0x1b,0xa3,0x0b,0x39,0x6a,0x0a,0xac,0x97,0x3d,0x58,0xe0,0x91 };
    static const uint8_t gtag[16] = {
        0x5b,0xc9,0x4f,0xbc,0x32,0x21,0xa5,0xdb,
        0x94,0xfa,0xe9,0x5a,0xe7,0x12,0x1a,0x47 };
    uint8_t buf[64], tag[16], plainback[64];

    aes_gcm_encrypt(gkey, 128, giv, 12, gaad, 20, gpt, buf, 60, tag, 16);
    check("GCM-AES128 szyfr (przypadek 4)", buf, gct, 60);
    check("GCM-AES128 znacznik", tag, gtag, 16);
    check_flag("GCM odszyfrowanie",
               aes_gcm_decrypt(gkey, 128, giv, 12, gaad, 20, gct, plainback,
                               60, gtag, 16) &&
               equal(plainback, gpt, 60));

    /* One bit changed in the associated data - the tag has to catch it,
       otherwise the whole authentication is decoration. */
    uint8_t badaad[20];
    for (int i = 0; i < 20; i++)
        badaad[i] = gaad[i];
    badaad[19] ^= 1;
    check_flag("GCM wykrycie zmiany w naglowku",
               !aes_gcm_decrypt(gkey, 128, giv, 12, badaad, 20, gct,
                                plainback, 60, gtag, 16));

    /* RFC 3610, packet vector #1 - the shape WPA2 uses: a thirteen-byte
       nonce (so L=2) and an eight-byte tag. */
    static const uint8_t ckey[16] = {
        0xc0,0xc1,0xc2,0xc3,0xc4,0xc5,0xc6,0xc7,
        0xc8,0xc9,0xca,0xcb,0xcc,0xcd,0xce,0xcf };
    static const uint8_t cnonce[13] = {
        0x00,0x00,0x00,0x03,0x02,0x01,0x00,0xa0,0xa1,0xa2,0xa3,0xa4,0xa5 };
    static const uint8_t caad[8] = {
        0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07 };
    static const uint8_t cpt[23] = {
        0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,0x10,0x11,0x12,0x13,
        0x14,0x15,0x16,0x17,0x18,0x19,0x1a,0x1b,0x1c,0x1d,0x1e };
    static const uint8_t cct[23] = {
        0x58,0x8c,0x97,0x9a,0x61,0xc6,0x63,0xd2,0xf0,0x66,0xd0,0xc2,
        0xc0,0xf9,0x89,0x80,0x6d,0x5f,0x6b,0x61,0xda,0xc3,0x84 };
    static const uint8_t ctag[8] = {
        0x17,0xe8,0xd1,0x2c,0xfd,0xf9,0x26,0xe0 };

    check_flag("CCM parametry przyjete",
               aes_ccm_encrypt(ckey, 128, cnonce, 13, caad, 8, cpt, buf, 23,
                               tag, 8));
    check("CCM-AES128 szyfr (RFC 3610 nr 1)", buf, cct, 23);
    check("CCM-AES128 znacznik", tag, ctag, 8);
    check_flag("CCM odszyfrowanie",
               aes_ccm_decrypt(ckey, 128, cnonce, 13, caad, 8, cct, plainback,
                               23, ctag, 8) &&
               equal(plainback, cpt, 23));

    uint8_t badtag[8];
    for (int i = 0; i < 8; i++)
        badtag[i] = ctag[i];
    badtag[0] ^= 0x80;
    check_flag("CCM wykrycie podmiany znacznika",
               !aes_ccm_decrypt(ckey, 128, cnonce, 13, caad, 8, cct,
                                plainback, 23, badtag, 8));

    /* A tag that does not agree must not leave anything readable in the
       caller's buffer - CCM decrypts before it checks. */
    int leak = 0;
    for (int i = 0; i < 23; i++)
        if (plainback[i])
            leak = 1;
    check_flag("CCM po odrzuceniu nie zostawia tresci", !leak);

    /* CCM refuses parameters it cannot express: a nonce longer than
       thirteen bytes leaves no room for the length field. */
    check_flag("CCM odrzuca zly nonce",
               !aes_ccm_encrypt(ckey, 128, cnonce, 14, caad, 8, cpt, buf, 23,
                                tag, 8));
}

/* --- Material derived from a seed, and printed ------------------- */

static void derive(uint8_t *p, uint32_t n, uint32_t seed, uint8_t salt)
{
    uint8_t x = (uint8_t)(seed ^ salt);

    for (uint32_t i = 0; i < n; i++) {
        x = (uint8_t)(x * 73 + salt + i * 31 + 11);
        p[i] = x;
    }
}

#define MAXDATA 192

static void derived(const char *mode, uint32_t seed, uint32_t len,
                    uint32_t bits)
{
    uint8_t key[32], nonce[13], aad[20];
    uint8_t pt[MAXDATA], ct[MAXDATA], back[MAXDATA], tag[16];
    uint32_t keylen = bits / 8;
    /* "ccm" is the shape WPA2 uses: a thirteen-byte nonce and an
       eight-byte tag. "ccm7" is the other end of the range - a
       seven-byte nonce, so the length field takes eight bytes, which
       is where a thirty-two-bit shift would fall over. */
    uint32_t noncelen = same_word(mode, "ccm") ? 13
                      : same_word(mode, "ccm7") ? 7 : 12;
    uint32_t taglen = same_word(mode, "ccm") ? 8 : 16;
    struct aes a;

    if (len > MAXDATA)
        len = MAXDATA;

    derive(key, keylen, seed, 0x11);
    derive(nonce, noncelen, seed, 0x22);
    derive(aad, 20, seed, 0x33);
    derive(pt, len, seed, 0x44);

    print("[aes] ");
    print(mode);
    print(" ");
    print_dec(bits);
    print(" bitow, ");
    print_dec(len);
    print(" B\n");
    field("klucz", key, keylen);
    field("jawny", pt, len);

    if (same_word(mode, "ecb")) {
        for (uint32_t off = 0; off + 16 <= len; off += 16) {
            aes_key(&a, key, (int)bits);
            aes_encrypt_block(&a, pt + off, ct + off);
            aes_decrypt_block(&a, ct + off, back + off);
        }
        len -= len % 16;
        field("szyfr", ct, len);
        check_flag("powrot", equal(back, pt, len));
    } else if (same_word(mode, "ctr")) {
        uint8_t ctr[16];
        derive(ctr, 16, seed, 0x22);
        field("nonce", ctr, 16);
        aes_key(&a, key, (int)bits);
        aes_ctr(&a, ctr, pt, ct, len);
        field("szyfr", ct, len);
        derive(ctr, 16, seed, 0x22);
        aes_ctr(&a, ctr, ct, back, len);
        check_flag("powrot", equal(back, pt, len));
    } else if (same_word(mode, "gcm")) {
        field("nonce", nonce, noncelen);
        field("aad", aad, 20);
        aes_gcm_encrypt(key, (int)bits, nonce, noncelen, aad, 20,
                        pt, ct, len, tag, taglen);
        field("szyfr", ct, len);
        field("tag", tag, taglen);
        check_flag("powrot",
                   aes_gcm_decrypt(key, (int)bits, nonce, noncelen, aad, 20,
                                   ct, back, len, tag, taglen) &&
                   equal(back, pt, len));
    } else {
        field("nonce", nonce, noncelen);
        field("aad", aad, 20);
        if (!aes_ccm_encrypt(key, (int)bits, nonce, noncelen, aad, 20,
                             pt, ct, len, tag, taglen)) {
            print("[aes] CCM odmowil\n");
            failures++;
            return;
        }
        field("szyfr", ct, len);
        field("tag", tag, taglen);
        check_flag("powrot",
                   aes_ccm_decrypt(key, (int)bits, nonce, noncelen, aad, 20,
                                   ct, back, len, tag, taglen) &&
                   equal(back, pt, len));
    }
}

/* --- A ciphertext from outside the system ------------------------ */

/* The file: "AESV", the mode, the lengths, and then the key, the
   nonce, the associated data, the ciphertext and the tag one after
   another. Nothing clever - it exists so that the test can hand in
   something OpenSSL encrypted. */
#define HDR 12

static void from_file(const char *name)
{
    uint8_t buf[512];
    uint8_t pt[MAXDATA];
    int fd = sys_open(name, O_READ);
    int n = 0, got;

    if (fd < 0) {
        print("[aes] nie moge otworzyc '");
        print(name);
        print("'\n");
        sys_exit(1);
    }
    while ((got = sys_read(fd, (char *)buf + n, sizeof(buf) - (uint32_t)n)) > 0)
        n += got;
    sys_close(fd);

    if (n < HDR || buf[0] != 'A' || buf[1] != 'E' || buf[2] != 'S' ||
        buf[3] != 'V') {
        print("[aes] to nie jest plik z wektorem\n");
        sys_exit(1);
    }

    uint32_t mode = buf[4], keylen = buf[5], noncelen = buf[6];
    uint32_t aadlen = buf[7], taglen = buf[8];
    uint32_t ctlen = (uint32_t)buf[10] | ((uint32_t)buf[11] << 8);
    const uint8_t *key = buf + HDR;
    const uint8_t *nonce = key + keylen;
    const uint8_t *aad = nonce + noncelen;
    const uint8_t *ct = aad + aadlen;
    const uint8_t *tag = ct + ctlen;

    if (ctlen > MAXDATA ||
        HDR + keylen + noncelen + aadlen + ctlen + taglen > (uint32_t)n) {
        print("[aes] plik urwany\n");
        sys_exit(1);
    }

    print("[aes] plik: ");
    print(mode == 1 ? "gcm" : "ccm");
    print(", ");
    print_dec(ctlen);
    print(" B\n");

    int ok = mode == 1
        ? aes_gcm_decrypt(key, (int)keylen * 8, nonce, noncelen, aad, aadlen,
                          ct, pt, ctlen, tag, taglen)
        : aes_ccm_decrypt(key, (int)keylen * 8, nonce, noncelen, aad, aadlen,
                          ct, pt, ctlen, tag, taglen);

    if (!ok) {
        print("[aes] tag NIE zgadza sie\n");
        sys_exit(0);
    }
    print("[aes] tag zgadza sie\n");
    field("jawny", pt, ctlen);
}

void _start(int argc, char **argv)
{
    if (argc >= 3 && same_word(argv[1], "plik")) {
        from_file(argv[2]);
        sys_exit(0);
    }

    if (argc >= 4) {
        uint32_t bits = argc >= 5 ? to_num(argv[4]) : 128;

        if (bits != 128 && bits != 192 && bits != 256)
            bits = 128;
        derived(argv[1], from_hex(argv[2]), to_num(argv[3]), bits);
    } else {
        vectors();
    }

    if (failures) {
        print("[aes] BLEDOW: ");
        print_dec((uint32_t)failures);
        print("\n");
        sys_exit(1);
    }
    print("[aes] wszystko sie zgadza\n");
    sys_exit(0);
}
