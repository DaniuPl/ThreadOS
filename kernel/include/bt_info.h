#ifndef THREADOS_BT_INFO_H
#define THREADOS_BT_INFO_H

#include <stdint.h>

/* What a program is told about Bluetooth (SYS64_BT). The same layout
   is in user/ubt.h, which is what programs include: a program cannot
   reach the kernel's headers, so the two have to be kept alike. */
#define BT_INFO_DEVICES 8

struct bt_info {
    int32_t present;
    int32_t scanning;           /* a search is running right now */
    int32_t found;              /* devices heard in the last search */
    char why[96];               /* when there is no adapter */
    char address[18];
    char name[32];
    char version[40];
    char dev_addr[BT_INFO_DEVICES][18];
    char dev_name[BT_INFO_DEVICES][32];
    int32_t dev_rssi[BT_INFO_DEVICES];
};

#endif
