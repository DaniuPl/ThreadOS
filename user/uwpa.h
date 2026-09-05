/* WPA2: the keys and the encryption of a frame (IEEE 802.11i)
 *
 * Everything a station needs to talk to a protected network, except
 * the radio. Two halves:
 *
 *   the keys   - the password becomes a PMK through PBKDF2, and the
 *                four-way handshake turns the PMK plus two nonces plus
 *                two addresses into a PTK: KCK for the signature of the
 *                handshake messages, KEK for the group key travelling
 *                inside them, TK for the data;
 *   the frames - CCMP, that is AES-CCM with the nonce and the
 *                associated data assembled out of the 802.11 header.
 *
 * The associated data is the interesting part: it covers the header,
 * but with the fields that legitimately change on the way MASKED OUT -
 * retry, power management, more-data, the sequence number. What is
 * covered are the addresses. That is exactly the point of it: an
 * attacker may not redirect a frame, and a repeater may retransmit
 * one.
 *
 * There is no radio in this system yet. This header can be tested
 * without one - against the vectors from P802.11i - and it will be
 * ready when there is something to send.
 */

#ifndef PROTONOS_UWPA_H
#define PROTONOS_UWPA_H

#include "uapi.h"
#include "uaes.h"
#include "usha1.h"

/* --- The key hierarchy ------------------------------------------- */

/* PRF from 802.11i 8.5.1.1: HMAC-SHA1 over the label, a zero byte, the
   data and a counter, as many times as the wanted length needs. */
static inline void wpa_prf(const uint8_t *key, uint32_t keylen,
                           const char *label, const uint8_t *data,
                           uint32_t datalen, uint8_t *out, uint32_t outlen)
{
    uint8_t block[128];
    uint8_t hash[SHA1_SIZE];
    uint32_t labellen = 0;
    uint8_t counter = 0;

    while (label[labellen])
        labellen++;

    while (outlen) {
        uint32_t n = outlen < SHA1_SIZE ? outlen : SHA1_SIZE;
        uint32_t k = 0;

        for (uint32_t i = 0; i < labellen; i++)
            block[k++] = (uint8_t)label[i];
        block[k++] = 0;
        for (uint32_t i = 0; i < datalen; i++)
            block[k++] = data[i];
        block[k++] = counter;

        hmac_sha1(key, keylen, block, k, hash);
        for (uint32_t i = 0; i < n; i++)
            out[i] = hash[i];

        out += n;
        outlen -= n;
        counter++;
    }
}

/* The password of a network becomes a key: PBKDF2-HMAC-SHA1, four
   thousand and ninety-six rounds, and the SSID as the salt. The salt
   being the network's name is why two networks of the same name and
   the same password share a key - and why the computation cannot be
   done once for all networks. */
static inline void wpa_pmk(const char *passphrase, const char *ssid,
                           uint8_t pmk[32])
{
    uint32_t plen = 0, slen = 0;

    while (passphrase[plen])
        plen++;
    while (ssid[slen])
        slen++;

    pbkdf2_sha1((const uint8_t *)passphrase, plen,
                (const uint8_t *)ssid, slen, 4096, pmk, 32);
}

static inline int wpa_smaller(const uint8_t *a, const uint8_t *b, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        if (a[i] != b[i])
            return a[i] < b[i];
    }
    return 1;
}

/* PTK = PRF-384(PMK, "Pairwise key expansion",
                 min(AA,SPA) || max(AA,SPA) || min(nonces) || max(nonces))
 *
 * The smaller address first, the smaller nonce first - so that both
 * sides compute the same thing without agreeing who is who. The first
 * sixteen bytes are the KCK, the next sixteen the KEK, the last
 * sixteen the TK (CCMP; TKIP would want 512 bits and we do not do
 * TKIP). */
static inline void wpa_ptk(const uint8_t pmk[32], const uint8_t aa[6],
                           const uint8_t spa[6], const uint8_t anonce[32],
                           const uint8_t snonce[32], uint8_t ptk[48])
{
    uint8_t data[76];
    uint32_t k = 0;
    int addr_first = wpa_smaller(aa, spa, 6);
    int nonce_first = wpa_smaller(anonce, snonce, 32);

    for (int i = 0; i < 6; i++)
        data[k++] = addr_first ? aa[i] : spa[i];
    for (int i = 0; i < 6; i++)
        data[k++] = addr_first ? spa[i] : aa[i];
    for (int i = 0; i < 32; i++)
        data[k++] = nonce_first ? anonce[i] : snonce[i];
    for (int i = 0; i < 32; i++)
        data[k++] = nonce_first ? snonce[i] : anonce[i];

    wpa_prf(pmk, 32, "Pairwise key expansion", data, k, ptk, 48);
}

/* The signature of an EAPOL-Key message, key descriptor version 2:
   HMAC-SHA1 over the whole frame with the MIC field zeroed, cut to
   sixteen bytes. */
static inline void wpa_eapol_mic(const uint8_t kck[16], const uint8_t *frame,
                                 uint32_t len, uint8_t mic[16])
{
    uint8_t full[SHA1_SIZE];

    hmac_sha1(kck, 16, frame, len, full);
    for (int i = 0; i < 16; i++)
        mic[i] = full[i];
}

/* --- CCMP (802.11-2012 11.4.3) ------------------------------------ */

#define CCMP_HDR  8        /* the CCMP header in front of the data */
#define CCMP_MIC  8        /* and the signature behind it */

static inline int wpa_is_qos(const uint8_t *mpdu)
{
    /* Type = data (2) and the QoS bit of the subtype */
    return (mpdu[0] & 0x0C) == 0x08 && (mpdu[0] & 0x80) != 0;
}

static inline uint32_t wpa_hdrlen(const uint8_t *mpdu)
{
    uint32_t len = 24;

    /* Both directions set: the frame carries a fourth address */
    if ((mpdu[1] & 0x03) == 0x03)
        len += 6;
    if (wpa_is_qos(mpdu))
        len += 2;
    return len;
}

/* The associated data: the header with the changeable fields masked.
   Returns its length. */
static inline uint32_t ccmp_aad(const uint8_t *mpdu, uint32_t hdrlen,
                                uint8_t *aad)
{
    uint32_t k = 0;

    /* Frame control: the subtype bits go, retry, power management and
       more-data go, the protected bit is forced on. What is left is
       what the sender meant, not how the air treated it. */
    aad[k++] = (uint8_t)(mpdu[0] & ~0x70);
    aad[k++] = (uint8_t)((mpdu[1] & ~0x38) | 0x40);

    for (int i = 4; i < 22; i++)          /* A1, A2, A3 */
        aad[k++] = mpdu[i];

    /* Sequence control: the fragment number stays, the sequence number
       goes - a retransmission is the same frame. */
    aad[k++] = (uint8_t)(mpdu[22] & 0x0F);
    aad[k++] = 0;

    if (hdrlen >= 30) {                   /* the fourth address */
        for (int i = 24; i < 30; i++)
            aad[k++] = mpdu[i];
    }
    if (wpa_is_qos(mpdu)) {
        uint32_t qc = hdrlen - 2;
        aad[k++] = (uint8_t)(mpdu[qc] & 0x0F);   /* the priority stays */
        aad[k++] = 0;
    }
    return k;
}

/* The nonce: priority, the sender's address, the packet number.
   Thirteen bytes, so L=2 - which is why CCM was written the way it
   was. */
static inline void ccmp_nonce(const uint8_t *mpdu, uint32_t hdrlen,
                              const uint8_t pn[6], uint8_t nonce[13])
{
    nonce[0] = 0;
    if (wpa_is_qos(mpdu))
        nonce[0] = (uint8_t)(mpdu[hdrlen - 2] & 0x0F);
    for (int i = 0; i < 6; i++)
        nonce[1 + i] = mpdu[10 + i];      /* A2 */
    for (int i = 0; i < 6; i++)
        nonce[7 + i] = pn[5 - i];         /* the number, most significant
                                             first */
}

/* Encrypts a frame: header, then the CCMP header, then the ciphertext
   and the signature. Gives back the whole length. */
static inline uint32_t ccmp_encrypt(const uint8_t tk[16], uint8_t keyid,
                                    const uint8_t pn[6], const uint8_t *mpdu,
                                    uint32_t mpdulen, uint8_t *out)
{
    uint8_t aad[32], nonce[13], mic[8];
    uint32_t hdrlen = wpa_hdrlen(mpdu);
    uint32_t aadlen;
    uint32_t datalen;

    if (mpdulen <= hdrlen)
        return 0;
    datalen = mpdulen - hdrlen;

    for (uint32_t i = 0; i < hdrlen; i++)
        out[i] = mpdu[i];
    /* The protected bit has to be set in what goes on the air - the
       receiver decides by it whether to decrypt at all. */
    out[1] |= 0x40;

    /* The CCMP header carries the packet number in an order nobody
       would invent twice: the two lowest bytes, a reserved zero, the
       key index with the ExtIV bit, and then the four higher bytes. */
    out[hdrlen + 0] = pn[0];
    out[hdrlen + 1] = pn[1];
    out[hdrlen + 2] = 0;
    out[hdrlen + 3] = (uint8_t)(0x20 | (keyid << 6));
    out[hdrlen + 4] = pn[2];
    out[hdrlen + 5] = pn[3];
    out[hdrlen + 6] = pn[4];
    out[hdrlen + 7] = pn[5];

    aadlen = ccmp_aad(mpdu, hdrlen, aad);
    ccmp_nonce(mpdu, hdrlen, pn, nonce);

    if (!aes_ccm_encrypt(tk, 128, nonce, 13, aad, aadlen, mpdu + hdrlen,
                         out + hdrlen + CCMP_HDR, datalen, mic, CCMP_MIC))
        return 0;
    for (int i = 0; i < CCMP_MIC; i++)
        out[hdrlen + CCMP_HDR + datalen + i] = mic[i];

    return hdrlen + CCMP_HDR + datalen + CCMP_MIC;
}

/* Decrypts one. Gives back the length of the frame with the plain data
   in it, or 0 - and 0 means the frame is to be dropped without a word:
   a failed signature is either damage or an attack, and there is
   nothing to answer either. 'pn' takes the packet number, so that the
   caller can compare it with the last one seen. */
static inline uint32_t ccmp_decrypt(const uint8_t tk[16], const uint8_t *mpdu,
                                    uint32_t mpdulen, uint8_t *out,
                                    uint8_t pn[6])
{
    uint8_t aad[32], nonce[13];
    uint32_t hdrlen = wpa_hdrlen(mpdu);
    uint32_t aadlen, datalen;

    if (mpdulen < hdrlen + CCMP_HDR + CCMP_MIC)
        return 0;
    if (!(mpdu[hdrlen + 3] & 0x20))       /* no ExtIV: not CCMP */
        return 0;

    datalen = mpdulen - hdrlen - CCMP_HDR - CCMP_MIC;

    pn[0] = mpdu[hdrlen + 0];
    pn[1] = mpdu[hdrlen + 1];
    pn[2] = mpdu[hdrlen + 4];
    pn[3] = mpdu[hdrlen + 5];
    pn[4] = mpdu[hdrlen + 6];
    pn[5] = mpdu[hdrlen + 7];

    for (uint32_t i = 0; i < hdrlen; i++)
        out[i] = mpdu[i];

    aadlen = ccmp_aad(mpdu, hdrlen, aad);
    ccmp_nonce(mpdu, hdrlen, pn, nonce);

    if (!aes_ccm_decrypt(tk, 128, nonce, 13, aad, aadlen,
                         mpdu + hdrlen + CCMP_HDR, out + hdrlen, datalen,
                         mpdu + hdrlen + CCMP_HDR + datalen, CCMP_MIC))
        return 0;

    return hdrlen + datalen;
}

#endif
