/* The key that signs ThreadOS - its releases (user/update.c) and its
 * application packages (user/tapp.c, scripts/mktapp.py)
 *
 * The public half of a P-256 key, 0x04 || X || Y. The private half is
 * only in the repository's secret THREADOS_UPDATE_KEY, which the
 * release workflow signs with; tests make keys of their own.
 *
 * update.key in the root directory, when an administrator put a key
 * there (130 hex digits), is trusted instead. By its full name: only
 * in the root is update.key a system file nobody but an administrator
 * can write, and a session starts in its own home - where a plain
 * user's update.key would otherwise be the key an administrator's
 * 'sudo run update' trusted.
 */
#ifndef USIGNKEY_H
#define USIGNKEY_H

#include "uapi.h"

static const uint8_t official_key[65] = {
    0x04, 0x40, 0x6f, 0xcb, 0x12, 0x6f, 0x92, 0xdc, 0xe9, 0x07, 0x05, 0xde,
    0x16, 0x84, 0x7e, 0xc4, 0x48, 0x34, 0x31, 0xf3, 0x1e, 0x9f, 0xd2, 0x6d,
    0x48, 0xac, 0x16, 0x85, 0x09, 0x5a, 0xdb, 0x86, 0xf1, 0x18, 0xc1, 0x24,
    0xba, 0x54, 0x0e, 0x70, 0xf2, 0xb3, 0x1f, 0x77, 0xcc, 0xdd, 0x9a, 0x02,
    0xef, 0xaa, 0x08, 0xfd, 0x1a, 0xc2, 0xf4, 0xcf, 0x4b, 0xaf, 0x38, 0xb6,
    0x88, 0x32, 0x93, 0x65, 0x0e
};

#define SIGNKEY_OFFICIAL 0        /* the one built in */
#define SIGNKEY_DISK     1        /* /update.key */
#define SIGNKEY_BAD      2        /* /update.key is there but no key */

static inline int signkey_hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* The key to check signatures against, and where it came from */
static inline const uint8_t *signing_key_from(int *from)
{
    static uint8_t key[65];
    static char text[140];
    int fd = sys_open("/update.key", O_READ);
    if (fd < 0) {
        *from = SIGNKEY_OFFICIAL;
        return official_key;
    }
    int64_t n = sys_read(fd, text, sizeof(text) - 1);
    sys_close(fd);
    int ok = n >= 130;
    for (int i = 0; ok && i < 65; i++) {
        int a = signkey_hexval(text[2 * i]), b = signkey_hexval(text[2 * i + 1]);
        ok = a >= 0 && b >= 0;
        key[i] = (uint8_t)(a * 16 + b);
    }
    if (ok && key[0] == 0x04) {
        *from = SIGNKEY_DISK;
        return key;
    }
    *from = SIGNKEY_BAD;
    return official_key;
}

#endif
