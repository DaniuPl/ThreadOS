/* Bluetooth, as a program sees it: the adapter, and what a search
   heard. The layout is kernel/include/bt_info.h's and has to stay
   alike. Only the 64-bit system answers; elsewhere sys_bt says -1. */
#ifndef THREADOS_USER_UBT_H
#define THREADOS_USER_UBT_H

#include "uapi.h"

#define BT_INFO_DEVICES 8

struct bt_info {
    int32_t present;
    int32_t scanning;
    int32_t found;
    char why[96];
    char address[18];
    char name[32];
    char version[40];
    char dev_addr[BT_INFO_DEVICES][18];
    char dev_name[BT_INFO_DEVICES][32];
    int32_t dev_rssi[BT_INFO_DEVICES];
};

#define BT_INFO 0
#define BT_SEARCH 1

static inline int sys_bt(int op, struct bt_info *out)
{
    return (int)syscall64(SYS64_BT, op, (int64_t)(uintptr_t)out);
}

#endif
