/* IEEE 802.11 frames for a station (IEEE 802.11-2020 9.3 and 9.4)
 *
 * The station's side of joining a network the plain way - open system
 * authentication, then association - and of passing Ethernet frames
 * through it, as a card with the 802.11 MAC in its firmware wants them:
 * whole frames, the header written by us, sequence numbers and the
 * encryption left to the card. Nothing but bytes here; kernel/iwlwifi.c
 * sends and receives them.
 *
 * No 802.11n or WMM is asked for: the association request offers the
 * legacy rates alone (up to 54 Mb/s), and the access point then talks
 * to us without QoS data frames - though they are read if it sends
 * some.
 */

#include <stdint.h>
#include "include/wlan.h"
#include "include/string.h"

/* The elements */
#define IE_SSID      0
#define IE_RATES     1
#define IE_DS        3
#define IE_TIM       5
#define IE_RSN       48
#define IE_EXT_RATES 50
#define IE_VENDOR    221

/* The rates this station speaks, in 500 kb/s: CCK, then OFDM */
static const uint8_t our_rates[] = { 2, 4, 11, 22, 12, 18, 24, 36, 48, 72,
                                     96, 108 };

static uint32_t le16(const uint8_t *p)
{
    return p[0] | ((uint32_t)p[1] << 8);
}

static void put_le16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

/* The management header: to the access point, from us */
static void mgmt_hdr(uint8_t *f, uint8_t fc, const uint8_t bssid[6],
                     const uint8_t sa[6])
{
    memset(f, 0, WLAN_HDR);
    f[0] = fc;
    memcpy(f + 4, bssid, 6);
    memcpy(f + 10, sa, 6);
    memcpy(f + 16, bssid, 6);
}

/* A management frame from the access point to us, of this subtype */
static int mgmt_for_us(const uint8_t *f, uint32_t n, uint8_t fc,
                       const uint8_t me[6], const uint8_t bssid[6])
{
    return n >= WLAN_HDR && f[0] == fc && !memcmp(f + 4, me, 6) &&
           !memcmp(f + 10, bssid, 6) && !memcmp(f + 16, bssid, 6);
}

int wlan_parse_beacon(const uint8_t *f, uint32_t n, struct wlan_bss_info *b)
{
    if (n < 36 || (f[0] != WLAN_FC_BEACON && f[0] != WLAN_FC_PROBE_RESP))
        return -1;
    memset(b, 0, sizeof(*b));
    memcpy(b->bssid, f + 16, 6);
    for (int i = 7; i >= 0; i--)
        b->tsf = (b->tsf << 8) | f[24 + i];
    b->beacon_int = (uint16_t)le16(f + 32);
    b->capability = (uint16_t)le16(f + 34);
    for (uint32_t i = 36; i + 2 <= n;) {
        uint8_t id = f[i], len = f[i + 1];
        const uint8_t *v = f + i + 2;
        if (i + 2 + len > n)
            return -1;
        if (id == IE_SSID && len <= 32) {
            memcpy(b->ssid, v, len);
            b->ssid_len = len;
        } else if (id == IE_RATES || id == IE_EXT_RATES) {
            for (uint32_t k = 0; k < len && b->n_rates < sizeof(b->rates); k++)
                b->rates[b->n_rates++] = v[k];
        } else if (id == IE_DS && len == 1) {
            b->channel = v[0];
        } else if (id == IE_TIM && len >= 4) {
            b->has_tim = 1;
            b->dtim_count = v[0];
            b->dtim_period = v[1];
        } else if (id == IE_RSN && len + 2u <= sizeof(b->rsn)) {
            memcpy(b->rsn, f + i, len + 2u);
            b->rsn_len = (uint8_t)(len + 2);
        } else if (id == IE_VENDOR && len >= 5 && v[0] == 0x00 &&
                   v[1] == 0x50 && v[2] == 0xF2 && v[3] == 2) {
            b->wmm = 1;
        }
        i += 2u + len;
    }
    return 0;
}

uint32_t wlan_auth_req(uint8_t *f, const uint8_t bssid[6],
                       const uint8_t sa[6])
{
    mgmt_hdr(f, WLAN_FC_AUTH, bssid, sa);
    put_le16(f + 24, 0);            /* open system */
    put_le16(f + 26, 1);            /* the first of two */
    put_le16(f + 28, 0);            /* status */
    return 30;
}

int wlan_auth_resp(const uint8_t *f, uint32_t n, const uint8_t me[6],
                   const uint8_t bssid[6], uint16_t *status)
{
    if (!mgmt_for_us(f, n, WLAN_FC_AUTH, me, bssid) || n < 30)
        return -1;
    if (le16(f + 24) != 0 || le16(f + 26) != 2)
        return -1;
    *status = (uint16_t)le16(f + 28);
    return 0;
}

/* Whether the access point offers a rate (with or without its basic bit) */
static int offered(const struct wlan_bss_info *b, uint8_t r)
{
    for (uint32_t i = 0; i < b->n_rates; i++)
        if ((b->rates[i] & 0x7F) == r)
            return 1;
    return 0;
}

uint32_t wlan_assoc_req(uint8_t *f, const struct wlan_bss_info *b,
                        const uint8_t sa[6], const uint8_t *rsn,
                        uint32_t rsn_len)
{
    mgmt_hdr(f, WLAN_FC_ASSOC_REQ, b->bssid, sa);
    uint16_t cap = WLAN_CAP_ESS;
    if (b->capability & WLAN_CAP_PRIVACY)
        cap |= WLAN_CAP_PRIVACY;
    /* short preamble and slot where the network uses them: the card
       speaks both */
    cap |= b->capability & (WLAN_CAP_SHORT_PRE | WLAN_CAP_SHORT_SLOT);
    put_le16(f + 24, cap);
    put_le16(f + 26, 10);           /* listen interval, in beacons */
    uint32_t n = 28;
    f[n++] = IE_SSID;
    f[n++] = b->ssid_len;
    memcpy(f + n, b->ssid, b->ssid_len);
    n += b->ssid_len;
    /* The rates both sides speak: eight in the first element, the rest
       in the extended one. A network that offers none of ours (it
       did say it has rates) gets all of ours. */
    uint8_t r[sizeof(our_rates)];
    uint32_t nr = 0;
    for (uint32_t i = 0; i < sizeof(our_rates); i++)
        if (!b->n_rates || offered(b, our_rates[i]))
            r[nr++] = our_rates[i];
    if (!nr) {
        memcpy(r, our_rates, sizeof(our_rates));
        nr = sizeof(our_rates);
    }
    uint32_t first = nr > 8 ? 8 : nr;
    f[n++] = IE_RATES;
    f[n++] = (uint8_t)first;
    memcpy(f + n, r, first);
    n += first;
    if (nr > first) {
        f[n++] = IE_EXT_RATES;
        f[n++] = (uint8_t)(nr - first);
        memcpy(f + n, r + first, nr - first);
        n += nr - first;
    }
    if (rsn && rsn_len) {
        memcpy(f + n, rsn, rsn_len);
        n += rsn_len;
    }
    return n;
}

int wlan_assoc_resp(const uint8_t *f, uint32_t n, const uint8_t me[6],
                    const uint8_t bssid[6], uint16_t *status, uint16_t *aid)
{
    if (!mgmt_for_us(f, n, WLAN_FC_ASSOC_RESP, me, bssid) || n < 30)
        return -1;
    *status = (uint16_t)le16(f + 26);
    *aid = (uint16_t)(le16(f + 28) & 0x3FFF);   /* the top bits are set */
    return 0;
}

int wlan_kicked(const uint8_t *f, uint32_t n, const uint8_t me[6],
                const uint8_t bssid[6])
{
    if (n < 26 || (!mgmt_for_us(f, n, WLAN_FC_DEAUTH, me, bssid) &&
                   !mgmt_for_us(f, n, WLAN_FC_DISASSOC, me, bssid)))
        return -1;
    return (int)le16(f + 24);
}

static const uint8_t snap[6] = { 0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00 };

uint32_t wlan_from_eth(const uint8_t *eth, uint32_t n, const uint8_t bssid[6],
                       int protect, uint8_t hdr[WLAN_HDR], uint8_t *body)
{
    if (n < 14 || n - 14 + 8 > WLAN_FRAME_MAX - WLAN_HDR - WLAN_CCMP_HDR -
                               WLAN_CCMP_MIC)
        return 0;
    memset(hdr, 0, WLAN_HDR);
    hdr[0] = WLAN_FC_DATA;
    hdr[1] = WLAN_FC1_TODS | (protect ? WLAN_FC1_PROTECTED : 0);
    memcpy(hdr + 4, bssid, 6);          /* receiver: the access point */
    memcpy(hdr + 10, eth + 6, 6);       /* transmitter: us */
    memcpy(hdr + 16, eth, 6);           /* where it is going */
    memcpy(body, snap, 6);
    memcpy(body + 6, eth + 12, 2);
    memcpy(body + 8, eth + 14, n - 14);
    return n - 14 + 8;
}

int wlan_to_eth(const uint8_t *f, uint32_t n, const uint8_t bssid[6],
                int ccmp, uint8_t *eth)
{
    if (n < WLAN_HDR || (f[0] & 0x0C) != 0x08)
        return -1;                      /* not a data frame */
    if (f[0] & 0x40)
        return -1;                      /* a null frame: nothing in it */
    if ((f[1] & (WLAN_FC1_TODS | WLAN_FC1_FROMDS)) != WLAN_FC1_FROMDS ||
        memcmp(f + 10, bssid, 6))
        return -1;                      /* not from our access point */
    uint32_t h = WLAN_HDR + ((f[0] & 0x80) ? 2 : 0);   /* QoS control */
    uint32_t end = n;
    if (ccmp) {
        if (!(f[1] & WLAN_FC1_PROTECTED))
            return -1;
        h += WLAN_CCMP_HDR;
        if (end < WLAN_CCMP_MIC)
            return -1;
        end -= WLAN_CCMP_MIC;
    }
    if (end < h + 8 || memcmp(f + h, snap, 6) ||
        end - h - 8 + 14 > WLAN_FRAME_MAX)
        return -1;
    memcpy(eth, f + 4, 6);              /* destination: us or a group */
    memcpy(eth + 6, f + 16, 6);         /* source */
    memcpy(eth + 12, f + h + 6, 2);
    memcpy(eth + 14, f + h + 8, end - h - 8);
    return (int)(end - h - 8 + 14);
}
