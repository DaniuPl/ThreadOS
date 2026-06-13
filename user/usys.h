/* Biblioteczka programow uzytkownika ProtonOS - syscalle int 0x80 */

#ifndef PROTONOS_USYS_H
#define PROTONOS_USYS_H

#include <stdint.h>

#define SYS_WRITE    1
#define SYS_EXIT     2
#define SYS_SLEEP    3
#define SYS_GETTICKS 4
#define SYS_READ     5
#define SYS_SPAWN    6
#define SYS_WAIT     7
#define SYS_GETPID   8
#define SYS_SBRK     9
#define SYS_OPEN     10
#define SYS_CLOSE    11
#define SYS_SEEK     12
#define SYS_PIPELINE 13

/* Flagi open() */
#define O_READ   1
#define O_WRITE  2
#define O_CREATE 4
#define O_TRUNC  8

/* Standardowe deskryptory */
#define STDIN  0
#define STDOUT 1
#define STDERR 2

static inline int32_t sys_write(int fd, const void *buf, uint32_t len)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_WRITE), "b"(fd), "c"(buf), "d"(len)
                     : "memory");
    return ret;
}

static inline void sys_exit(int code)
{
    __asm__ volatile("int $0x80" : : "a"(SYS_EXIT), "b"(code) : "memory");
    __builtin_unreachable();
}

static inline void sys_sleep(uint32_t ms)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_SLEEP), "b"(ms)
                     : "memory");
    (void)ret;
}

static inline uint32_t sys_getticks(void)
{
    uint32_t ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(SYS_GETTICKS) : "memory");
    return ret;
}

static inline int32_t sys_read(int fd, void *buf, uint32_t len)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_READ), "b"(fd), "c"(buf), "d"(len)
                     : "memory");
    return ret;
}

/* Uruchamia program; gdy outfile != NULL, jego stdout idzie do pliku */
static inline int32_t sys_spawn(const char *cmdline, const char *outfile)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_SPAWN), "b"(cmdline), "c"(outfile)
                     : "memory");
    return ret;
}

/* Potok 'a | b | c'; outfile != NULL kieruje stdout ostatniego do pliku */
static inline int32_t sys_pipeline(const char *pipeline, const char *outfile)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_PIPELINE), "b"(pipeline), "c"(outfile)
                     : "memory");
    return ret;
}

static inline void sys_wait(int32_t pid)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_WAIT), "b"(pid)
                     : "memory");
    (void)ret;
}

static inline int32_t sys_getpid(void)
{
    int32_t ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(SYS_GETPID) : "memory");
    return ret;
}

static inline void *sys_sbrk(int32_t increment)
{
    void *ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_SBRK), "b"(increment)
                     : "memory");
    return ret;
}

/* Najprostszy bump-allocator na sbrk: tylko alokuje, nie zwalnia.
   W zupelnosci wystarczy dla zabawkowych programow. */
static inline void *malloc(uint32_t size)
{
    size = (size + 7) & ~7u;
    void *p = sys_sbrk((int32_t)size);
    return p == (void *)-1 ? (void *)0 : p;
}

static inline int32_t sys_open(const char *name, int flags)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_OPEN), "b"(name), "c"(flags)
                     : "memory");
    return ret;
}

static inline int32_t sys_close(int fd)
{
    int32_t ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(SYS_CLOSE), "b"(fd) : "memory");
    return ret;
}

static inline int32_t sys_seek(int fd, uint32_t off)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_SEEK), "b"(fd), "c"(off)
                     : "memory");
    return ret;
}

static inline int ustreq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static inline uint32_t ustrlen(const char *s)
{
    uint32_t n = 0;
    while (s[n])
        n++;
    return n;
}

/* Czy w 'hay' wystepuje podlancuch 'needle' */
static inline int ustrstr(const char *hay, const char *needle)
{
    if (!*needle)
        return 1;
    for (; *hay; hay++) {
        const char *h = hay, *n = needle;
        while (*h && *n && *h == *n) {
            h++;
            n++;
        }
        if (!*n)
            return 1;
    }
    return 0;
}

static inline int uatoi(const char *s)
{
    int v = 0;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return v;
}

/* Wypisuje liczbe dziesietna na stdout */
static inline void print_dec(uint32_t v)
{
    char buf[12];
    int n = 0;
    do {
        buf[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        sys_write(STDOUT, &buf[--n], 1);
}

static inline void print(const char *s)
{
    sys_write(STDOUT, s, ustrlen(s));
}

#endif
