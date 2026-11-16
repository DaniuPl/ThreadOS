/* kernel/random.c on the host, for t_random.py
 *
 * The kernel's own source is compiled in whole, with the few kernel
 * headers it needs stood in for (a lock that is never contended here,
 * printk as printf, a clock that reads zero). The test then asks it
 * questions on standard input and checks the answers against Python's
 * BLAKE2s and a ChaCha20 of its own:
 *
 *   b2s <split> <hex>      BLAKE2s-256 of the bytes, fed in pieces
 *                          of <split> bytes
 *   chacha <key> <counter> <nonce>
 *                          one ChaCha20 block
 *   draw <n>               random_bytes(n): the key before and after,
 *                          the request number, whether the pool was
 *                          drawn on, and the output
 *   irq <n>                n interrupts
 *
 * "-" stands for no bytes.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define THREADOS_SPINLOCK_H
#define THREADOS_PRINTK_H
#define THREADOS_RTC_H
struct spinlock { int taken; };
typedef unsigned long irqflags_t;
#define SPINLOCK_FREE { 0 }
static irqflags_t spin_lock_irq(struct spinlock *l) { l->taken = 1; return 0; }
static void spin_unlock_irq(struct spinlock *l, irqflags_t f) { (void)f; l->taken = 0; }
#define printk printf
struct rtc_time { uint16_t year; uint8_t month, day, hour, min, sec; } __attribute__((packed));
static void rtc_read(struct rtc_time *t) { memset(t, 0, sizeof(*t)); }

#include "../../../kernel/random.c"

static size_t unhex(const char *s, uint8_t *out, size_t max)
{
    size_t n = 0;
    if (!strcmp(s, "-"))
        return 0;
    while (s[0] && s[1] && n < max) {
        unsigned v;
        sscanf(s, "%2x", &v);
        out[n++] = (uint8_t)v;
        s += 2;
    }
    return n;
}

static void put_hex(const uint8_t *p, size_t n)
{
    if (!n)
        printf("-");
    for (size_t i = 0; i < n; i++)
        printf("%02x", p[i]);
}

int main(void)
{
    static char line[1 << 16], arg1[1 << 15], arg3[256];
    static uint8_t data[1 << 14];
    setvbuf(stdout, 0, _IOLBF, 0);
    random_init();
    while (fgets(line, sizeof(line), stdin)) {
        unsigned long a, b;
        if (sscanf(line, "b2s %lu %32767s", &a, arg1) == 2) {
            size_t n = unhex(arg1, data, sizeof(data));
            struct blake2s s;
            uint8_t h[32];
            b2s_init(&s);
            for (size_t at = 0; at < n; at += a)
                b2s_update(&s, data + at, (uint32_t)(n - at < a ? n - at : a));
            b2s_final(&s, h);
            put_hex(h, 32);
            printf("\n");
        } else if (sscanf(line, "chacha %32767s %lu %255s", arg1, &a, arg3) == 3) {
            uint8_t k[32], nonce[12], out[64];
            unhex(arg1, k, 32);
            unhex(arg3, nonce, 12);
            chacha20_block(k, (uint32_t)a, nonce, out);
            put_hex(out, 64);
            printf("\n");
        } else if (sscanf(line, "draw %lu", &a) == 1 && a <= sizeof(data)) {
            uint8_t before[32];
            int pool = (requests & 63) == 0 || fast_count >= 64;
            memcpy(before, key, 32);
            random_bytes(data, (uint32_t)a);
            printf("pool %d request %llu key ", pool, (unsigned long long)requests);
            put_hex(before, 32);
            printf(" next ");
            put_hex(key, 32);
            printf(" out ");
            put_hex(data, a);
            printf("\n");
        } else if (sscanf(line, "irq %lu", &b) == 1) {
            for (unsigned long i = 0; i < b; i++)
                random_add_interrupt((int)(i & 15));
            printf("ok\n");
        } else {
            printf("?\n");
        }
    }
    return 0;
}
