#ifndef THREADOS_WPA_H
#define THREADOS_WPA_H

#include <stdint.h>

/* WPA2-Personal: the supplicant's half of the four-way handshake
 * (kernel/wpa.c)
 *
 * The network's password becomes the pairwise master key (PMK); the
 * access point and we each pick a random number (ANonce, SNonce), and
 * from the PMK, the two numbers and the two addresses both sides work
 * out the same pairwise transient key (PTK) without sending it. Its
 * first part signs the handshake's frames (KCK), its second wraps the
 * group key the access point sends (KEK), its third is the key the
 * card encrypts our traffic with (TK). Only CCMP, the AES cipher every
 * WPA2 network offers, and only the pre-shared key.
 *
 * Frames go in and out as EAPOL frames, from the 802.1X header on; the
 * driver puts them into data frames and takes them out. Nothing here
 * touches the card.
 */

#define WPA_PMK_LEN     32
#define WPA_NONCE_LEN   32
#define WPA_KCK_LEN     16
#define WPA_KEK_LEN     16
#define WPA_TK_LEN      16          /* CCMP */
#define WPA_PTK_LEN     (WPA_KCK_LEN + WPA_KEK_LEN + WPA_TK_LEN)
#define WPA_GTK_MAX     32
#define WPA_IE_MAX      64
#define WPA_FRAME_MAX   256         /* the longest frame we send */

/* What wpa_rx did with a frame */
#define WPA_DROP        0           /* nothing to do: not ours, or refused */
#define WPA_REPLY       1           /* send *out (message 2) */
#define WPA_KEYS        2           /* send *out (message 4), then install
                                       the pairwise and group keys */
#define WPA_GROUP       3           /* send *out (the group handshake's
                                       message 2), then install the new
                                       group key */

/* Where the handshake is */
#define WPA_IDLE        0
#define WPA_STARTED     1           /* message 2 sent */
#define WPA_DONE        2           /* keys in place */

struct wpa_keys {
    uint8_t tk[WPA_TK_LEN];
    uint8_t gtk[WPA_GTK_MAX];
    uint32_t gtk_len;
    int gtk_idx;                    /* 1..3 */
    uint8_t gtk_rsc[8];             /* where the group's packet numbers are */
};

struct wpa_sm {
    int state;
    uint8_t pmk[WPA_PMK_LEN];
    uint8_t aa[6], spa[6];          /* the access point's and our address */
    uint8_t ie[WPA_IE_MAX];         /* our RSN element, as we asked */
    uint32_t ie_len;
    uint8_t ap_ie[WPA_IE_MAX];      /* the access point's, as it said in
                                       its beacon (0 bytes: not known) */
    uint32_t ap_ie_len;
    uint8_t anonce[WPA_NONCE_LEN], snonce[WPA_NONCE_LEN];
    uint8_t ptk[WPA_PTK_LEN];       /* KCK | KEK | TK, in use */
    int ptk_set;
    uint8_t tptk[WPA_PTK_LEN];      /* from the last message 1: in use
                                       once a message 3 signed with it
                                       holds up - message 1 is unsigned,
                                       and anybody can send one */
    int tptk_set;
    uint8_t replay[8];              /* the last counter a signed frame had */
    int replay_set;
    struct wpa_keys keys;
    char why[96];                   /* why the last frame was refused */
};

/* The PMK from a password of 8..63 characters and the network's name:
   PBKDF2-HMAC-SHA1, 4096 rounds (IEEE 802.11 J.4) */
void wpa_password_pmk(const char *password, const uint8_t *ssid,
                      uint32_t ssid_len, uint8_t pmk[WPA_PMK_LEN]);

/* Our RSN element for the association request: CCMP for both kinds of
   traffic, the pre-shared key; its length */
uint32_t wpa_rsn_ie(uint8_t *out);

/* Whether an access point's RSN element (from 0x30 on) offers what we
   speak: CCMP group and pairwise ciphers and PSK. 0 = yes, else <0 */
int wpa_rsn_usable(const uint8_t *ie, uint32_t len);

/* A new association: the PMK, the two addresses, the access point's RSN
   element as its beacon had it (or NULL), a fresh random SNonce */
void wpa_begin(struct wpa_sm *sm, const uint8_t pmk[WPA_PMK_LEN],
               const uint8_t aa[6], const uint8_t spa[6],
               const uint8_t *ap_ie, uint32_t ap_ie_len,
               const uint8_t snonce[WPA_NONCE_LEN]);

/* One EAPOL frame from the access point, from the 802.1X header on.
   WPA_REPLY / WPA_KEYS / WPA_GROUP put the frame to send into out (at
   most out_cap bytes; WPA_FRAME_MAX is always enough) and its length
   into *out_len; WPA_DROP leaves the reason in sm->why. */
int wpa_rx(struct wpa_sm *sm, const uint8_t *in, uint32_t in_len,
           uint8_t *out, uint32_t out_cap, uint32_t *out_len);

/* Forgets the keys */
void wpa_end(struct wpa_sm *sm);

/* AES key unwrap (RFC 3394) with a 128-bit key: n = in_len - 8 bytes out.
   0, or -1 if the integrity check fails */
int wpa_aes_unwrap(const uint8_t kek[16], const uint8_t *in, uint32_t in_len,
                   uint8_t *out);

#endif
