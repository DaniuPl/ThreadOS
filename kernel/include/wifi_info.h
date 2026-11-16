#ifndef THREADOS_WIFI_INFO_H
#define THREADOS_WIFI_INFO_H

#include <stdint.h>

/* What a program is told about Wi-Fi (SYS64_WIFI). The same layout is
   in user/uwifi.h, which is what programs include: a program cannot
   reach the kernel's headers, so the two have to be kept alike. */
#define WIFI_INFO_NETS 16

#define WIFI_SEC_OPEN 0
#define WIFI_SEC_WEP  1
#define WIFI_SEC_WPA  2
#define WIFI_SEC_WPA2 3

struct wifi_net {
    char ssid[33];              /* "" when the network hides its name */
    char bssid[18];
    int8_t rssi;                /* dBm */
    uint8_t channel;
    uint8_t security;           /* WIFI_SEC_* */
    uint8_t reserved;
};

struct wifi_info {
    int32_t present;            /* the driver has a card */
    int32_t ready;              /* ...up, and able to scan */
    int32_t scanning;           /* a scan is running right now */
    int32_t found;              /* networks in the last scan; -1 = none yet */
    int32_t recorded;           /* the list is 'wifi test's recorded frames */
    char card[40];
    char state[32];
    char mac[18];
    char error[96];
    struct wifi_net net[WIFI_INFO_NETS];   /* the loudest first */
};

#endif
