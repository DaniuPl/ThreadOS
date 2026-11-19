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
    int32_t dev_paired[BT_INFO_DEVICES];  /* a link key is kept for it */
    int32_t pair_state;         /* BT_PAIR_*: 0 none, 1 connecting, 2 the
                                   number to confirm, 3 bonding, 4 paired,
                                   5 failed */
    uint32_t pair_number;       /* the six digits both devices show */
    char pair_name[32];
    char pair_why[96];
};

#define BT_INFO 0
#define BT_SEARCH 1
#define BT_PAIR 2               /* the device's address ("aa:bb:..") */
#define BT_ANSWER 3             /* 1 the numbers match, 0 they do not */
#define BT_FORGET 4             /* the device's address */

#define BT_PAIR_IDLE       0
#define BT_PAIR_CONNECTING 1
#define BT_PAIR_CONFIRM    2
#define BT_PAIR_BONDING    3
#define BT_PAIR_PAIRED     4
#define BT_PAIR_FAILED     5

static inline int sys_bt(int op, struct bt_info *out)
{
    return (int)syscall64(SYS64_BT, op, (int64_t)(uintptr_t)out);
}

/* BT_PAIR and BT_FORGET take an address, BT_ANSWER a yes or no */
static inline int sys_bt_arg(int op, int64_t arg)
{
    return (int)syscall64(SYS64_BT, op, arg);
}

#endif
