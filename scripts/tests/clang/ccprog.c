/* What clang on ThreadOS builds and the host's gcc builds against glibc
 * (scripts/tests/t_clang.py): the two programs are to print the same.
 *
 * Each part leans on something a compiler and its runtime must get
 * right besides plain arithmetic: compiler-rt's 128-bit division and
 * conversions, the x87's long double, floating point printed to the
 * digit, thread-local storage across threads, qsort with a callback,
 * switch tables and function pointers, and files written and read.
 */

#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t fnv(uint32_t h, const void *p, size_t n)
{
    const unsigned char *b = p;
    for (size_t i = 0; i < n; i++) {
        h ^= b[i];
        h *= 16777619u;
    }
    return h;
}

static void sieve(void)
{
    enum { N = 200000 };
    static unsigned char composite[N];
    unsigned count = 0;
    unsigned long long sum = 0;
    for (unsigned i = 2; i < N; i++) {
        if (composite[i])
            continue;
        count++;
        sum += i;
        for (unsigned long long j = (unsigned long long)i * i; j < N; j += i)
            composite[j] = 1;
    }
    printf("primes below %d: %u, their sum %llu\n", N, count, sum);
}

static void wide(void)
{
    /* 34! does not fit 128 bits; taken modulo the Mersenne prime
       2^127 - 1 it needs 128-bit division at every step */
    const unsigned __int128 m = ((unsigned __int128)1 << 127) - 1;
    unsigned __int128 f = 1;
    for (unsigned k = 2; k <= 34; k++)
        f = f * k % m;
    printf("34! mod (2^127-1) = %016llx%016llx\n",
           (unsigned long long)(f >> 64), (unsigned long long)f);

    __int128 neg = -((__int128)123456789 << 70);
    __int128 q = neg / 1000003, r = neg % 1000003;
    printf("signed: quotient %lld (high %lld), remainder %lld\n",
           (long long)q, (long long)(q >> 64), (long long)r);

    double d = (double)((unsigned __int128)1 << 100) * 3.0;
    unsigned __int128 back = (unsigned __int128)d;
    printf("2^100*3 as a double %.17g, and back %016llx%016llx\n", d,
           (unsigned long long)(back >> 64), (unsigned long long)back);
    printf("1e19 as unsigned long long: %llu\n", (unsigned long long)1e19);
}

static void floating(void)
{
    double s = 0;
    for (int k = 1; k <= 1000000; k++)
        s += 1.0 / ((double)k * k);
    printf("sum of 1/k^2 to a million: %.15f\n", s);
    printf("sqrt(2) %.17g, floor(-2.5) %.1f, fmod(10, 3) %.1f\n",
           sqrt(2.0), floor(-2.5), fmod(10.0, 3.0));
    long double third = 1.0L / 3;
    printf("long double 1/3: %.20Lf (%zu bytes of it)\n", third,
           sizeof(long double));
    printf("float 0.1f: %.10f, 1e-310 %g, -0.0 %g\n", 0.1f, 1e-310, -0.0);
}

static _Thread_local unsigned long tl_sum;

static void *worker(void *arg)
{
    unsigned long n = (unsigned long)(uintptr_t)arg;
    for (unsigned long i = 1; i <= 100000 * n; i++)
        tl_sum += i % 7;
    return (void *)(uintptr_t)tl_sum;
}

static void threads(void)
{
    pthread_t t[4];
    unsigned long got[4];
    for (int i = 0; i < 4; i++)
        pthread_create(&t[i], 0, worker, (void *)(uintptr_t)(i + 1));
    for (int i = 0; i < 4; i++) {
        void *r;
        pthread_join(t[i], &r);
        got[i] = (unsigned long)(uintptr_t)r;
    }
    printf("four threads, each its own thread-local sum: %lu %lu %lu %lu; "
           "the main thread's still %lu\n", got[0], got[1], got[2], got[3],
           tl_sum);
}

static int by_value(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static void sorting(void)
{
    enum { N = 50000 };
    static uint32_t v[N];
    uint32_t x = 12345;
    for (int i = 0; i < N; i++) {
        x = x * 1664525u + 1013904223u;
        v[i] = x;
    }
    qsort(v, N, sizeof(v[0]), by_value);
    int sorted = 1;
    for (int i = 1; i < N; i++)
        sorted &= v[i - 1] <= v[i];
    printf("qsort of %d numbers: %s, smallest %u, largest %u, fnv %08x\n", N,
           sorted ? "in order" : "OUT OF ORDER", v[0], v[N - 1],
           fnv(2166136261u, v, sizeof(v)));
}

typedef unsigned (*op_fn)(unsigned, unsigned);
static unsigned op_add(unsigned a, unsigned b) { return a + b; }
static unsigned op_xor(unsigned a, unsigned b) { return a ^ b; }
static unsigned op_rot(unsigned a, unsigned b) { return a << (b & 31) | a >> (-b & 31); }

static unsigned dispatch(unsigned what, unsigned a, unsigned b)
{
    switch (what % 9) {
    case 0: return a * 3;
    case 1: return a - b;
    case 2: return a | b;
    case 3: return a & ~b;
    case 4: return (unsigned)__builtin_popcount(a) + b;
    case 5: return a ? (unsigned)__builtin_clz(a) : 32;
    case 6: return a / (b | 1);
    case 7: return a % (b | 1);
    default: return ~a;
    }
}

static void control(void)
{
    static const op_fn ops[3] = { op_add, op_xor, op_rot };
    unsigned a = 1, b = 2;
    for (unsigned i = 0; i < 300000; i++) {
        a = ops[i % 3](a, b) + dispatch(i, a, b);
        b = b * 2654435761u + i;
    }
    printf("switch tables and function pointers: %08x %08x\n", a, b);
}

static void files(void)
{
    char line[256];
    FILE *f = fopen("ccprog.out", "w");
    if (!f) {
        printf("files: cannot write ccprog.out\n");
        return;
    }
    for (int i = 0; i < 1000; i++)
        fprintf(f, "line %d of the file, %x\n", i, i * 2654435761u);
    fclose(f);
    f = fopen("ccprog.out", "r");
    uint32_t h = 2166136261u;
    int lines = 0;
    while (f && fgets(line, sizeof(line), f)) {
        h = fnv(h, line, strlen(line));
        lines++;
    }
    if (f)
        fclose(f);
    remove("ccprog.out");
    printf("files: %d lines written and read back, fnv %08x\n", lines, h);
}

int main(void)
{
    sieve();
    wide();
    floating();
    threads();
    sorting();
    control();
    files();
    return 0;
}
