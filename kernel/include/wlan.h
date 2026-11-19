#ifndef THREADOS_WLAN_H
#define THREADOS_WLAN_H

#include <stdint.h>

/* IEEE 802.11 frames a station sends and reads to join a network and
 * pass data through it (kernel/wlan.c): the beacon read, open system
 * authentication, the association request and response, and data
 * frames to and from Ethernet. Nothing here knows a card; every length
 * from the air is checked.
 */

#define WLAN_HDR        24          /* a frame's header, three addresses */
#define WLAN_FRAME_MAX  2400        /* the most a frame of ours takes */
#define WLAN_CCMP_HDR   8           /* the CCMP header after the 802.11 one */
#define WLAN_CCMP_MIC   8           /* and the MIC at the end */

/* Frame control: type and subtype in the first byte */
#define WLAN_FC_ASSOC_REQ   0x00
#define WLAN_FC_ASSOC_RESP  0x10
#define WLAN_FC_PROBE_RESP  0x50
#define WLAN_FC_BEACON      0x80
#define WLAN_FC_DISASSOC    0xA0
#define WLAN_FC_AUTH        0xB0
#define WLAN_FC_DEAUTH      0xC0
#define WLAN_FC_DATA        0x08
#define WLAN_FC_QOS_DATA    0x88
/* ... and flags in the second */
#define WLAN_FC1_TODS       0x01
#define WLAN_FC1_FROMDS     0x02
#define WLAN_FC1_PROTECTED  0x40

#define WLAN_CAP_ESS        0x0001
#define WLAN_CAP_PRIVACY    0x0010
#define WLAN_CAP_SHORT_PRE  0x0020
#define WLAN_CAP_SHORT_SLOT 0x0400

#define WLAN_ETH_EAPOL      0x888E

/* What a beacon or a probe response says about its network */
struct wlan_bss_info {
    uint8_t bssid[6];
    uint8_t ssid[32];
    uint8_t ssid_len;
    uint16_t capability;
    uint16_t beacon_int;            /* TU */
    uint64_t tsf;                   /* the access point's clock, us */
    uint8_t channel;                /* 0: the frame did not say */
    uint8_t rates[16];              /* in 500 kb/s; bit 7: a basic rate */
    uint8_t n_rates;
    uint8_t rsn[64];                /* the whole RSN element, or n = 0 */
    uint8_t rsn_len;
    int has_tim;                    /* a beacon's TIM: */
    uint8_t dtim_count, dtim_period;
    int wmm;                        /* WMM parameters offered */
};

/* A beacon or a probe response (from the frame control on). 0, or -1
   if it is neither or does not hold together */
int wlan_parse_beacon(const uint8_t *f, uint32_t n, struct wlan_bss_info *b);

/* Open system authentication, sequence 1 - the frame and its length */
uint32_t wlan_auth_req(uint8_t *f, const uint8_t bssid[6],
                       const uint8_t sa[6]);

/* The access point's answer to it (sequence 2, to us, from it): 0 and
   the status code, or -1 if the frame is something else */
int wlan_auth_resp(const uint8_t *f, uint32_t n, const uint8_t me[6],
                   const uint8_t bssid[6], uint16_t *status);

/* The association request: the capabilities we have, the network's
   name, the rates we speak (the ones it offers that we do) and, for a
   protected network, our RSN element */
uint32_t wlan_assoc_req(uint8_t *f, const struct wlan_bss_info *b,
                        const uint8_t sa[6], const uint8_t *rsn,
                        uint32_t rsn_len);

/* The association response: 0 with its status and association id, or
   -1 if the frame is something else */
int wlan_assoc_resp(const uint8_t *f, uint32_t n, const uint8_t me[6],
                    const uint8_t bssid[6], uint16_t *status, uint16_t *aid);

/* A deauthentication or disassociation from the access point to us:
   its reason code, or -1 if the frame is not one */
int wlan_kicked(const uint8_t *f, uint32_t n, const uint8_t me[6],
                const uint8_t bssid[6]);

/* An Ethernet frame (destination, source, type, payload) as a data
   frame to the access point: the 802.11 header into hdr (WLAN_HDR
   bytes, protected if asked - the card adds the CCMP header's room
   after it), the LLC/SNAP header and payload into body. The body's
   length, or 0 if the frame is too short or too long */
uint32_t wlan_from_eth(const uint8_t *eth, uint32_t n, const uint8_t bssid[6],
                       int protect, uint8_t hdr[WLAN_HDR], uint8_t *body);

/* A data frame from the access point as an Ethernet frame into eth
   (WLAN_FRAME_MAX bytes of room): its length, or -1 if it is not a
   data frame from this network carrying LLC/SNAP. ccmp: the frame was
   protected and the card left the CCMP header and MIC in place */
int wlan_to_eth(const uint8_t *f, uint32_t n, const uint8_t bssid[6],
                int ccmp, uint8_t *eth);

#endif
