/* aesref - what OpenSSL says the ciphertext should be
 *
 * The AES test needs a second opinion, and it has to come from code
 * that shares nothing with ours. OpenSSL is on the machine, but its
 * command line tool cannot do GCM or CCM - the authenticated modes are
 * reachable only through the library. Hence these forty lines: the
 * test compiles them once (cc -lcrypto), hands over the key, the
 * nonce, the associated data and the message in hex, and gets back the
 * ciphertext and the tag.
 *
 * When the compiler or the library is missing, the test says so and
 * checks what it can against the published vectors instead.
 *
 *   aesref gcm|ccm|ctr|ecb <key> <nonce> <aad> <plain> <taglen>
 */

#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int unhex(const char *s, unsigned char *out, int max)
{
    int n = 0;

    while (s[2 * n] && n < max) {
        unsigned v;
        if (sscanf(s + 2 * n, "%2x", &v) != 1)
            break;
        out[n++] = (unsigned char)v;
    }
    return n;
}

int main(int argc, char **argv)
{
    unsigned char key[64], nonce[64], aad[1024], in[8192], out[8192], tag[16];
    const EVP_CIPHER *cipher;
    EVP_CIPHER_CTX *ctx;
    int keylen, noncelen, aadlen, inlen, taglen, len, total = 0;
    int aead;

    if (argc < 7) {
        fprintf(stderr, "usage: aesref gcm|ccm|ctr|ecb key nonce aad "
                        "plain taglen\n");
        return 2;
    }

    keylen = unhex(argv[2], key, sizeof(key));
    noncelen = unhex(argv[3], nonce, sizeof(nonce));
    aadlen = unhex(argv[4], aad, sizeof(aad));
    inlen = unhex(argv[5], in, sizeof(in));
    taglen = atoi(argv[6]);
    aead = strcmp(argv[1], "gcm") == 0 || strcmp(argv[1], "ccm") == 0;

    if (!strcmp(argv[1], "gcm"))
        cipher = keylen == 16 ? EVP_aes_128_gcm() :
                 keylen == 24 ? EVP_aes_192_gcm() : EVP_aes_256_gcm();
    else if (!strcmp(argv[1], "ccm"))
        cipher = keylen == 16 ? EVP_aes_128_ccm() :
                 keylen == 24 ? EVP_aes_192_ccm() : EVP_aes_256_ccm();
    else if (!strcmp(argv[1], "ctr"))
        cipher = keylen == 16 ? EVP_aes_128_ctr() :
                 keylen == 24 ? EVP_aes_192_ctr() : EVP_aes_256_ctr();
    else
        cipher = keylen == 16 ? EVP_aes_128_ecb() :
                 keylen == 24 ? EVP_aes_192_ecb() : EVP_aes_256_ecb();

    ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        return 1;

    if (!EVP_EncryptInit_ex(ctx, cipher, NULL, NULL, NULL))
        return 1;
    if (aead) {
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, noncelen, NULL);
        /* CCM has to be told the length of the tag before the key, and
           the length of the message before the message - it writes both
           into its first block. */
        if (!strcmp(argv[1], "ccm"))
            EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, taglen, NULL);
    }
    if (!EVP_EncryptInit_ex(ctx, NULL, NULL, key, noncelen ? nonce : NULL))
        return 1;
    EVP_CIPHER_CTX_set_padding(ctx, 0);

    if (!strcmp(argv[1], "ccm"))
        EVP_EncryptUpdate(ctx, NULL, &len, NULL, inlen);
    if (aead && aadlen)
        EVP_EncryptUpdate(ctx, NULL, &len, aad, aadlen);

    if (!EVP_EncryptUpdate(ctx, out, &len, in, inlen))
        return 1;
    total = len;
    if (!EVP_EncryptFinal_ex(ctx, out + total, &len))
        return 1;
    total += len;

    if (aead)
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, taglen, tag);
    EVP_CIPHER_CTX_free(ctx);

    for (int i = 0; i < total; i++)
        printf("%02x", out[i]);
    printf(" ");
    if (aead)
        for (int i = 0; i < taglen; i++)
            printf("%02x", tag[i]);
    printf("\n");
    return 0;
}
