/* WPA2-Personal's four-way handshake: kernel/wpa.c against an access
 * point written with OpenSSL (tests/hw/wpa_ap.c)
 *
 * The kernel's code, compiled as it is with the sanitizers, plays the
 * station; the access point is written from the standard with another
 * library's primitives. A key both work out alike is the key a real
 * access point would, and every frame the station sends is checked the
 * way an access point checks it. Then what an attacker or a damaged
 * frame does: each must be refused, and leave the keys as they were.
 *
 * Prints "ok: ..." / "FAIL: ..." per check.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include "kernel/include/wpa.h"
#include "wpa_ap.h"

static int failures;

static void check(const char *what, int ok)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok)
        failures++;
}

static const uint8_t AA[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };
static const uint8_t SPA[6] = { 0x7C, 0x5C, 0xF8, 0x01, 0x02, 0x03 };
static const char *SSID = "ThreadOS test";
static const char *PASS = "correct horse battery";

static char hex[200];

static const char *tohex(const uint8_t *p, int n)
{
    for (int i = 0; i < n && i < 90; i++)
        sprintf(hex + 2 * i, "%02x", p[i]);
    return hex;
}

/* IEEE 802.11 J.4: the PSK test vectors */
static void psk_vectors(void)
{
    static const struct { const char *pass, *ssid, *pmk; } v[] = {
        { "password", "IEEE",
          "f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e" },
        { "ThisIsAPassword", "ThisIsASSID",
          "0dc0d6eb90555ed6419756b9a15ec3e3209b63df707dd508d14581f8982721af" },
    };
    for (unsigned i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
        uint8_t pmk[32];
        wpa_password_pmk(v[i].pass, (const uint8_t *)v[i].ssid,
                         (uint32_t)strlen(v[i].ssid), pmk);
        char what[160];
        snprintf(what, sizeof(what), "the PMK of \"%s\" on \"%s\" is the "
                 "standard's (802.11 J.4)", v[i].pass, v[i].ssid);
        check(what, !strcmp(tohex(pmk, 32), v[i].pmk));
    }
    /* and of passwords of every length, against OpenSSL's PBKDF2 */
    int same = 1;
    for (int len = 8; len <= 63; len++) {
        char pass[64];
        for (int i = 0; i < len; i++)
            pass[i] = (char)(' ' + (i * 7 + len) % 94);
        pass[len] = 0;
        uint8_t a[32], b[32];
        wpa_password_pmk(pass, (const uint8_t *)"net", 3, a);
        PKCS5_PBKDF2_HMAC_SHA1(pass, len, (const unsigned char *)"net", 3,
                               4096, 32, b);
        same &= !memcmp(a, b, 32);
    }
    check("the PMK of passwords of 8 to 63 characters is OpenSSL's", same);
}

/* RFC 3394 4.1: 128 bits of key data with a 128-bit KEK */
static void unwrap_vector(void)
{
    static const uint8_t kek[16] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F };
    static const uint8_t wrapped[24] = {
        0x1F, 0xA6, 0x8B, 0x0A, 0x81, 0x12, 0xB4, 0x47,
        0xAE, 0xF3, 0x4B, 0xD8, 0xFB, 0x5A, 0x7B, 0x82,
        0x9D, 0x3E, 0x86, 0x23, 0x71, 0xD2, 0xCF, 0xE5 };
    static const uint8_t plain[16] = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF };
    uint8_t out[16];
    int r = wpa_aes_unwrap(kek, wrapped, 24, out);
    check("AES key unwrap gives RFC 3394's key data (4.1)",
          r == 0 && !memcmp(out, plain, 16));
    uint8_t bad[24];
    memcpy(bad, wrapped, 24);
    bad[20] ^= 1;
    check("and refuses it with one bit changed",
          wpa_aes_unwrap(kek, bad, 24, out) < 0);
}

struct run {
    struct ap ap;
    struct wpa_sm sm;
    uint8_t m[1024], r[WPA_FRAME_MAX];
    uint32_t rlen;
    int m3len;
    uint8_t m3[1024];
};

/* A station that knows the password, up to message 2 answered */
static int start(struct run *x, const char *pass, int ap_ie_known)
{
    uint8_t pmk[32], snonce[32], ie[64];
    uint32_t ie_len = wpa_rsn_ie(ie);
    ap_init(&x->ap, PASS, SSID, AA, SPA, ie, (int)ie_len);
    wpa_password_pmk(pass, (const uint8_t *)SSID, (uint32_t)strlen(SSID), pmk);
    RAND_bytes(snonce, 32);
    wpa_begin(&x->sm, pmk, AA, SPA, ap_ie_known ? x->ap.ie : NULL,
              ap_ie_known ? (uint32_t)x->ap.ie_len : 0, snonce);
    int n = ap_msg1(&x->ap, x->m);
    int a = wpa_rx(&x->sm, x->m, (uint32_t)n, x->r, sizeof(x->r), &x->rlen);
    if (a != WPA_REPLY)
        return -1;
    return ap_rx_msg2(&x->ap, x->r, (int)x->rlen);
}

static void handshake(void)
{
    struct run *x = calloc(1, sizeof(*x));
    uint8_t ie[64];
    uint32_t ie_len = wpa_rsn_ie(ie);
    check("our RSN element is one a CCMP/PSK access point takes",
          wpa_rsn_usable(ie, ie_len) == 0);
    int r = start(x, PASS, 1);
    check("the access point's RSN element (CCMP/CCMP/PSK) is usable",
          wpa_rsn_usable(x->ap.ie, (uint32_t)x->ap.ie_len) == 0);
    uint8_t tkip[64];
    memcpy(tkip, x->ap.ie, (size_t)x->ap.ie_len);
    tkip[13] = 2;                           /* pairwise TKIP only */
    check("one that offers only TKIP for our traffic is not",
          wpa_rsn_usable(tkip, (uint32_t)x->ap.ie_len) < 0);
    char what[300];
    snprintf(what, sizeof(what), "message 1 answered with a message 2 the "
             "access point takes: its SNonce, its RSN element, its MIC (%s)",
             r ? x->ap.why : "taken");
    check(what, r == 0);
    int n = ap_msg3(&x->ap, x->m3);
    x->m3len = n;
    int a = wpa_rx(&x->sm, x->m3, (uint32_t)n, x->r, sizeof(x->r), &x->rlen);
    snprintf(what, sizeof(what), "message 3 taken, answered with message 4 "
             "(%s)", a == WPA_KEYS ? "keys" : x->sm.why);
    check(what, a == WPA_KEYS);
    r = ap_rx_msg4(&x->ap, x->r, (int)x->rlen);
    snprintf(what, sizeof(what), "message 4 is what the access point takes "
             "(%s)", r ? x->ap.why : "taken");
    check(what, r == 0);
    check("the pairwise key (TK) is the access point's",
          !memcmp(x->sm.keys.tk, x->ap.ptk + 32, 16));
    check("the group key, its index and length are the access point's",
          x->sm.keys.gtk_len == 16 && x->sm.keys.gtk_idx == x->ap.gtk_idx &&
          !memcmp(x->sm.keys.gtk, x->ap.gtk, 16));
    check("the handshake is done", x->sm.state == WPA_DONE);

    /* A replayed message 3: the counter has not grown */
    uint8_t tk[16];
    memcpy(tk, x->sm.keys.tk, 16);
    a = wpa_rx(&x->sm, x->m3, (uint32_t)x->m3len, x->r, sizeof(x->r), &x->rlen);
    snprintf(what, sizeof(what), "message 3 again, replayed, is refused (%s)",
             x->sm.why);
    check(what, a == WPA_DROP && !memcmp(tk, x->sm.keys.tk, 16));

    /* A new group key */
    n = ap_group1(&x->ap, 2, x->m);
    a = wpa_rx(&x->sm, x->m, (uint32_t)n, x->r, sizeof(x->r), &x->rlen);
    r = a == WPA_GROUP ? ap_rx_group2(&x->ap, x->r, (int)x->rlen) : -1;
    snprintf(what, sizeof(what), "the group handshake: the new group key "
             "(index 2) taken and answered as the access point wants (%s)",
             a != WPA_GROUP ? x->sm.why : r ? x->ap.why : "taken");
    check(what, a == WPA_GROUP && r == 0 && x->sm.keys.gtk_idx == 2 &&
          !memcmp(x->sm.keys.gtk, x->ap.gtk, 16));

    /* An unsigned message 1 from somebody else, then a group key frame
       the real access point signed: the keys in use still work */
    n = ap_msg1(&x->ap, x->m);
    x->m[AP_K_NONCE] ^= 0xFF;               /* another ANonce */
    a = wpa_rx(&x->sm, x->m, (uint32_t)n, x->r, sizeof(x->r), &x->rlen);
    n = ap_group1(&x->ap, 1, x->m);
    a = wpa_rx(&x->sm, x->m, (uint32_t)n, x->r, sizeof(x->r), &x->rlen);
    snprintf(what, sizeof(what), "a stranger's message 1 does not change "
             "the keys in use: the next group key still opens (%s)",
             a == WPA_GROUP ? "it does" : x->sm.why);
    check(what, a == WPA_GROUP && !memcmp(tk, x->sm.keys.tk, 16));
    wpa_end(&x->sm);
    free(x);
}

/* A damaged message 3, by what the damage is */
static void bad_msg3(const char *what, void (*damage)(struct run *x),
                     int ap_ie_known)
{
    struct run *x = calloc(1, sizeof(*x));
    if (start(x, PASS, ap_ie_known) < 0) {
        check(what, 0);
        free(x);
        return;
    }
    x->m3len = ap_msg3(&x->ap, x->m3);
    damage(x);
    int a = wpa_rx(&x->sm, x->m3, (uint32_t)x->m3len, x->r, sizeof(x->r),
                   &x->rlen);
    char line[300];
    snprintf(line, sizeof(line), "%s (%s)", what, x->sm.why);
    check(line, a == WPA_DROP && x->sm.state != WPA_DONE && x->rlen == 0);
    free(x);
}

static void flip_data(struct run *x) { x->m3[AP_K_DATA + 5] ^= 0x10; }
static void flip_mic(struct run *x) { x->m3[AP_K_MIC] ^= 1; }
static void other_anonce(struct run *x)
{
    x->m3[AP_K_NONCE + 3] ^= 1;
    ap_sign(&x->ap, x->m3, x->m3len);
}
static void no_install(struct run *x)
{
    x->m3[AP_K_INFO + 1] &= (uint8_t)~0x40;
    ap_sign(&x->ap, x->m3, x->m3len);
}
static void long_data(struct run *x)
{
    x->m3[AP_K_DATALEN + 1] += 8;           /* past the frame's end */
}
static void downgrade(struct run *x)
{
    /* A man in the middle: the beacon said CCMP; message 3, rebuilt
       with another element, says TKIP for the group */
    x->ap.ie[7] = 2;
    x->m3len = ap_msg3(&x->ap, x->m3);
}

static void wrong_password(void)
{
    struct run *x = calloc(1, sizeof(*x));
    int r = start(x, "not the password", 1);
    char what[300];
    snprintf(what, sizeof(what), "another password: the access point "
             "refuses message 2 (%s)", r ? x->ap.why : "taken");
    check(what, r < 0);
    /* and message 3 built with the right key does not hold up either */
    x->m3len = ap_msg3(&x->ap, x->m3);
    int a = wpa_rx(&x->sm, x->m3, (uint32_t)x->m3len, x->r, sizeof(x->r),
                   &x->rlen);
    snprintf(what, sizeof(what), "and the station refuses a message 3 "
             "signed with the other key (%s)", x->sm.why);
    check(what, a == WPA_DROP && x->sm.state != WPA_DONE);
    free(x);
}

static void odd_frames(void)
{
    struct run *x = calloc(1, sizeof(*x));
    start(x, PASS, 1);
    int all = 1;
    /* every shorter cut of message 3, and a few bytes of nothing */
    uint8_t m3[1024];
    int n = ap_msg3(&x->ap, m3);
    for (int len = 0; len < n; len++) {
        uint32_t out_len = 1;
        int a = wpa_rx(&x->sm, m3, (uint32_t)len, x->r, sizeof(x->r), &out_len);
        all &= a == WPA_DROP && out_len == 0;
    }
    check("message 3 cut short anywhere is refused, and nothing is sent", all);
    uint8_t group_first[1024];
    struct run *y = calloc(1, sizeof(*y));
    start(y, PASS, 1);
    n = ap_group1(&y->ap, 2, group_first);
    int a = wpa_rx(&y->sm, group_first, (uint32_t)n, y->r, sizeof(y->r),
                   &y->rlen);
    check("a group key before the handshake is done is refused",
          a == WPA_DROP);
    free(y);
    free(x);
}

int main(void)
{
    psk_vectors();
    unwrap_vector();
    handshake();
    bad_msg3("message 3 with its key data changed is refused", flip_data, 1);
    bad_msg3("message 3 with its MIC changed is refused", flip_mic, 1);
    bad_msg3("message 3 with another ANonce, signed, is refused",
             other_anonce, 1);
    bad_msg3("message 3 without Install is refused", no_install, 1);
    bad_msg3("message 3 whose key data runs past its end is refused",
             long_data, 1);
    bad_msg3("message 3 whose RSN element is not the beacon's is refused",
             downgrade, 1);
    wrong_password();
    odd_frames();
    return failures ? 1 : 0;
}
