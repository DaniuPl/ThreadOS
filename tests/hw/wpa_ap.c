/* The access point's side of WPA2-Personal, with OpenSSL (see wpa_ap.h) */

#include <stdio.h>
#include <string.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include "wpa_ap.h"

#define KI_VER_AES   2
#define KI_PAIRWISE  0x0008
#define KI_INSTALL   0x0040
#define KI_ACK       0x0080
#define KI_MIC       0x0100
#define KI_SECURE    0x0200
#define KI_ENC       0x1000

static const uint8_t rsn_ccmp_psk[] = {
    0x30, 20, 1, 0, 0x00, 0x0F, 0xAC, 4,
    1, 0, 0x00, 0x0F, 0xAC, 4,
    1, 0, 0x00, 0x0F, 0xAC, 2,
    0x0C, 0x00,     /* capabilities: 16 replay counters, as hostapd's */
};

static void fail(struct ap *ap, const char *why)
{
    snprintf(ap->why, sizeof(ap->why), "%s", why);
}

void ap_init(struct ap *ap, const char *password, const char *ssid,
             const uint8_t aa[6], const uint8_t spa[6],
             const uint8_t *sta_ie, int sta_ie_len)
{
    memset(ap, 0, sizeof(*ap));
    PKCS5_PBKDF2_HMAC_SHA1(password, (int)strlen(password),
                           (const unsigned char *)ssid, (int)strlen(ssid),
                           4096, 32, ap->pmk);
    memcpy(ap->aa, aa, 6);
    memcpy(ap->spa, spa, 6);
    memcpy(ap->ie, rsn_ccmp_psk, sizeof(rsn_ccmp_psk));
    ap->ie_len = sizeof(rsn_ccmp_psk);
    if (sta_ie_len > 0 && sta_ie_len <= 64) {
        memcpy(ap->sta_ie, sta_ie, (size_t)sta_ie_len);
        ap->sta_ie_len = sta_ie_len;
    }
    RAND_bytes(ap->anonce, 32);
    RAND_bytes(ap->gtk, 16);
    ap->gtk_idx = 1;
}

/* PRF-384 (IEEE 802.11 12.7.1.2) */
static void prf384(const uint8_t *key, const char *label, const uint8_t *data,
                   size_t data_len, uint8_t out[48])
{
    uint8_t in[200];
    size_t ll = strlen(label);
    memcpy(in, label, ll);
    in[ll] = 0;
    memcpy(in + ll + 1, data, data_len);
    size_t n = ll + 1 + data_len;
    for (int i = 0, done = 0; done < 48; i++) {
        uint8_t h[20];
        unsigned hl = 20;
        in[n] = (uint8_t)i;
        HMAC(EVP_sha1(), key, 32, in, n + 1, h, &hl);
        int take = 48 - done < 20 ? 48 - done : 20;
        memcpy(out + done, h, (size_t)take);
        done += take;
    }
}

static void mic_of(const uint8_t kck[16], const uint8_t *frame, int len,
                   uint8_t mic[16])
{
    uint8_t copy[2048];
    uint8_t h[20];
    unsigned hl = 20;
    memcpy(copy, frame, (size_t)len);
    memset(copy + AP_K_MIC, 0, 16);
    HMAC(EVP_sha1(), kck, 16, copy, (size_t)len, h, &hl);
    memcpy(mic, h, 16);
}

void ap_sign(const struct ap *ap, uint8_t *frame, int len)
{
    uint8_t mic[16];
    mic_of(ap->ptk, frame, len, mic);
    memcpy(frame + AP_K_MIC, mic, 16);
}

static int frame(struct ap *ap, uint8_t *out, unsigned info, int key_len,
                 const uint8_t *nonce, const uint8_t *data, int data_len)
{
    int len = AP_K_DATA + data_len;
    memset(out, 0, (size_t)len);
    out[0] = 2;                     /* 802.1X-2004 */
    out[1] = 3;                     /* key */
    out[2] = (uint8_t)((len - 4) >> 8);
    out[3] = (uint8_t)(len - 4);
    out[4] = 2;                     /* RSN */
    out[5] = (uint8_t)(info >> 8);
    out[6] = (uint8_t)info;
    out[7] = (uint8_t)(key_len >> 8);
    out[8] = (uint8_t)key_len;
    ap->replay++;
    for (int i = 0; i < 8; i++)
        out[AP_K_REPLAY + i] = (uint8_t)(ap->replay >> (56 - 8 * i));
    if (nonce)
        memcpy(out + AP_K_NONCE, nonce, 32);
    out[AP_K_DATALEN] = (uint8_t)(data_len >> 8);
    out[AP_K_DATALEN + 1] = (uint8_t)data_len;
    if (data_len)
        memcpy(out + AP_K_DATA, data, (size_t)data_len);
    if (info & KI_MIC)
        ap_sign(ap, out, len);
    return len;
}

int ap_msg1(struct ap *ap, uint8_t *out)
{
    return frame(ap, out, KI_VER_AES | KI_PAIRWISE | KI_ACK, 16,
                 ap->anonce, NULL, 0);
}

static unsigned info_of(const uint8_t *in)
{
    return ((unsigned)in[AP_K_INFO] << 8) | in[AP_K_INFO + 1];
}

static int check_frame(struct ap *ap, const uint8_t *in, int len)
{
    if (len < AP_K_DATA || in[1] != 3 || in[4] != 2) {
        fail(ap, "not an RSN key frame");
        return -1;
    }
    int body = (in[2] << 8) | in[3];
    int dl = (in[AP_K_DATALEN] << 8) | in[AP_K_DATALEN + 1];
    if (body + 4 != len || AP_K_DATA + dl != len) {
        fail(ap, "its lengths do not add up");
        return -1;
    }
    uint64_t r = 0;
    for (int i = 0; i < 8; i++)
        r = (r << 8) | in[AP_K_REPLAY + i];
    if (r != ap->replay) {
        fail(ap, "its replay counter is not the one asked with");
        return -1;
    }
    return 0;
}

static int check_mic(struct ap *ap, const uint8_t *in, int len)
{
    uint8_t mic[16];
    mic_of(ap->ptk, in, len, mic);
    if (memcmp(mic, in + AP_K_MIC, 16)) {
        fail(ap, "its MIC is wrong");
        return -1;
    }
    return 0;
}

int ap_rx_msg2(struct ap *ap, const uint8_t *in, int len)
{
    if (check_frame(ap, in, len) < 0)
        return -1;
    if (info_of(in) != (KI_VER_AES | KI_PAIRWISE | KI_MIC)) {
        fail(ap, "message 2's key information is not version 2, pairwise, MIC");
        return -1;
    }
    memcpy(ap->snonce, in + AP_K_NONCE, 32);
    uint8_t d[76];
    int a = memcmp(ap->aa, ap->spa, 6) < 0;
    memcpy(d, a ? ap->aa : ap->spa, 6);
    memcpy(d + 6, a ? ap->spa : ap->aa, 6);
    int n = memcmp(ap->anonce, ap->snonce, 32) < 0;
    memcpy(d + 12, n ? ap->anonce : ap->snonce, 32);
    memcpy(d + 44, n ? ap->snonce : ap->anonce, 32);
    prf384(ap->pmk, "Pairwise key expansion", d, sizeof(d), ap->ptk);
    if (check_mic(ap, in, len) < 0)
        return -1;
    int dl = (in[AP_K_DATALEN] << 8) | in[AP_K_DATALEN + 1];
    if (dl != ap->sta_ie_len || memcmp(in + AP_K_DATA, ap->sta_ie, (size_t)dl)) {
        fail(ap, "message 2's RSN element is not the association request's");
        return -1;
    }
    ap->ptk_ok = 1;
    return 0;
}

/* Wraps key data, padded as the standard says (0xDD, then zeros) */
static int wrap(const struct ap *ap, const uint8_t *plain, int n, uint8_t *out)
{
    uint8_t buf[256];
    memcpy(buf, plain, (size_t)n);
    if (n % 8 || n < 16) {
        buf[n++] = 0xDD;
        while (n % 8 || n < 16)
            buf[n++] = 0;
    }
    EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
    EVP_CIPHER_CTX_set_flags(c, EVP_CIPHER_CTX_FLAG_WRAP_ALLOW);
    EVP_EncryptInit_ex(c, EVP_aes_128_wrap(), NULL, ap->ptk + 16, NULL);
    int outl = 0, fin = 0;
    EVP_EncryptUpdate(c, out, &outl, buf, n);
    EVP_EncryptFinal_ex(c, out + outl, &fin);
    EVP_CIPHER_CTX_free(c);
    return outl + fin;
}

static int gtk_kde(const struct ap *ap, uint8_t *out)
{
    out[0] = 0xDD;
    out[1] = 6 + 16;
    out[2] = 0x00; out[3] = 0x0F; out[4] = 0xAC; out[5] = 1;
    out[6] = (uint8_t)(ap->gtk_idx & 3);
    out[7] = 0;
    memcpy(out + 8, ap->gtk, 16);
    return 24;
}

int ap_msg3(struct ap *ap, uint8_t *out)
{
    uint8_t plain[128], wrapped[160];
    memcpy(plain, ap->ie, (size_t)ap->ie_len);
    int n = ap->ie_len + gtk_kde(ap, plain + ap->ie_len);
    int w = wrap(ap, plain, n, wrapped);
    return frame(ap, out, KI_VER_AES | KI_PAIRWISE | KI_INSTALL | KI_ACK |
                 KI_MIC | KI_SECURE | KI_ENC, 16, ap->anonce, wrapped, w);
}

int ap_rx_msg4(struct ap *ap, const uint8_t *in, int len)
{
    if (check_frame(ap, in, len) < 0)
        return -1;
    if (info_of(in) != (KI_VER_AES | KI_PAIRWISE | KI_MIC | KI_SECURE)) {
        fail(ap, "message 4's key information is not pairwise, MIC, secure");
        return -1;
    }
    if ((in[AP_K_DATALEN] | in[AP_K_DATALEN + 1]) != 0) {
        fail(ap, "message 4 carries key data");
        return -1;
    }
    return check_mic(ap, in, len);
}

int ap_group1(struct ap *ap, int idx, uint8_t *out)
{
    RAND_bytes(ap->gtk, 16);
    ap->gtk_idx = idx;
    uint8_t plain[32], wrapped[64];
    int w = wrap(ap, plain, gtk_kde(ap, plain), wrapped);
    return frame(ap, out, KI_VER_AES | KI_ACK | KI_MIC | KI_SECURE | KI_ENC,
                 0, NULL, wrapped, w);
}

int ap_rx_group2(struct ap *ap, const uint8_t *in, int len)
{
    if (check_frame(ap, in, len) < 0)
        return -1;
    if (info_of(in) != (KI_VER_AES | KI_MIC | KI_SECURE)) {
        fail(ap, "the group message 2's key information is not MIC, secure");
        return -1;
    }
    return check_mic(ap, in, len);
}
