/* The access point's side of WPA2-Personal, for the tests
 *
 * Written from IEEE 802.11-2020 12.7 with OpenSSL's primitives (PBKDF2,
 * HMAC-SHA1, AES key wrap) - not from kernel/wpa.c - so that a key the
 * two agree on is a key a real access point would agree on too. The
 * WPA test (tests/hw/wpa_model.c) and the card's model (iwl_model.c)
 * use it.
 */
#ifndef WPA_AP_H
#define WPA_AP_H

#include <stdint.h>

struct ap {
    uint8_t pmk[32];
    uint8_t aa[6], spa[6];
    uint8_t anonce[32], snonce[32];
    uint8_t ptk[48];                /* KCK | KEK | TK */
    int ptk_ok;
    uint8_t gtk[16];
    int gtk_idx;
    uint8_t ie[64];                 /* the access point's RSN element */
    int ie_len;
    uint8_t sta_ie[64];             /* the station's, from its request */
    int sta_ie_len;
    uint64_t replay;
    char why[160];
};

/* The access point of a network with this password and name, its RSN
   element CCMP/CCMP/PSK; the station's address and the RSN element its
   association request carried */
void ap_init(struct ap *ap, const char *password, const char *ssid,
             const uint8_t aa[6], const uint8_t spa[6],
             const uint8_t *sta_ie, int sta_ie_len);

/* Message 1 into out; its length */
int ap_msg1(struct ap *ap, uint8_t *out);

/* Message 2 from the station: the PTK worked out from its SNonce, the
   MIC and the RSN element checked. 0, or -1 with ap->why */
int ap_rx_msg2(struct ap *ap, const uint8_t *in, int len);

/* Message 3 into out: the ANonce, the RSN element and the group key
   (index gtk_idx), wrapped and signed. Its length */
int ap_msg3(struct ap *ap, uint8_t *out);

/* Message 4: signed with the PTK, secure, no key data. 0 or -1 */
int ap_rx_msg4(struct ap *ap, const uint8_t *in, int len);

/* The group handshake: a new group key (index idx) wrapped and signed;
   and the station's answer to it */
int ap_group1(struct ap *ap, int idx, uint8_t *out);
int ap_rx_group2(struct ap *ap, const uint8_t *in, int len);

/* The key data frame's parts, for tests that damage them */
#define AP_K_INFO     5
#define AP_K_REPLAY   9
#define AP_K_NONCE    17
#define AP_K_MIC      81
#define AP_K_DATALEN  97
#define AP_K_DATA     99

/* Signs a frame again after a test changed it */
void ap_sign(const struct ap *ap, uint8_t *frame, int len);

#endif
