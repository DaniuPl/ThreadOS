#ifndef THREADOS_IWLWIFI_H
#define THREADOS_IWLWIFI_H

#include <stdint.h>

/* Intel Wireless 3160 (kernel/iwlwifi.c).
 *
 * Stage 1: the card found, its firmware loaded and calibrated, the
 * runtime firmware up and idle; the MAC address and the channel list
 * read from the card's NVM. Stage 2: scanning - the networks around,
 * with their names, channels, signal and protection. Joining and data
 * frames come later, on top of the interface in the second half of
 * this file. */

/* Finds the card. Without one it says so in one line and returns; with
   one it starts the driver's thread, which brings the card up (the
   firmware takes a second or two) and then serves it. Never blocks the
   caller on the card. */
void iwlwifi_start(void);

/* The thread's work, callable directly (a test that stands in for the
   card calls these instead of iwlwifi_start):
   iwlwifi_run   - probe and the whole bring-up; IWL_OK when the runtime
                   firmware is up, or the error that stopped it;
   iwlwifi_poll  - one pass of serving the running card: interrupt
                   causes, received packets, RF kill, watchdogs. */
int iwlwifi_run(void);
void iwlwifi_poll(void);

/* How far the driver got */
enum iwlwifi_stage {
    IWL_STAGE_NONE = 0,     /* not started */
    IWL_STAGE_NO_CARD,      /* there is no 3160 in this machine */
    IWL_STAGE_PROBED,       /* card found, registers reachable */
    IWL_STAGE_FW_LOADED,    /* firmware file read and checked */
    IWL_STAGE_INIT_ALIVE,   /* the INIT image runs */
    IWL_STAGE_NVM_READ,     /* MAC address and channels known */
    IWL_STAGE_CALIBRATED,   /* INIT finished its calibrations */
    IWL_STAGE_RT_ALIVE,     /* the runtime image runs */
    IWL_STAGE_READY,        /* runtime image configured and idle */
    IWL_STAGE_RFKILL,       /* stopped: the radio switch is off */
    IWL_STAGE_FAILED,       /* stopped by an error (see last_error) */
};

#define IWLWIFI_MAX_CHANNELS 39

/* A channel the card may use, as its NVM describes it */
struct iwlwifi_channel {
    uint16_t number;        /* 1..14, 36..165 */
    uint16_t freq;          /* MHz */
    uint8_t band;           /* IWLWIFI_BAND_* */
    uint8_t max_power;      /* dBm */
    uint16_t nvm_flags;     /* as the NVM has them (VALID, IBSS, ...) */
    uint16_t flags;         /* IWLWIFI_CHAN_*, derived from them */
};

#define IWLWIFI_BAND_24          0
#define IWLWIFI_BAND_5           1

#define IWLWIFI_CHAN_NO_IR       0x0001  /* passive: listen, never start */
#define IWLWIFI_CHAN_RADAR       0x0002  /* DFS */
#define IWLWIFI_CHAN_INDOOR      0x0004
#define IWLWIFI_CHAN_HT40_PLUS   0x0008  /* a 40 MHz pair upwards allowed */
#define IWLWIFI_CHAN_HT40_MINUS  0x0010  /* ... downwards */
#define IWLWIFI_CHAN_NO_80MHZ    0x0020
#define IWLWIFI_CHAN_NO_160MHZ   0x0040
#define IWLWIFI_CHAN_IR_CONCURRENT 0x0080

/* The driver's state, for the 'wifi' command and Settings */
struct iwlwifi_state {
    int stage;                  /* enum iwlwifi_stage */
    int bus, slot, func;
    uint16_t device;            /* PCI device id (0x08B3, 0x08B4) */
    uint32_t hw_rev;            /* CSR_HW_REV */
    char fw_version[32];        /* "17.bfb58538.0" */
    uint8_t mac[6];
    int mac_valid;
    uint16_t nvm_version, sku, radio_cfg;
    int n_channels, n_24ghz, n_5ghz;
    int calib_results;          /* PHY DB sections from calibration */
    int rfkill;                 /* the hardware switch is off */
    uint32_t fw_error_id;       /* from the firmware's error log, 0 = none */
    char last_error[192];
};

const struct iwlwifi_state *iwlwifi_state(void);
const char *iwlwifi_stage_name(int stage);
/* Copies the channel list; how many there are */
int iwlwifi_channels(struct iwlwifi_channel *out, int max);

/* --- For the MAC layer of the later stages ---------------------------
 *
 * Everything below runs on the driver's thread: the firmware is
 * served by polling, and the code that waits for an answer is the code
 * that polls for it. */

#define IWL_OK            0
#define IWL_ERR_IO       -1    /* the card refused register access */
#define IWL_ERR_TIMEOUT  -2
#define IWL_ERR_FW       -3    /* the firmware crashed */
#define IWL_ERR_RFKILL   -4
#define IWL_ERR_NOMEM    -5
#define IWL_ERR_NOCARD   -6
#define IWL_ERR_BADFW    -7    /* the firmware file is not usable */
#define IWL_ERR_BUSY     -8    /* queue full, or one sync command at a time */
#define IWL_ERR_GONE     -9    /* the card fell off the bus */
#define IWL_ERR_STATE   -10    /* no firmware running to take it */
#define IWL_ERR_REJECTED -11   /* the firmware said no (a status) */

/* Host command flags */
#define IWL_CMD_ASYNC          0x01   /* do not wait for the answer */
#define IWL_CMD_WANT_RESP      0x02   /* copy the answer's payload out */
#define IWL_CMD_SEND_IN_RFKILL 0x04   /* allowed with the radio switch off */

/* The id: opcode in bits 7:0, group in 15:8, version in 23:16 (a
   command of group 0 goes with the short header, any other with the
   wide one) */
#define IWL_CMD_ID(op, grp, ver) \
    ((uint32_t)(op) | ((uint32_t)(grp) << 8) | ((uint32_t)(ver) << 16))

struct iwl_hcmd {
    uint32_t id;
    const void *data[2];        /* the payload, in up to two pieces */
    uint32_t len[2];
    uint32_t flags;
    void *resp;                 /* WANT_RESP: the answer's payload ... */
    uint32_t resp_cap;          /* ... up to this many bytes */
    uint32_t resp_len;          /* out: the answer's payload length */
};

/* Sends a host command; sync ones wait (2 s) for the answer. A payload
   too large for the command queue's own buffer goes in a DMA buffer of
   its own, as further transfer buffers of the same descriptor. */
int iwl_send_cmd(struct iwl_hcmd *cmd);
/* The common case: one piece, sync, the answer ignored */
int iwl_send_cmd_pdu(uint32_t id, const void *data, uint32_t len,
                     uint32_t flags);

/* Notifications (unsolicited packets from the firmware) that the driver
   does not handle itself go to a handler registered for their id */
typedef void (*iwl_notif_fn)(uint32_t id, const uint8_t *payload,
                             uint32_t len, uint16_t seq);
int iwl_notif_register(uint32_t id, iwl_notif_fn fn);

/* A data queue: the transport side opened, the firmware told which
   FIFO feeds it (SCD_QUEUE_CFG); and closed again */
int iwl_txq_enable(int queue, int fifo, int sta_id, int tid);
int iwl_txq_disable(int queue);

/* --- Stage 2: scanning ---------------------------------------------- */

#define IWLWIFI_MAX_BSS 32

#define IWLWIFI_SEC_OPEN 0
#define IWLWIFI_SEC_WEP  1
#define IWLWIFI_SEC_WPA  2
#define IWLWIFI_SEC_WPA2 3

/* A network heard in the last scan */
struct iwlwifi_bss {
    uint8_t bssid[6];
    char ssid[33];          /* "" when the network hides it */
    uint8_t ssid_len;
    uint8_t channel;
    int8_t rssi;            /* dBm, the loudest it was heard */
    uint8_t security;       /* IWLWIFI_SEC_* */
};

/* A scan of every channel the card may use: how many networks were
   heard, or IWL_ERR_*. iwlwifi_scan asks the driver's thread and waits
   for it (from any thread: the shell, Settings); iwlwifi_scan_now does
   it on the calling thread, which has to be the driver's - or a test
   standing in for it. */
int iwlwifi_scan(void);
int iwlwifi_scan_now(void);
/* The networks of the last scan; how many there are */
int iwlwifi_bss(struct iwlwifi_bss *out, int max);

/* One 802.11 frame onto a data queue. tx_cmd is the 56-byte TX command
   (the caller fills rate, station, flags, security; the transport fills
   the DMA pointers); hdr is the MAC header, body what follows it. Done
   frames are reclaimed when the firmware's TX response comes in. */
int iwl_tx_frame(int queue, const uint8_t *tx_cmd, const void *hdr,
                 uint32_t hdr_len, const void *body, uint32_t body_len);

#endif
