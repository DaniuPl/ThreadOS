/* Random numbers for the kernel and for programs
 *
 * TLS is only as good as its keys, and the keys come from here. There
 * is no one good source on a PC, so several are pooled, as Linux does:
 *
 *   - RDSEED and RDRAND, when the processor has them (most since 2012,
 *     not QEMU's default model): straight out of the chip's generator
 *   - the time stamp counter at every interrupt: when a key, a packet,
 *     a disk block or a timer tick arrives, measured in cycles, has
 *     low bits nobody outside can predict
 *   - the clock, and the counter, at boot
 *
 * Everything goes into a BLAKE2s state (RFC 7693) - a hash: whatever is
 * added, known or not, only adds to what an attacker has to guess. To
 * hand bytes out, the pool's hash becomes a ChaCha20 key (RFC 8439);
 * the keystream is the output, and the first block of every request
 * replaces the key ("fast key erasure"), so bytes once handed out
 * cannot be recomputed from the state left behind. The key is renewed
 * from the pool now and then, as more has come in.
 *
 * Interrupts only add a word to a small array (cheap enough for every
 * one); the array is folded into the pool on the way out.
 *
 * The ARM64 kernel builds this file too: its counter is the generic
 * timer's, its chip's generator RNDR (ARMv8.5's, which QEMU's "max"
 * processor has), and it adds what a virtio entropy device gives
 * through random_add (kernelarm64/vrng.c).
 */

#include <stdint.h>
#include "include/random.h"
#include "include/string.h"
#include "include/spinlock.h"
#include "include/printk.h"
#if defined(__aarch64__)
uint32_t rtc_seconds(void);         /* kernelarm64/rtc.c: the PL031 */
#else
#include "include/rtc.h"
#endif

/* --- BLAKE2s-256 ----------------------------------------------------- */

static const uint32_t B2S_IV[8] = {
    0x6A09E667, 0xBB67AE85, 0x3C6EF372, 0xA54FF53A,
    0x510E527F, 0x9B05688C, 0x1F83D9AB, 0x5BE0CD19,
};

static const uint8_t B2S_SIGMA[10][16] = {
    { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 },
    { 14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3 },
    { 11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4 },
    { 7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8 },
    { 9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13 },
    { 2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9 },
    { 12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11 },
    { 13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10 },
    { 6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5 },
    { 10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0 },
};

struct blake2s {
    uint32_t h[8], t[2];
    uint8_t buf[64];
    uint32_t len;
};

static uint32_t ror32(uint32_t x, int n) { return x >> n | x << (32 - n); }
static uint32_t rol32(uint32_t x, int n) { return x << n | x >> (32 - n); }

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

#define B2S_G(a, b, c, d, x, y)                                    \
    do {                                                           \
        v[a] += v[b] + (x); v[d] = ror32(v[d] ^ v[a], 16);         \
        v[c] += v[d];       v[b] = ror32(v[b] ^ v[c], 12);         \
        v[a] += v[b] + (y); v[d] = ror32(v[d] ^ v[a], 8);          \
        v[c] += v[d];       v[b] = ror32(v[b] ^ v[c], 7);          \
    } while (0)

static void b2s_compress(struct blake2s *s, const uint8_t *block, int last)
{
    uint32_t m[16], v[16];
    for (int i = 0; i < 16; i++)
        m[i] = le32(block + 4 * i);
    for (int i = 0; i < 8; i++) {
        v[i] = s->h[i];
        v[i + 8] = B2S_IV[i];
    }
    v[12] ^= s->t[0];
    v[13] ^= s->t[1];
    if (last)
        v[14] = ~v[14];
    for (int r = 0; r < 10; r++) {
        const uint8_t *z = B2S_SIGMA[r];
        B2S_G(0, 4, 8, 12, m[z[0]], m[z[1]]);
        B2S_G(1, 5, 9, 13, m[z[2]], m[z[3]]);
        B2S_G(2, 6, 10, 14, m[z[4]], m[z[5]]);
        B2S_G(3, 7, 11, 15, m[z[6]], m[z[7]]);
        B2S_G(0, 5, 10, 15, m[z[8]], m[z[9]]);
        B2S_G(1, 6, 11, 12, m[z[10]], m[z[11]]);
        B2S_G(2, 7, 8, 13, m[z[12]], m[z[13]]);
        B2S_G(3, 4, 9, 14, m[z[14]], m[z[15]]);
    }
    for (int i = 0; i < 8; i++)
        s->h[i] ^= v[i] ^ v[i + 8];
}

static void b2s_init(struct blake2s *s)
{
    for (int i = 0; i < 8; i++)
        s->h[i] = B2S_IV[i];
    s->h[0] ^= 0x01010000u | 32;      /* no key, 32 bytes out */
    s->t[0] = s->t[1] = 0;
    s->len = 0;
}

static void b2s_update(struct blake2s *s, const void *data, uint32_t n)
{
    const uint8_t *p = data;
    while (n) {
        if (s->len == 64) {           /* a full block, and more is coming */
            s->t[0] += 64;
            if (s->t[0] < 64)
                s->t[1]++;
            b2s_compress(s, s->buf, 0);
            s->len = 0;
        }
        uint32_t k = 64 - s->len < n ? 64 - s->len : n;
        memcpy(s->buf + s->len, p, k);
        s->len += k;
        p += k;
        n -= k;
    }
}

static void b2s_final(struct blake2s *s, uint8_t out[32])
{
    s->t[0] += s->len;
    if (s->t[0] < s->len)
        s->t[1]++;
    memset(s->buf + s->len, 0, 64 - s->len);
    b2s_compress(s, s->buf, 1);
    for (int i = 0; i < 8; i++)
        put_le32(out + 4 * i, s->h[i]);
}

/* --- ChaCha20 -------------------------------------------------------- */

#define QR(a, b, c, d)                                             \
    do {                                                           \
        x[a] += x[b]; x[d] = rol32(x[d] ^ x[a], 16);               \
        x[c] += x[d]; x[b] = rol32(x[b] ^ x[c], 12);               \
        x[a] += x[b]; x[d] = rol32(x[d] ^ x[a], 8);                \
        x[c] += x[d]; x[b] = rol32(x[b] ^ x[c], 7);                \
    } while (0)

static void chacha20_block(const uint8_t key[32], uint32_t counter,
                           const uint8_t nonce[12], uint8_t out[64])
{
    uint32_t in[16], x[16];
    in[0] = 0x61707865; in[1] = 0x3320646e; in[2] = 0x79622d32; in[3] = 0x6b206574;
    for (int i = 0; i < 8; i++)
        in[4 + i] = le32(key + 4 * i);
    in[12] = counter;
    for (int i = 0; i < 3; i++)
        in[13 + i] = le32(nonce + 4 * i);
    for (int i = 0; i < 16; i++)
        x[i] = in[i];
    for (int r = 0; r < 10; r++) {
        QR(0, 4, 8, 12); QR(1, 5, 9, 13); QR(2, 6, 10, 14); QR(3, 7, 11, 15);
        QR(0, 5, 10, 15); QR(1, 6, 11, 12); QR(2, 7, 8, 13); QR(3, 4, 9, 14);
    }
    for (int i = 0; i < 16; i++)
        put_le32(out + 4 * i, x[i] + in[i]);
}

/* --- The pool -------------------------------------------------------- */

static struct spinlock lock = SPINLOCK_FREE;
static struct blake2s pool;
static uint8_t key[32];
static uint64_t requests;
static int seeded;
static int have_rdrand, have_rdseed;
/* What interrupts left since the last fold: cheap to add to */
static uint32_t fast[16];
static uint32_t fast_count;
static uint32_t interrupts_seen;

#if defined(__aarch64__)
/* The generic timer's counter: tens of megahertz rather than the cycle,
   but its low bits at an interrupt are as much the machine's own */
static uint64_t rdtsc(void)
{
    uint64_t v;
    __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v));
    return v;
}

/* RNDR (S3_3_C2_C4_0): a word, and the flags say whether it is one -
   Z set is "none this time" */
static int hw_random(uint64_t *v)
{
    for (int tries = 0; tries < 10 && have_rdrand; tries++) {
        uint64_t nzcv;
        __asm__ volatile("mrs %0, s3_3_c2_c4_0; mrs %1, nzcv"
                         : "=r"(*v), "=r"(nzcv));
        if (!(nzcv & (1UL << 30)))
            return 1;
    }
    return 0;
}

/* ID_AA64ISAR0_EL1.RNDR, bits 63-60 */
static void find_generators(void)
{
    uint64_t isar0;
    __asm__ volatile("mrs %0, id_aa64isar0_el1" : "=r"(isar0));
    have_rdrand = (isar0 >> 60) != 0;
    have_rdseed = 0;
}
#else
static uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (uint64_t)hi << 32 | lo;
}

static int hw_random(uint64_t *v)
{
    unsigned char ok = 0;
    for (int tries = 0; tries < 10 && !ok; tries++) {
        if (have_rdseed)
            __asm__ volatile("rdseed %0; setc %1" : "=r"(*v), "=qm"(ok));
        if (!ok && have_rdrand)
            __asm__ volatile("rdrand %0; setc %1" : "=r"(*v), "=qm"(ok));
    }
    return ok;
}

static void find_generators(void)
{
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    have_rdrand = (c >> 30) & 1;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(7), "c"(0));
    have_rdseed = (b >> 18) & 1;
}
#endif

void random_add_interrupt(int irq)
{
    /* No lock: a lost word is lost entropy, never a wrong answer -
       everything here only ever adds to the pool */
    uint32_t i = fast_count++;
    uint64_t t = rdtsc();
    fast[i & 15] = rol32(fast[i & 15], 7) ^ (uint32_t)t ^
                   (uint32_t)(t >> 32) * 0x9E3779B9u ^ (uint32_t)irq << 24;
    interrupts_seen++;
}

void random_add(const void *data, uint32_t n)
{
    irqflags_t f = spin_lock_irq(&lock);
    b2s_update(&pool, data, n);
    spin_unlock_irq(&lock, f);
}

/* The key renewed from the pool: what came in since, the chip's words,
   and the counter now - then the pool continues from its own hash */
static void reseed(void)
{
    uint64_t now = rdtsc();
    b2s_update(&pool, fast, sizeof(fast));
    b2s_update(&pool, &now, sizeof(now));
    for (int i = 0; i < 4; i++) {
        uint64_t v;
        if (hw_random(&v))
            b2s_update(&pool, &v, sizeof(v));
    }
    struct blake2s copy = pool;
    uint8_t h[32];
    b2s_final(&copy, h);
    for (int i = 0; i < 32; i++)
        key[i] ^= h[i];
    b2s_update(&pool, h, sizeof(h));
    memset(h, 0, sizeof(h));
    memset(&copy, 0, sizeof(copy));
}

int random_ready(void)
{
    /* With a hardware generator at once; otherwise once enough
       interrupts have been timed - a few seconds of the timer alone */
    return seeded && (have_rdseed || have_rdrand || interrupts_seen >= 256);
}

void random_bytes(void *out, uint32_t n)
{
    uint8_t block[64], nonce[12] = { 0 };
    uint8_t *p = out;
    irqflags_t f = spin_lock_irq(&lock);
    if ((requests++ & 63) == 0 || fast_count >= 64) {
        reseed();
        fast_count = 0;
    }
    /* the first block is the next key; the rest is the output */
    put_le32(nonce, (uint32_t)requests);
    put_le32(nonce + 4, (uint32_t)(requests >> 32));
    chacha20_block(key, 0, nonce, block);
    uint8_t this_key[32];
    memcpy(this_key, key, 32);
    memcpy(key, block, 32);
    spin_unlock_irq(&lock, f);

    for (uint32_t counter = 1; n; counter++) {
        chacha20_block(this_key, counter, nonce, block);
        uint32_t k = n < 64 ? n : 64;
        memcpy(p, block, k);
        p += k;
        n -= k;
    }
    memset(block, 0, sizeof(block));
    memset(this_key, 0, sizeof(this_key));
}

/* --- Start, with the primitives checked against their RFCs ---------- */

static int self_test(void)
{
    /* RFC 7693, appendix B: BLAKE2s-256("abc") */
    static const uint8_t abc[32] = {
        0x50, 0x8C, 0x5E, 0x8C, 0x32, 0x7C, 0x14, 0xE2, 0xE1, 0xA7, 0x2B, 0xA3,
        0x4E, 0xEB, 0x45, 0x2F, 0x37, 0x45, 0x8B, 0x20, 0x9E, 0xD6, 0x3A, 0x29,
        0x4D, 0x99, 0x9B, 0x4C, 0x86, 0x67, 0x59, 0x82,
    };
    struct blake2s s;
    uint8_t h[32];
    b2s_init(&s);
    b2s_update(&s, "abc", 3);
    b2s_final(&s, h);
    if (memcmp(h, abc, 32))
        return -1;
    /* RFC 8439, 2.3.2: the block function */
    uint8_t k[32], nonce[12] = { 0, 0, 0, 0x09, 0, 0, 0, 0x4a, 0, 0, 0, 0 };
    for (int i = 0; i < 32; i++)
        k[i] = (uint8_t)i;
    static const uint8_t first[16] = {
        0x10, 0xf1, 0xe7, 0xe4, 0xd1, 0x3b, 0x59, 0x15,
        0x50, 0x0f, 0xdd, 0x1f, 0xa3, 0x20, 0x71, 0xc4,
    };
    uint8_t out[64];
    chacha20_block(k, 1, nonce, out);
    return memcmp(out, first, 16) ? -2 : 0;
}

void random_init(void)
{
    find_generators();
    int t = self_test();
    b2s_init(&pool);
#if defined(__aarch64__)
    uint32_t rt = rtc_seconds();
#else
    struct rtc_time rt;
    rtc_read(&rt);
#endif
    uint64_t now = rdtsc();
    b2s_update(&pool, &rt, sizeof(rt));
    b2s_update(&pool, &now, sizeof(now));
    reseed();
    seeded = 1;
    if (t)
        printk("[ !! ] Random numbers: the self-test failed (%d)\n", t);
    else
        printk("[ OK ] Random numbers: BLAKE2s and ChaCha20 match RFC 7693 "
               "and 8439; sources: %s%sinterrupt timing\n",
               have_rdseed ? "RDSEED, " : "",
#if defined(__aarch64__)
               have_rdrand ? "RNDR, " : "");
#else
               have_rdrand ? "RDRAND, " : "");
#endif
}
