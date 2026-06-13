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
#define SYS_FWRITE   10
#define SYS_FREAD    11

static inline int32_t sys_write(const char *s, uint32_t len)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_WRITE), "b"(s), "c"(len)
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

static inline int32_t sys_read(char *buf, uint32_t len)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_READ), "b"(buf), "c"(len)
                     : "memory");
    return ret;
}

static inline int32_t sys_spawn(const char *name)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_SPAWN), "b"(name)
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

static inline int32_t sys_fwrite(const char *name, const void *buf, uint32_t len)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_FWRITE), "b"(name), "c"(buf), "d"(len)
                     : "memory");
    return ret;
}

static inline int32_t sys_fread(const char *name, void *buf, uint32_t max)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_FREAD), "b"(name), "c"(buf), "d"(max)
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

static inline void print(const char *s)
{
    sys_write(s, ustrlen(s));
}

#endif
