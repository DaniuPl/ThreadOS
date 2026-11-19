/* The access point the model's card joins (tests/hw/iwl/ap.c) */
#ifndef IWL_AP_H
#define IWL_AP_H

#include <stdint.h>

extern const uint8_t ap_bssid[6];
extern const char ap_ssid[];
extern const char ap_password[];

/* What happened, for the checks */
struct ap_state {
    uint8_t sta[6];
    int auth, assoc;                /* requests answered */
    int msg1, msg2_ok, msg3, msg4_ok, msg2_refused;
    int eapol_protected;            /* handshake frames sent encrypted */
    uint8_t tk[16], gtk[16];        /* the keys both sides should hold */
    int gtk_idx;
    int data_frames;                /* other data frames from the station */
    uint8_t last_data[2400];
    uint32_t last_len;
};
extern struct ap_state aps;

void ap_reset(const uint8_t sta[6]);
/* A frame the station sent, as on the air (header and body; for one the
   card encrypts, the plaintext with its CCMP header) */
void ap_frame(const uint8_t *f, uint32_t n);
void ap_eapol(const uint8_t *e, int n);

/* A data frame from the network to the station (protected) */
void ap_send_data(const uint8_t da[6], const uint8_t sa[6], uint16_t type,
                  const uint8_t *payload, int n, uint64_t pn);

/* fw.c: a frame the card hears from the access point; decrypted = the
   firmware took its CCMP off with the key the driver installed */
void fw_air_rx(const uint8_t *f, uint32_t n, int decrypted);

#endif
