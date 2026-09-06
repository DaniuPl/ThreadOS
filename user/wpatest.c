/* wpatest - the cryptography of WPA2, checked against P802.11i
 *
 * Everything a station needs to join a protected network except the
 * radio: the password turning into a key, the four-way handshake
 * turning that into session keys, the group key travelling wrapped
 * inside another key, and a frame going out encrypted.
 *
 * The frames are checked against the vectors from P802.11i/D7.0
 * (section I.7.4) - real MPDUs with their headers, packet numbers and
 * signatures. A CCMP that encrypts correctly but assembles the
 * associated data wrongly agrees with itself and with no access point
 * in the world, and only a vector with a header in it catches that.
 *
 *   wpatest             - the vectors
 *   wpatest pochodne <s> - keys derived from a seed, printed, so that
 *                          the host can compute the same with its own
 *                          hashlib and compare
 */

#include "uapi.h"
#include "uwpa.h"

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
    print(": FAILED\n  came out: ");
    print_hex(got, n);
    print("\n  wanted:   ");
    print_hex(want, n);
    print("\n");
    failures++;
}

static void check_flag(const char *what, int ok)
{
    print(what);
    print(ok ? ": OK\n" : ": FAILED\n");
    if (!ok)
        failures++;
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

/* --- The three MPDUs from P802.11i ------------------------------- */

struct mpdu_vector {
    const char *name;
    uint8_t key[16];
    uint8_t keyid;
    uint8_t pn[6];
    uint8_t plain[64];
    uint32_t plainlen;
    uint8_t cipher[80];
    uint32_t cipherlen;
};

static const struct mpdu_vector vectors_11i[3] = {
    { "MPDU 1 (data, no QoS)",
      { 0xc9,0x7c,0x1f,0x67,0xce,0x37,0x11,0x85,
        0x51,0x4a,0x8a,0x19,0xf2,0xbd,0xd5,0x2f }, 0,
      { 0x0c,0xe7,0x76,0x97,0x03,0xb5 },
      { 0x08,0x48,0xc3,0x2c,0x0f,0xd2,0xe1,0x28,0xa5,0x7c,0x50,0x30,
        0xf1,0x84,0x44,0x08,0xab,0xae,0xa5,0xb8,0xfc,0xba,0x80,0x33,
        0xf8,0xba,0x1a,0x55,0xd0,0x2f,0x85,0xae,0x96,0x7b,0xb6,0x2f,
        0xb6,0xcd,0xa8,0xeb,0x7e,0x78,0xa0,0x50 }, 44,
      { 0x08,0x48,0xc3,0x2c,0x0f,0xd2,0xe1,0x28,0xa5,0x7c,0x50,0x30,
        0xf1,0x84,0x44,0x08,0xab,0xae,0xa5,0xb8,0xfc,0xba,0x80,0x33,
        0x0c,0xe7,0x00,0x20,0x76,0x97,0x03,0xb5,0xf3,0xd0,0xa2,0xfe,
        0x9a,0x3d,0xbf,0x23,0x42,0xa6,0x43,0xe4,0x32,0x46,0xe8,0x0c,
        0x3c,0x04,0xd0,0x19,0x78,0x45,0xce,0x0b,0x16,0xf9,0x76,0x23 }, 60 },

    { "MPDU 2 (another subtype, key no 2)",
      { 0x8f,0x7a,0x05,0x3f,0xa5,0x77,0xa5,0x59,
        0x75,0x29,0x27,0x20,0x97,0xa6,0x03,0xd5 }, 2,
      { 0xea,0x97,0xba,0xcb,0xf3,0x31 },
      { 0x38,0xc0,0x6a,0x51,0xea,0x10,0x0c,0x84,0x68,0x50,0xee,0xc1,
        0x76,0x2c,0x88,0xde,0xaf,0x2e,0xe9,0xf4,0x6a,0x07,0xe0,0xcc,
        0x83,0xa0,0x63,0x4b,0x5e,0xd7,0x62,0x7e,0xb9,0xdf,0x22,0x5e,
        0x05,0x74,0x03,0x42,0xde,0x19,0x41,0x17 }, 44,
      { 0x38,0xc0,0x6a,0x51,0xea,0x10,0x0c,0x84,0x68,0x50,0xee,0xc1,
        0x76,0x2c,0x88,0xde,0xaf,0x2e,0xe9,0xf4,0x6a,0x07,0xe0,0xcc,
        0xea,0x97,0x00,0xa0,0xba,0xcb,0xf3,0x31,0x81,0x4b,0x69,0x65,
        0xd0,0x5b,0xf2,0xb2,0xed,0x38,0xd4,0xbe,0xb0,0x69,0xfe,0x82,
        0x71,0x4a,0x61,0x0b,0x54,0x2f,0xbf,0x8d,0xa0,0x6a,0xa4,0xae }, 60 },

    { "MPDU 3 (QoS, a 26-byte header)",
      { 0x40,0xcf,0xb7,0xa6,0x2e,0x88,0x01,0x3b,
        0xd6,0xd3,0xaf,0xfc,0xc1,0x91,0x04,0x1e }, 2,
      { 0xae,0xa5,0xf3,0xa0,0xdc,0x2f },
      { 0xb8,0xc8,0xdc,0x61,0xd9,0x57,0x7d,0xf7,0x63,0xc8,0xb6,0xa8,
        0x8a,0xdf,0x36,0x91,0xdc,0x4a,0x8b,0xca,0x94,0xdd,0x60,0x82,
        0x20,0x85,
        0x2c,0x1b,0xd0,0x36,0x83,0x1c,0x95,0x49,0x6c,0x5f,0x4d,0xbf,
        0x3d,0x55,0x9e,0x72,0xde,0x80,0x2a,0x18 }, 46,
      { 0xb8,0xc8,0xdc,0x61,0xd9,0x57,0x7d,0xf7,0x63,0xc8,0xb6,0xa8,
        0x8a,0xdf,0x36,0x91,0xdc,0x4a,0x8b,0xca,0x94,0xdd,0x60,0x82,
        0x20,0x85,
        0xae,0xa5,0x00,0xa0,0xf3,0xa0,0xdc,0x2f,0x89,0xd8,0x58,0x03,
        0x40,0xb6,0x26,0xa0,0xb6,0xd4,0xd0,0x13,0xbf,0x18,0xf2,0x91,
        0xb8,0x96,0x46,0xc8,0xfd,0x1f,0x1f,0x61,0xa9,0xfb,0x4b,0xb3 }, 62 },
};

static void vectors(void)
{
    uint8_t out[128], back[128], pn[6];

    print("[wpa] vectors: FIPS 180, RFC 2202, RFC 3394, P802.11i\n");

    /* SHA-1 - here only because WPA2 asks for it. Nothing in this
       system signs anything with SHA-1. */
    static const uint8_t sha_abc[20] = {
        0xa9,0x99,0x3e,0x36,0x47,0x06,0x81,0x6a,0xba,0x3e,
        0x25,0x71,0x78,0x50,0xc2,0x6c,0x9c,0xd0,0xd8,0x9d };
    sha1("abc", 3, out);
    check("SHA-1('abc')", out, sha_abc, 20);

    static const uint8_t sha_two[20] = {
        0x84,0x98,0x3e,0x44,0x1c,0x3b,0xd2,0x6e,0xba,0xae,
        0x4a,0xa1,0xf9,0x51,0x29,0xe5,0xe5,0x46,0x70,0xf1 };
    sha1("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, out);
    check("SHA-1 (two blocks)", out, sha_two, 20);

    /* HMAC-SHA1, RFC 2202 case 1 */
    static const uint8_t hkey[20] = {
        0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,
        0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b };
    static const uint8_t hwant[20] = {
        0xb6,0x17,0x31,0x86,0x55,0x05,0x72,0x64,0xe2,0x8b,
        0xc0,0xb6,0xfb,0x37,0x8c,0x8e,0xf1,0x46,0xbe,0x00 };
    hmac_sha1(hkey, 20, "Hi There", 8, out);
    check("HMAC-SHA1 (RFC 2202 nr 1)", out, hwant, 20);

    /* The PSK from IEEE 802.11i: the password "password" on a network
       called "IEEE". Four thousand and ninety-six rounds - slowly, and
       that is the point of it. */
    static const uint8_t pmk_want[32] = {
        0xf4,0x2c,0x6f,0xc5,0x2d,0xf0,0xeb,0xef,0x9e,0xbb,0x4b,0x90,
        0xb3,0x8a,0x5f,0x90,0x2e,0x83,0xfe,0x1b,0x13,0x5a,0x70,0xe2,
        0x3a,0xed,0x76,0x2e,0x97,0x10,0xa1,0x2e };
    uint8_t pmk[32];
    wpa_pmk("password", "IEEE", pmk);
    check("PMK from a passphrase (802.11i, 4096 rounds)", pmk, pmk_want, 32);

    /* RFC 3394 4.1: a 128-bit key wrapped in a 128-bit one */
    static const uint8_t kek[16] = {
        0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
        0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f };
    static const uint8_t keydata[16] = {
        0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,
        0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff };
    static const uint8_t wrapped[24] = {
        0x1f,0xa6,0x8b,0x0a,0x81,0x12,0xb4,0x47,0xae,0xf3,0x4b,0xd8,
        0xfb,0x5a,0x7b,0x82,0x9d,0x3e,0x86,0x23,0x71,0xd2,0xcf,0xe5 };

    aes_wrap(kek, 128, keydata, 2, out);
    check("AES key wrap (RFC 3394 4.1)", out, wrapped, 24);
    check_flag("unwrapping gives the same key back",
               aes_unwrap(kek, 128, wrapped, 2, back) &&
               equal(back, keydata, 16));

    uint8_t broken[24];
    for (int i = 0; i < 24; i++)
        broken[i] = wrapped[i];
    broken[5] ^= 1;
    check_flag("one changed bit = the unwrap is refused",
               !aes_unwrap(kek, 128, broken, 2, back));

    /* And the frames themselves */
    for (int v = 0; v < 3; v++) {
        const struct mpdu_vector *m = &vectors_11i[v];
        uint32_t n = ccmp_encrypt(m->key, m->keyid, m->pn, m->plain,
                                  m->plainlen, out);

        print(m->name);
        print("\n");
        check_flag("  the frame length after encryption", n == m->cipherlen);
        if (n == m->cipherlen)
            check("  CCMP encrypts as in P802.11i", out, m->cipher, n);

        n = ccmp_decrypt(m->key, m->cipher, m->cipherlen, back, pn);
        check_flag("  and decrypts back again",
                   n == m->plainlen && equal(back, m->plain, n));
        check_flag("  the packet number read out of the header",
                   equal(pn, m->pn, 6));
    }

    /* What the associated data are for: an attacker changes the
       ADDRESS in the header, leaving the encrypted data untouched. The
       frame has to be dropped - otherwise a frame can be redirected
       without knowing the key. */
    const struct mpdu_vector *m = &vectors_11i[0];
    uint8_t moved[80];
    for (uint32_t i = 0; i < m->cipherlen; i++)
        moved[i] = m->cipher[i];
    moved[18] ^= 0x40;                    /* one bit of A3 */
    check_flag("a changed address in the header = the frame is refused",
               ccmp_decrypt(m->key, moved, m->cipherlen, back, pn) == 0);

    for (uint32_t i = 0; i < m->cipherlen; i++)
        moved[i] = m->cipher[i];
    moved[40] ^= 0x01;                    /* one bit of the data */
    check_flag("a changed data byte = the frame is refused",
               ccmp_decrypt(m->key, moved, m->cipherlen, back, pn) == 0);

    /* A frame from a different key - the whole point of the handshake */
    uint8_t otherkey[16];
    for (int i = 0; i < 16; i++)
        otherkey[i] = (uint8_t)(m->key[i] ^ 0x01);
    check_flag("a foreign key does not decrypt it",
               ccmp_decrypt(otherkey, m->cipher, m->cipherlen, back, pn) == 0);
}

/* --- Keys derived from a seed, printed for comparison ------------- */

static void derive(uint8_t *p, uint32_t n, uint32_t seed, uint8_t salt)
{
    uint8_t x = (uint8_t)(seed ^ salt);

    for (uint32_t i = 0; i < n; i++) {
        x = (uint8_t)(x * 73 + salt + i * 31 + 11);
        p[i] = x;
    }
}

static void derived(uint32_t seed)
{
    char pass[13], ssid[9];
    uint8_t raw[16];
    uint8_t pmk[32], ptk[48], mic[16];
    uint8_t aa[6], spa[6], anonce[32], snonce[32], frame[64];

    /* The password and the network name have to be text, so the bytes
       from the seed are pressed into letters. */
    derive(raw, 12, seed, 0x55);
    for (int i = 0; i < 12; i++)
        pass[i] = (char)('a' + raw[i] % 26);
    pass[12] = 0;
    derive(raw, 8, seed, 0x66);
    for (int i = 0; i < 8; i++)
        ssid[i] = (char)('a' + raw[i] % 26);
    ssid[8] = 0;

    derive(aa, 6, seed, 0x77);
    derive(spa, 6, seed, 0x88);
    derive(anonce, 32, seed, 0x99);
    derive(snonce, 32, seed, 0xAA);
    derive(frame, 64, seed, 0xBB);

    wpa_pmk(pass, ssid, pmk);
    wpa_ptk(pmk, aa, spa, anonce, snonce, ptk);
    wpa_eapol_mic(ptk, frame, 64, mic);   /* the KCK is the first part */

    print("[wpa] derived material\n");
    field("passwd", (const uint8_t *)pass, 12);
    field("network", (const uint8_t *)ssid, 8);
    field("pmk", pmk, 32);
    field("aa", aa, 6);
    field("spa", spa, 6);
    field("anonce", anonce, 32);
    field("snonce", snonce, 32);
    field("ptk", ptk, 48);
    field("frame", frame, 64);
    field("mic", mic, 16);
}

void _start(int argc, char **argv)
{
    if (argc >= 3 && argv[1][0] == 'p' && argv[1][1] == 'o')
        derived(from_hex(argv[2]));
    else
        vectors();

    if (failures) {
        print("[wpa] FAILURES: ");
        print_dec((uint32_t)failures);
        print("\n");
        sys_exit(1);
    }
    print("[wpa] everything matches\n");
    sys_exit(0);
}
