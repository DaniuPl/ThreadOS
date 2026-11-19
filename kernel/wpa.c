/* WPA2-Personal: the supplicant's side of the four-way handshake
 *
 * What IEEE 802.11-2020 12.7 asks of a station joining a network with a
 * pre-shared key and CCMP, and nothing more: no TKIP, no WPA1, no
 * fast roaming, no SAE. The access point speaks first and leads; we
 * answer, and refuse anything that does not hold up:
 *
 *   message 1  ANonce, unsigned       -> we work out the PTK, answer
 *   message 2  SNonce, our RSN element, signed with the KCK
 *   message 3  ANonce again, the access point's RSN element and the
 *              group key wrapped with the KEK, signed
 *                                     -> checked, then
 *   message 4  signed, empty          -> the keys go into the card
 *
 * and later, whenever the access point changes the group key, a
 * two-message group handshake signed and wrapped the same way.
 *
 * Every frame comes from the air, so every length is checked before it
 * is believed. A signed frame (MIC) must carry a replay counter larger
 * than the last signed one did, and message 3 must repeat message 1's
 * ANonce and the RSN element the access point's beacon had - a man in
 * the middle offering a weaker cipher shows up there.
 *
 * The cryptography is the programs' (user/uwpa.h: the PRF, the PTK and
 * the frames' MIC, over user/usha1.h; user/uaes.h's key unwrap), checked
 * by t_wpa and t_aes against the standards' vectors; this file is the
 * protocol around it.
 */

#include <stdint.h>
#include "include/wpa.h"
#include "include/string.h"
/* the cryptography alone, not the programs' system calls beside it */
#define THREADOS_UAPI_H
#include "../user/uwpa.h"
#undef THREADOS_UAPI_H

/* EAPOL (IEEE 802.1X-2004) and its key frames */
#define EAPOL_HDR          4            /* version, type, body length */
#define EAPOL_KEY          3
#define KEY_DESC_RSN       2
#define KEY_FRAME_FIXED    (EAPOL_HDR + 95)   /* everything before key data */

/* Offsets from the start of the EAPOL frame */
#define K_DESC             4
#define K_INFO             5
#define K_KEYLEN           7
#define K_REPLAY           9
#define K_NONCE            17
#define K_IV               49
#define K_RSC              65
#define K_MIC              81
#define K_DATALEN          97
#define K_DATA             99

/* Key information */
#define KI_VERSION         0x0007
#define KI_VERSION_AES     2            /* HMAC-SHA1-128 MIC, AES key wrap */
#define KI_PAIRWISE        0x0008
#define KI_INSTALL         0x0040
#define KI_ACK             0x0080
#define KI_MIC             0x0100
#define KI_SECURE          0x0200
#define KI_ERROR           0x0400
#define KI_REQUEST         0x0800
#define KI_ENCRYPTED       0x1000

/* The elements: RSN, and the vendor-specific one the key data elements
   (KDEs) wear */
#define IE_RSN             0x30
#define IE_VENDOR          0xDD
static const uint8_t ieee_oui[3] = { 0x00, 0x0F, 0xAC };
#define KDE_GTK            1
#define SUITE_CCMP         4
#define AKM_PSK            2

/* A memset the compiler may not drop: keys are not left lying about */
static void wipe(void *buf, uint32_t len)
{
    volatile uint8_t *p = buf;
    while (len--)
        *p++ = 0;
}

static uint32_t be16(const uint8_t *p)
{
    return ((uint32_t)p[0] << 8) | p[1];
}

static void put_be16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void refuse(struct wpa_sm *sm, const char *why)
{
    uint32_t i = 0;
    while (why[i] && i + 1 < sizeof(sm->why)) {
        sm->why[i] = why[i];
        i++;
    }
    sm->why[i] = 0;
}

/* Compares in constant time: a MIC is a secret's shadow */
static int same(const uint8_t *a, const uint8_t *b, uint32_t n)
{
    uint8_t d = 0;
    for (uint32_t i = 0; i < n; i++)
        d |= (uint8_t)(a[i] ^ b[i]);
    return d == 0;
}

void wpa_password_pmk(const char *password, const uint8_t *ssid,
                      uint32_t ssid_len, uint8_t pmk[WPA_PMK_LEN])
{
    uint32_t n = 0;
    while (password[n])
        n++;
    pbkdf2_sha1((const uint8_t *)password, n, ssid, ssid_len > 32 ? 32 : ssid_len,
                4096, pmk, WPA_PMK_LEN);
}

uint32_t wpa_rsn_ie(uint8_t *out)
{
    static const uint8_t ie[] = {
        IE_RSN, 20,
        1, 0,                           /* version 1 */
        0x00, 0x0F, 0xAC, SUITE_CCMP,   /* group cipher */
        1, 0, 0x00, 0x0F, 0xAC, SUITE_CCMP,   /* one pairwise cipher */
        1, 0, 0x00, 0x0F, 0xAC, AKM_PSK,      /* one key management */
        0, 0,                           /* capabilities */
    };
    memcpy(out, ie, sizeof(ie));
    return sizeof(ie);
}

/* A list of suites - a count, then four bytes each - holding this one */
static int has_suite(const uint8_t *p, uint32_t left, uint32_t *used,
                     uint8_t type)
{
    if (left < 2)
        return -1;
    uint32_t n = p[0] | ((uint32_t)p[1] << 8);
    if (n == 0 || left - 2 < n * 4)
        return -1;
    *used = 2 + n * 4;
    int found = 0;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *s = p + 2 + i * 4;
        if (s[0] == 0x00 && s[1] == 0x0F && s[2] == 0xAC && s[3] == type)
            found = 1;
    }
    return found ? 0 : -1;
}

int wpa_rsn_usable(const uint8_t *ie, uint32_t len)
{
    if (len < 2 || ie[0] != IE_RSN || ie[1] + 2u > len || ie[1] < 8)
        return -1;
    const uint8_t *p = ie + 2;
    uint32_t left = ie[1], used;
    if (p[0] != 1 || p[1] != 0)
        return -1;                      /* version 1 */
    if (memcmp(p + 2, ieee_oui, 3) || p[5] != SUITE_CCMP)
        return -1;                      /* the group's cipher */
    p += 6;
    left -= 6;
    if (has_suite(p, left, &used, SUITE_CCMP) < 0)
        return -1;
    p += used;
    left -= used;
    if (has_suite(p, left, &used, AKM_PSK) < 0)
        return -1;
    return 0;
}

void wpa_begin(struct wpa_sm *sm, const uint8_t pmk[WPA_PMK_LEN],
               const uint8_t aa[6], const uint8_t spa[6],
               const uint8_t *ap_ie, uint32_t ap_ie_len,
               const uint8_t snonce[WPA_NONCE_LEN])
{
    memset(sm, 0, sizeof(*sm));
    memcpy(sm->pmk, pmk, WPA_PMK_LEN);
    memcpy(sm->aa, aa, 6);
    memcpy(sm->spa, spa, 6);
    sm->ie_len = wpa_rsn_ie(sm->ie);
    if (ap_ie && ap_ie_len && ap_ie_len <= WPA_IE_MAX) {
        memcpy(sm->ap_ie, ap_ie, ap_ie_len);
        sm->ap_ie_len = ap_ie_len;
    }
    memcpy(sm->snonce, snonce, WPA_NONCE_LEN);
    sm->state = WPA_IDLE;
}

void wpa_end(struct wpa_sm *sm)
{
    wipe(sm, sizeof(*sm));
}

/* The PTK to try, from message 1's ANonce */
static void derive_ptk(struct wpa_sm *sm)
{
    wpa_ptk(sm->pmk, sm->aa, sm->spa, sm->anonce, sm->snonce, sm->tptk);
    sm->tptk_set = 1;
}

/* HMAC-SHA1-128 over the whole frame with its MIC field zero */
static void frame_mic(const uint8_t kck[WPA_KCK_LEN], const uint8_t *frame,
                      uint32_t len, uint8_t mic[16])
{
    uint8_t copy[WPA_FRAME_MAX + 512];
    memcpy(copy, frame, len);
    memset(copy + K_MIC, 0, 16);
    wpa_eapol_mic(kck, copy, len, mic);
}

int wpa_aes_unwrap(const uint8_t kek[16], const uint8_t *in, uint32_t in_len,
                   uint8_t *out)
{
    if (in_len < 24 || in_len % 8)
        return -1;
    return aes_unwrap(kek, 128, in, in_len / 8 - 1, out) ? 0 : -1;
}

/* The replay counter of a signed frame must have grown */
static int fresh(const struct wpa_sm *sm, const uint8_t *c)
{
    if (!sm->replay_set)
        return 1;
    for (int i = 0; i < 8; i++)
        if (c[i] != sm->replay[i])
            return c[i] > sm->replay[i];
    return 0;
}

/* Our answer: a key frame with no key data but what is given, signed */
static uint32_t answer(const uint8_t *kck, const uint8_t *in, uint32_t info,
                       const uint8_t *nonce, const uint8_t *data,
                       uint32_t data_len, uint8_t *out)
{
    uint32_t len = K_DATA + data_len;
    memset(out, 0, len);
    out[0] = 1;                         /* 802.1X-2001, as most send */
    out[1] = EAPOL_KEY;
    put_be16(out + 2, len - EAPOL_HDR);
    out[K_DESC] = KEY_DESC_RSN;
    put_be16(out + K_INFO, info);
    memcpy(out + K_REPLAY, in + K_REPLAY, 8);
    if (nonce)
        memcpy(out + K_NONCE, nonce, WPA_NONCE_LEN);
    put_be16(out + K_DATALEN, data_len);
    if (data_len)
        memcpy(out + K_DATA, data, data_len);
    uint8_t mic[16];
    frame_mic(kck, out, len, mic);
    memcpy(out + K_MIC, mic, 16);
    return len;
}

/* Finds the elements in decrypted key data: the RSN element and the
   group key. 0, or -1 if the data is not well formed */
static int key_data(struct wpa_sm *sm, const uint8_t *d, uint32_t n,
                    const uint8_t **rsn, uint32_t *rsn_len, int need_gtk)
{
    int got_gtk = 0;
    *rsn = 0;
    *rsn_len = 0;
    for (uint32_t i = 0; i < n; ) {
        if (d[i] == IE_VENDOR && (i + 1 >= n || d[i + 1] == 0))
            break;                      /* the padding: 0xDD then zeros */
        if (i + 2 > n || i + 2 + d[i + 1] > n) {
            refuse(sm, "key data: an element runs past its end");
            return -1;
        }
        const uint8_t *e = d + i;
        uint32_t len = e[1];
        if (e[0] == IE_RSN) {
            *rsn = e;
            *rsn_len = len + 2;
        } else if (e[0] == IE_VENDOR && len >= 4 &&
                   !memcmp(e + 2, ieee_oui, 3) && e[5] == KDE_GTK) {
            /* key id and Tx in one byte, a reserved one, the key */
            if (len < 6 + 16 || len - 6 > WPA_GTK_MAX) {
                refuse(sm, "key data: the group key's length is wrong");
                return -1;
            }
            sm->keys.gtk_idx = e[6] & 3;
            sm->keys.gtk_len = len - 6;
            memcpy(sm->keys.gtk, e + 8, len - 6);
            if (sm->keys.gtk_idx == 0) {
                refuse(sm, "key data: group key index 0");
                return -1;
            }
            got_gtk = 1;
        }
        i += 2 + len;
    }
    if (need_gtk && !got_gtk) {
        refuse(sm, "key data: no group key");
        return -1;
    }
    return 0;
}

/* Checks the signature and unwraps the key data of a signed frame;
   plain receives its plaintext. Returns the plaintext's length or -1 */
static int open_signed(struct wpa_sm *sm, const uint8_t *ptk,
                       const uint8_t *in, uint32_t len, uint8_t *plain,
                       uint32_t cap)
{
    if (!fresh(sm, in + K_REPLAY)) {
        refuse(sm, "the replay counter did not grow");
        return -1;
    }
    uint8_t mic[16];
    frame_mic(ptk, in, len, mic);
    if (!same(mic, in + K_MIC, 16)) {
        refuse(sm, "the MIC is wrong (another password?)");
        return -1;
    }
    uint32_t n = be16(in + K_DATALEN);
    if (n < 24 || n % 8 || n - 8 > cap) {
        refuse(sm, "the wrapped key data has a wrong length");
        return -1;
    }
    if (wpa_aes_unwrap(ptk + WPA_KCK_LEN, in + K_DATA, n, plain) < 0) {
        refuse(sm, "the key data does not unwrap");
        return -1;
    }
    /* Signed and whole: from now on only a larger counter will do */
    memcpy(sm->replay, in + K_REPLAY, 8);
    sm->replay_set = 1;
    return (int)(n - 8);
}

int wpa_rx(struct wpa_sm *sm, const uint8_t *in, uint32_t in_len,
           uint8_t *out, uint32_t out_cap, uint32_t *out_len)
{
    *out_len = 0;
    sm->why[0] = 0;
    if (out_cap < WPA_FRAME_MAX) {
        refuse(sm, "no room for an answer");
        return WPA_DROP;
    }
    if (in_len < K_DATA || in[1] != EAPOL_KEY) {
        refuse(sm, "not an EAPOL key frame");
        return WPA_DROP;
    }
    uint32_t body = be16(in + 2);
    if (body + EAPOL_HDR > in_len || body + EAPOL_HDR < K_DATA) {
        refuse(sm, "the EAPOL length is wrong");
        return WPA_DROP;
    }
    in_len = body + EAPOL_HDR;          /* the rest is the frame's padding */
    if (in[K_DESC] != KEY_DESC_RSN) {
        refuse(sm, "not an RSN key descriptor");
        return WPA_DROP;
    }
    uint32_t info = be16(in + K_INFO);
    if ((info & KI_VERSION) != KI_VERSION_AES) {
        refuse(sm, "a key descriptor version other than AES");
        return WPA_DROP;
    }
    if (K_DATA + be16(in + K_DATALEN) > in_len ||
        in_len > WPA_FRAME_MAX + 512) {
        refuse(sm, "the key data runs past the frame");
        return WPA_DROP;
    }
    if (!(info & KI_ACK) || (info & (KI_REQUEST | KI_ERROR))) {
        refuse(sm, "not a frame an access point sends");
        return WPA_DROP;
    }
    uint32_t ver = KI_VERSION_AES;

    if ((info & KI_PAIRWISE) && !(info & KI_MIC)) {
        /* Message 1: a new ANonce, a new PTK to try, the same SNonce */
        if (info & (KI_INSTALL | KI_SECURE | KI_ENCRYPTED)) {
            refuse(sm, "message 1 with bits only message 3 has");
            return WPA_DROP;
        }
        memcpy(sm->anonce, in + K_NONCE, WPA_NONCE_LEN);
        derive_ptk(sm);
        *out_len = answer(sm->tptk, in, ver | KI_PAIRWISE | KI_MIC,
                          sm->snonce, sm->ie, sm->ie_len, out);
        if (sm->state != WPA_DONE)      /* the keys in use stay in use */
            sm->state = WPA_STARTED;
        return WPA_REPLY;
    }

    uint8_t plain[512];
    if (info & KI_PAIRWISE) {
        /* Message 3 */
        uint32_t want = KI_PAIRWISE | KI_INSTALL | KI_ACK | KI_MIC |
                        KI_SECURE | KI_ENCRYPTED;
        if ((info & want) != want) {
            refuse(sm, "message 3 without the bits it must have");
            return WPA_DROP;
        }
        if (!sm->tptk_set ||
            !same(in + K_NONCE, sm->anonce, WPA_NONCE_LEN)) {
            refuse(sm, "message 3's ANonce is not message 1's");
            return WPA_DROP;
        }
        int n = open_signed(sm, sm->tptk, in, in_len, plain, sizeof(plain));
        if (n < 0)
            return WPA_DROP;
        const uint8_t *rsn;
        uint32_t rsn_len;
        struct wpa_keys keep = sm->keys;
        if (key_data(sm, plain, (uint32_t)n, &rsn, &rsn_len, 1) < 0) {
            sm->keys = keep;
            wipe(plain, sizeof(plain));
            return WPA_DROP;
        }
        if (!rsn || (sm->ap_ie_len && (rsn_len != sm->ap_ie_len ||
                                       memcmp(rsn, sm->ap_ie, rsn_len)))) {
            refuse(sm, "message 3's RSN element is not the beacon's");
            sm->keys = keep;
            wipe(plain, sizeof(plain));
            return WPA_DROP;
        }
        wipe(plain, sizeof(plain));
        memcpy(sm->ptk, sm->tptk, WPA_PTK_LEN);
        sm->ptk_set = 1;
        memcpy(sm->keys.tk, sm->ptk + WPA_KCK_LEN + WPA_KEK_LEN, WPA_TK_LEN);
        memcpy(sm->keys.gtk_rsc, in + K_RSC, 8);
        *out_len = answer(sm->ptk, in, ver | KI_PAIRWISE | KI_MIC | KI_SECURE,
                          0, 0, 0, out);
        sm->state = WPA_DONE;
        return WPA_KEYS;
    }

    /* The group handshake's message 1: a new group key */
    uint32_t want = KI_ACK | KI_MIC | KI_SECURE | KI_ENCRYPTED;
    if ((info & want) != want || sm->state != WPA_DONE || !sm->ptk_set) {
        refuse(sm, "a group key frame out of place");
        return WPA_DROP;
    }
    int n = open_signed(sm, sm->ptk, in, in_len, plain, sizeof(plain));
    if (n < 0)
        return WPA_DROP;
    const uint8_t *rsn;
    uint32_t rsn_len;
    struct wpa_keys keep = sm->keys;
    if (key_data(sm, plain, (uint32_t)n, &rsn, &rsn_len, 1) < 0) {
        sm->keys = keep;
        wipe(plain, sizeof(plain));
        return WPA_DROP;
    }
    wipe(plain, sizeof(plain));
    memcpy(sm->keys.gtk_rsc, in + K_RSC, 8);
    *out_len = answer(sm->ptk, in, ver | KI_MIC | KI_SECURE, 0, 0, 0, out);
    return WPA_GROUP;
}
