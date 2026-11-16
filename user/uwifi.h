/* Wi-Fi, as a program sees it: the card, its state, and the networks
   the last scan heard. The layout is kernel/include/wifi_info.h's and
   has to stay alike. Only the 64-bit system answers. */
#ifndef THREADOS_USER_UWIFI_H
#define THREADOS_USER_UWIFI_H

#include "uapi.h"

#define WIFI_INFO_NETS 16

#define WIFI_SEC_OPEN 0
#define WIFI_SEC_WEP  1
#define WIFI_SEC_WPA  2
#define WIFI_SEC_WPA2 3

struct wifi_net {
    char ssid[33];
    char bssid[18];
    int8_t rssi;
    uint8_t channel;
    uint8_t security;
    uint8_t reserved;
};

struct wifi_info {
    int32_t present;
    int32_t ready;
    int32_t scanning;
    int32_t found;
    int32_t recorded;
    char card[40];
    char state[32];
    char mac[18];
    char error[96];
    struct wifi_net net[WIFI_INFO_NETS];
};

#define WIFI_INFO 0
#define WIFI_SEARCH 1

static inline int sys_wifi(int op, struct wifi_info *out)
{
    return (int)syscall64(SYS64_WIFI, op, (int64_t)(uintptr_t)out);
}

#endif
