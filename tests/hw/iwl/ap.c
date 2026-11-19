/* The access point the model's card joins (stage 3)
 *
 * "ThreadOS-Home" on channel 6, WPA2-PSK with CCMP, as the scan already
 * hears it. What the station sends over the air comes here as plain
 * 802.11 frames; what the access point answers goes back through the
 * firmware's receive path (fw.c). The 802.11 side is written from IEEE
 * 802.11-2020 9.3/9.4 and 12.7, apart from the driver's kernel/wlan.c,
 * and the handshake is tests/hw/wpa_ap.c's, on OpenSSL.
 *
 * Every rule a station's frame breaks is a violation, named after the
 * clause; the access point then does what a real one does - nothing,
 * or a refusal.
 */

#include <stdio.h>
#include <string.h>

#include "card.h"
#include "ap.h"
#include "../wpa_ap.h"

const uint8_t ap_bssid[6] = { 0x02, 0x7A, 0x00, 0x00, 0x00, 0x01 };
const char ap_ssid[] = "ThreadOS-Home";
const char ap_password[] = "threados rocks";
#define AP_AID 1

struct ap_state aps;
static struct ap wpa;

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

void ap_reset(const uint8_t sta[6])
{
    memset(&aps, 0, sizeof(aps));
    memcpy(aps.sta, sta, 6);
}

/* A management frame from the access point to the station */
static int mgmt(uint8_t *f, uint8_t fc)
{
    memset(f, 0, 24);
    f[0] = fc;
    memcpy(f + 4, aps.sta, 6);
    memcpy(f + 10, ap_bssid, 6);
    memcpy(f + 16, ap_bssid, 6);
    return 24;
}

/* An EAPOL frame to the station, as a data frame from the DS */
static void send_eapol(const uint8_t *e, int n)
{
    uint8_t f[1200];
    memset(f, 0, 24);
    f[0] = 0x08;
    f[1] = 0x02;                            /* FromDS */
    memcpy(f + 4, aps.sta, 6);
    memcpy(f + 10, ap_bssid, 6);
    memcpy(f + 16, ap_bssid, 6);
    static const uint8_t snap[8] = { 0xAA, 0xAA, 3, 0, 0, 0, 0x88, 0x8E };
    memcpy(f + 24, snap, 8);
    memcpy(f + 32, e, (size_t)n);
    fw_air_rx(f, (uint32_t)(32 + n), 0);
}

/* The association request's elements, held to what the station may
   ask of this network */
static int assoc_ok(const uint8_t *f, uint32_t n, uint8_t *sta_ie, int *sta_ie_len)
{
    if (n < 28) {
        violation("an association request of %u bytes: its fixed fields "
                  "alone are 4 (802.11 9.3.3.6)", n);
        return 0;
    }
    uint16_t cap = le16(f + 24);
    if (!(cap & 0x0001))
        violation("the association request's capabilities lack ESS "
                  "(802.11 9.4.1.4)");
    if (!(cap & 0x0010))
        violation("the association request to a protected network lacks "
                  "the privacy bit");
    int ssid_ok = 0, rates = 0, rsn = 0;
    *sta_ie_len = 0;
    for (uint32_t i = 28; i < n;) {
        if (i + 2 > n || i + 2 + f[i + 1] > n) {
            violation("an association request element runs past the frame");
            return 0;
        }
        uint8_t id = f[i], len = f[i + 1];
        const uint8_t *v = f + i + 2;
        if (id == 0)
            ssid_ok = len == strlen(ap_ssid) && !memcmp(v, ap_ssid, len);
        else if (id == 1 || id == 50) {
            for (int k = 0; k < len; k++) {
                uint8_t r = v[k] & 0x7F;
                static const uint8_t ours[] = { 2, 4, 11, 22, 12, 18, 24,
                                                36, 48, 72, 96, 108 };
                if (!memchr(ours, r, sizeof(ours)))
                    violation("the association request offers rate %u "
                              "(x500 kb/s), which the network never named",
                              r);
                rates++;
            }
            if (id == 1 && len > 8)
                violation("more than 8 rates in Supported Rates (9.4.2.3)");
        } else if (id == 48) {
            rsn = 1;
            if (len + 2 <= 64) {
                memcpy(sta_ie, f + i, len + 2u);
                *sta_ie_len = len + 2;
            }
        }
        i += 2u + len;
    }
    if (!ssid_ok)
        violation("the association request's SSID is not \"%s\"", ap_ssid);
    if (!rates)
        violation("the association request offers no rates");
    if (!rsn)
        violation("the association request to a WPA2 network has no RSN "
                  "element (12.6.2)");
    return ssid_ok && rates && rsn;
}

void ap_frame(const uint8_t *f, uint32_t n)
{
    if (n < 24)
        return;
    if (memcmp(f + 4, ap_bssid, 6) && !(f[4] & 1))
        return;                             /* for somebody else */
    if (memcmp(f + 10, aps.sta, 6)) {
        violation("a frame sent with transmitter %02x:...:%02x, not the "
                  "card's address", f[10], f[15]);
        return;
    }
    uint8_t out[600];
    switch (f[0]) {
    case 0xB0: {                            /* authentication */
        if (n < 30 || le16(f + 24) != 0 || le16(f + 26) != 1) {
            violation("an authentication frame that is not open system, "
                      "sequence 1 (802.11 9.3.3.12)");
            return;
        }
        if (memcmp(f + 16, ap_bssid, 6))
            violation("the authentication frame's BSSID is not the access "
                      "point's");
        aps.auth++;
        int k = mgmt(out, 0xB0);
        put16(out + k, 0);
        put16(out + k + 2, 2);
        put16(out + k + 4, 0);              /* success */
        fw_air_rx(out, (uint32_t)(k + 6), 0);
        return;
    }
    case 0x00: {                            /* association request */
        if (!aps.auth) {
            violation("an association request before authentication");
            return;
        }
        uint8_t ie[64];
        int ie_len;
        if (!assoc_ok(f, n, ie, &ie_len)) {
            int k = mgmt(out, 0x10);
            put16(out + k, 0x0411);
            put16(out + k + 2, 12);         /* refused, unspecified */
            put16(out + k + 4, 0);
            fw_air_rx(out, (uint32_t)(k + 6), 0);
            return;
        }
        aps.assoc++;
        fj.assoc_answered = 1;
        int k = mgmt(out, 0x10);
        put16(out + k, 0x0411);             /* ESS, privacy, short slot */
        put16(out + k + 2, 0);
        put16(out + k + 4, 0xC000 | AP_AID);
        static const uint8_t rates[] = { 1, 8, 0x82, 0x84, 0x8B, 0x96,
                                         0x0C, 0x12, 0x18, 0x24,
                                         50, 4, 0x30, 0x48, 0x60, 0x6C };
        memcpy(out + k + 6, rates, sizeof(rates));
        fw_air_rx(out, (uint32_t)(k + 6 + sizeof(rates)), 0);
        /* the handshake begins with the access point */
        ap_init(&wpa, ap_password, ap_ssid, ap_bssid, aps.sta, ie, ie_len);
        uint8_t e[256];
        int m = ap_msg1(&wpa, e);
        send_eapol(e, m);
        aps.msg1++;
        return;
    }
    case 0x08:
    case 0x88: {                            /* data */
        if ((f[1] & 3) != 1) {
            violation("a data frame from the station without ToDS alone "
                      "(802.11 9.2.4.1.4)");
            return;
        }
        if (memcmp(f + 4, ap_bssid, 6)) {
            violation("a data frame to the DS whose receiver is not the "
                      "access point");
            return;
        }
        uint32_t h = 24 + ((f[0] & 0x80) ? 2 : 0);
        if (n < h + 8 || f[h] != 0xAA || f[h + 1] != 0xAA || f[h + 2] != 3) {
            violation("a data frame without LLC/SNAP");
            return;
        }
        uint16_t type = (uint16_t)(f[h + 6] << 8 | f[h + 7]);
        if (type == 0x888E) {
            if (f[1] & 0x40)
                aps.eapol_protected++;
            ap_eapol(f + h + 8, (int)(n - h - 8));
        } else {
            aps.data_frames++;
            memcpy(aps.last_data, f, n < sizeof(aps.last_data) ? n
                   : sizeof(aps.last_data));
            aps.last_len = n;
        }
        return;
    }
    default:
        return;
    }
}

void ap_eapol(const uint8_t *e, int n)
{
    uint8_t out[256];
    if (aps.msg1 && !aps.msg2_ok) {
        if (ap_rx_msg2(&wpa, e, n) < 0) {
            note("EAPOL message 2 refused by the access point: %s", wpa.why);
            aps.msg2_refused++;
            return;
        }
        aps.msg2_ok = 1;
        int m = ap_msg3(&wpa, out);
        aps.msg3++;
        send_eapol(out, m);
        return;
    }
    if (aps.msg2_ok && !aps.msg4_ok) {
        if (ap_rx_msg4(&wpa, e, n) < 0) {
            violation("EAPOL message 4 refused by the access point: %s",
                      wpa.why);
            return;
        }
        aps.msg4_ok = 1;
        memcpy(aps.tk, wpa.ptk + 32, 16);
        memcpy(aps.gtk, wpa.gtk, 16);
        aps.gtk_idx = wpa.gtk_idx;
        return;
    }
    violation("an EAPOL frame from the station after the handshake");
}

/* A data frame from the network to the station, encrypted as the access
   point would - here the card's "decryption" is only whether it holds
   the key: the payload is in the clear, the CCMP header has the packet
   number, the MIC is eight bytes of room */
void ap_send_data(const uint8_t da[6], const uint8_t sa[6], uint16_t type,
                  const uint8_t *payload, int n, uint64_t pn)
{
    static uint8_t f[2400];
    int group = da[0] & 1;
    memset(f, 0, 24);
    f[0] = 0x08;
    f[1] = 0x42;                            /* FromDS, Protected */
    memcpy(f + 4, da, 6);
    memcpy(f + 10, ap_bssid, 6);
    memcpy(f + 16, sa, 6);
    uint8_t *c = f + 24;
    c[0] = (uint8_t)pn;
    c[1] = (uint8_t)(pn >> 8);
    c[2] = 0;
    c[3] = (uint8_t)(0x20 | ((group ? aps.gtk_idx : 0) << 6));
    for (int i = 0; i < 4; i++)
        c[4 + i] = (uint8_t)(pn >> (16 + 8 * i));
    static const uint8_t snap[6] = { 0xAA, 0xAA, 3, 0, 0, 0 };
    memcpy(f + 32, snap, 6);
    f[38] = (uint8_t)(type >> 8);
    f[39] = (uint8_t)type;
    memcpy(f + 40, payload, (size_t)n);
    memset(f + 40 + n, 0, 8);               /* the MIC */
    fw_air_rx(f, (uint32_t)(40 + n + 8), 1);
}
