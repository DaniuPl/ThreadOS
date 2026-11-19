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
    int32_t link;               /* 0 not joined, 1 joining, 2 keys, 3 joined */
    char joined[33];            /* the network joined or being joined */
    char join_error[96];        /* why the last join did not happen */
};

#define WIFI_INFO 0
#define WIFI_SEARCH 1
#define WIFI_JOIN 2             /* starts joining; WIFI_INFO says how it went */
#define WIFI_LEAVE 3

#define WIFI_LINK_DOWN 0
#define WIFI_LINK_JOINING 1
#define WIFI_LINK_KEYS 2
#define WIFI_LINK_UP 3

struct wifi_join {
    char ssid[33];
    char password[64];
};

static inline int sys_wifi(int op, struct wifi_info *out)
{
    return (int)syscall64(SYS64_WIFI, op, (int64_t)(uintptr_t)out);
}

/* 0 = joining (WIFI_INFO's link and join_error say how it goes), -1 =
   no card ready to */
static inline int sys_wifi_join(const struct wifi_join *j)
{
    return (int)syscall64(SYS64_WIFI, WIFI_JOIN, (int64_t)(uintptr_t)j);
}

#endif
