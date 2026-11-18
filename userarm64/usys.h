/* The system calls of the ARM64 kernel, for programs (kernelarm64/proc.c)
 *
 * The PC's numbers (kernel64/syscall64.h) and Linux's way of making the
 * call on AArch64: the number in x8, the arguments in x0-x5, SVC #0, the
 * answer in x0. A program includes this once; it brings its own start
 * and the few library functions the compiler may call by itself.
 */
#ifndef USYS_ARM64_H
#define USYS_ARM64_H

#include <stdint.h>
#include <stddef.h>

#define SYS64_WRITE  1
#define SYS64_EXIT   2
#define SYS64_SLEEP  4
#define SYS64_TICKS  5
#define SYS64_SBRK   15
#define SYS64_GETPID 39

static inline int64_t syscall3(int64_t n, int64_t a, int64_t b, int64_t c)
{
    register int64_t x8 __asm__("x8") = n;
    register int64_t x0 __asm__("x0") = a;
    register int64_t x1 __asm__("x1") = b;
    register int64_t x2 __asm__("x2") = c;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2)
                     : "memory");
    return x0;
}

static inline int64_t sys_write(int fd, const void *buf, uint64_t len)
{
    return syscall3(SYS64_WRITE, fd, (int64_t)(uintptr_t)buf, (int64_t)len);
}

static inline __attribute__((noreturn)) void sys_exit(int code)
{
    syscall3(SYS64_EXIT, code, 0, 0);
    for (;;)
        ;
}

static inline void sys_sleep(unsigned ms) { syscall3(SYS64_SLEEP, ms, 0, 0); }
static inline int64_t sys_ticks(void) { return syscall3(SYS64_TICKS, 0, 0, 0); }
static inline int64_t sys_getpid(void) { return syscall3(SYS64_GETPID, 0, 0, 0); }
static inline void *sys_sbrk(int64_t delta)
{
    return (void *)(uintptr_t)syscall3(SYS64_SBRK, delta, 0, 0);
}

static inline size_t ulen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

static inline void print(const char *s) { sys_write(1, s, ulen(s)); }

static inline void print_hex(uint64_t v)
{
    char b[17];
    for (int i = 15; i >= 0; i--) {
        b[i] = "0123456789abcdef"[v & 15];
        v >>= 4;
    }
    b[16] = 0;
    print(b);
}

static inline void print_dec(int64_t v)
{
    char b[24];
    int i = 23, neg = v < 0;
    uint64_t u = neg ? (uint64_t)-v : (uint64_t)v;
    b[i] = 0;
    do {
        b[--i] = (char)('0' + u % 10);
        u /= 10;
    } while (u);
    if (neg)
        b[--i] = '-';
    print(b + i);
}

/* A line put together first and written with one call: programs on
   other cores write too, and three calls for one line would let their
   pieces in between */
struct line {
    char b[256];
    int n;
};

static inline void line_add(struct line *l, const char *s)
{
    while (*s && l->n < (int)sizeof(l->b) - 1)
        l->b[l->n++] = *s++;
}

static inline void line_hex(struct line *l, uint64_t v)
{
    char b[17];
    for (int i = 15; i >= 0; i--) {
        b[i] = "0123456789abcdef"[v & 15];
        v >>= 4;
    }
    b[16] = 0;
    line_add(l, b);
}

static inline void line_dec(struct line *l, int64_t v)
{
    char b[24];
    int i = 23, neg = v < 0;
    uint64_t u = neg ? (uint64_t)-v : (uint64_t)v;
    b[i] = 0;
    do {
        b[--i] = (char)('0' + u % 10);
        u /= 10;
    } while (u);
    if (neg)
        b[--i] = '-';
    line_add(l, b + i);
}

static inline void line_out(struct line *l)
{
    line_add(l, "\n");
    sys_write(1, l->b, (uint64_t)l->n);
    l->n = 0;
}

/* What the compiler may call of its own accord */
void *memset(void *d, int c, size_t n)
{
    unsigned char *p = d;
    while (n--)
        *p++ = (unsigned char)c;
    return d;
}

void *memcpy(void *d, const void *s, size_t n)
{
    unsigned char *p = d;
    const unsigned char *q = s;
    while (n--)
        *p++ = *q++;
    return d;
}

int main(int argc, char **argv);

/* Where the kernel starts a program: x0 = argc, x1 = argv, the stack
   pointer on a 16-byte line (kernelarm64/proc.c) */
__attribute__((section(".text._start"), noreturn))
void _start(int argc, char **argv)
{
    sys_exit(main(argc, argv));
}

#endif
