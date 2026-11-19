/* kernel/wpa.c against corrupted EAPOL frames
 *
 * Whatever is in the file goes in three ways, as the air could bring
 * it: as a key frame of its own, before and after a message 1; as the
 * key data of a message 3 that is wrapped and signed with the keys that
 * message 1 gave - so that the element parser behind the MIC and the
 * unwrap sees it too, as it would from an access point that knows the
 * password; and as the key data of a group key frame after a finished
 * handshake. The first byte picks the message 3's key information bits.
 */

#include <stdio.h>
#include <string.h>

#include "kernel/include/wpa.h"
#define THREADOS_UAPI_H
#include "user/uwpa.h"
#undef THREADOS_UAPI_H

static const uint8_t AA[6] = { 2, 0, 0, 0, 0, 1 }, SPA[6] = { 2, 0, 0, 0, 0, 2 };

static int key_frame(const uint8_t *ptk, unsigned info, uint64_t replay,
                     const uint8_t *nonce, const uint8_t *data, int n,
                     uint8_t *out)
{
    uint8_t padded[600], wrapped[608];
    if (n > 512)
        n = 512;
    memcpy(padded, data, (size_t)n);
    while (n % 8 || n < 16)
        padded[n++] = 0;
    aes_wrap(ptk + 16, 128, padded, (uint32_t)n / 8, wrapped);
    int dl = n + 8, len = 99 + dl;
    memset(out, 0, (size_t)len);
    out[0] = 2; out[1] = 3;
    out[2] = (uint8_t)((len - 4) >> 8); out[3] = (uint8_t)(len - 4);
    out[4] = 2;
    out[5] = (uint8_t)(info >> 8); out[6] = (uint8_t)info;
    out[8] = 16;
    for (int i = 0; i < 8; i++)
        out[9 + i] = (uint8_t)(replay >> (56 - 8 * i));
    if (nonce)
        memcpy(out + 17, nonce, 32);
    out[97] = (uint8_t)(dl >> 8); out[98] = (uint8_t)dl;
    memcpy(out + 99, wrapped, (size_t)dl);
    uint8_t h[20];
    hmac_sha1(ptk, 16, out, (uint32_t)len, h);
    memcpy(out + 81, h, 16);
    return len;
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f)
        return 2;
    static uint8_t in[4096];
    int n = (int)fread(in, 1, sizeof(in), f);
    fclose(f);

    struct wpa_sm sm;
    uint8_t pmk[32], snonce[32], anonce[32], out[WPA_FRAME_MAX];
    uint32_t out_len;
    memset(pmk, 0x42, 32);
    memset(snonce, 0x17, 32);
    memset(anonce, 0x99, 32);
    wpa_begin(&sm, pmk, AA, SPA, NULL, 0, snonce);
    wpa_rx(&sm, in, (uint32_t)n, out, sizeof(out), &out_len);

    /* a message 1, then the input again */
    uint8_t m1[99];
    memset(m1, 0, sizeof(m1));
    m1[0] = 2; m1[1] = 3; m1[3] = 95; m1[4] = 2;
    m1[5] = 0x00; m1[6] = 0x8A; m1[8] = 16; m1[16] = 1;
    memcpy(m1 + 17, anonce, 32);
    if (wpa_rx(&sm, m1, sizeof(m1), out, sizeof(out), &out_len) != WPA_REPLY)
        return 3;                       /* a valid message 1 must answer */
    wpa_rx(&sm, in, (uint32_t)n, out, sizeof(out), &out_len);

    /* the input as message 3's key data, wrapped and signed */
    if (n < 1)
        return 0;
    static const unsigned infos[4] = { 0x13CA, 0x13CA, 0x138A, 0x03CA };
    static uint8_t m3[700];
    int len = key_frame(sm.tptk, infos[in[0] & 3], 2, anonce, in + 1, n - 1,
                        m3);
    int r = wpa_rx(&sm, m3, (uint32_t)len, out, sizeof(out), &out_len);

    /* and, the handshake done, as a group key's */
    if (r == WPA_KEYS) {
        len = key_frame(sm.ptk, 0x1382, 3, NULL, in + 1, n - 1, m3);
        wpa_rx(&sm, m3, (uint32_t)len, out, sizeof(out), &out_len);
    }
    wpa_end(&sm);
    return 0;
}
