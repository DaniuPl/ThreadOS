/* kernel/wlan.c against corrupted 802.11 frames: every reader a frame
 * from the air goes through, the bytes as they came */

#include <stdio.h>
#include <string.h>

#include "kernel/include/wlan.h"

int main(int argc, char **argv)
{
    if (argc < 2)
        return 2;
    FILE *fh = fopen(argv[1], "rb");
    if (!fh)
        return 2;
    static uint8_t f[4096];
    uint32_t n = (uint32_t)fread(f, 1, sizeof(f), fh);
    fclose(fh);
    static const uint8_t me[6] = { 0x7C, 0x5C, 0xF8, 0x12, 0x34, 0x56 };
    uint8_t bssid[6];
    memcpy(bssid, n >= 16 ? f + 10 : me, 6);
    struct wlan_bss_info b;
    uint16_t status, aid;
    static uint8_t eth[WLAN_FRAME_MAX];
    if (wlan_parse_beacon(f, n, &b) == 0) {
        /* what is parsed goes into an association request */
        static uint8_t req[512];
        uint32_t len = wlan_assoc_req(req, &b, me, b.rsn, b.rsn_len);
        if (len > sizeof(req))
            return 3;
    }
    wlan_auth_resp(f, n, me, bssid, &status);
    wlan_assoc_resp(f, n, me, bssid, &status, &aid);
    wlan_kicked(f, n, me, bssid);
    int a = wlan_to_eth(f, n, bssid, 0, eth);
    int c = wlan_to_eth(f, n, bssid, 1, eth);
    if (a > WLAN_FRAME_MAX || c > WLAN_FRAME_MAX)
        return 3;
    /* and an Ethernet frame of these bytes the other way */
    static uint8_t hdr[WLAN_HDR], body[WLAN_FRAME_MAX];
    if (wlan_from_eth(f, n, bssid, 1, hdr, body) > WLAN_FRAME_MAX)
        return 3;
    return 0;
}
