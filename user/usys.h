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
#define SYS_READDIR  14
#define SYS_UNLINK   15
#define SYS_TIME     16
#define SYS_CHDIR    17
#define SYS_GETCWD   18
#define SYS_MKDIR    19
#define SYS_KILL     20
#define SYS_WIN_CREATE 21
#define SYS_WIN_RECT   22
#define SYS_WIN_TEXT   23
#define SYS_WIN_POLL   24
#define SYS_MMAP       25
#define SYS_SHM        26
#define SYS_SYSINFO    27

#define FS_NAME_MAX  23

struct sysinfo {
    uint32_t ram_kib;
    uint32_t used_kib;
    uint32_t uptime_s;
};

/* Uklad pol musi byc zgodny ze struct rtc_time w jadrze */
struct utime {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t min;
    uint8_t sec;
} __attribute__((packed));

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

/* Uruchamia program; outfile != NULL kieruje stdout do pliku
   (append=1 dopisuje na koncu zamiast obcinac) */
static inline int32_t sys_spawn(const char *cmdline, const char *outfile,
                                int append)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_SPAWN), "b"(cmdline), "c"(outfile), "d"(append)
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

/* Czeka na proces 'pid' i zwraca jego kod wyjscia */
static inline int32_t sys_wait(int32_t pid)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_WAIT), "b"(pid)
                     : "memory");
    return ret;
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

/* mmap: swieza, wyzerowana pamiec procesu; zwraca adres albo NULL */
static inline void *sys_mmap(uint32_t size)
{
    void *ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_MMAP), "b"(size)
                     : "memory");
    return ret;
}

/* shm: pamiec wspoldzielona po kluczu; ten sam klucz = ta sama pamiec */
static inline void *sys_shm(int key, uint32_t size)
{
    void *ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_SHM), "b"(key), "c"(size)
                     : "memory");
    return ret;
}

/* sysinfo: statystyki systemu (RAM, uptime) do wskazanego bufora */
static inline void sys_sysinfo(struct sysinfo *si)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_SYSINFO), "b"(si)
                     : "memory");
    (void)ret;
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

/* Enumeracja katalogu roboczego: kolejny wpis o numerze 'index'.
   Zwraca rozmiar pliku, -2 dla katalogu albo -1 gdy nie ma wiecej wpisow. */
static inline int32_t sys_readdir(int index, char *name)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_READDIR), "b"(index), "c"(name)
                     : "memory");
    return ret;
}

static inline int32_t sys_chdir(const char *path)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_CHDIR), "b"(path)
                     : "memory");
    return ret;
}

static inline void sys_getcwd(char *buf)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_GETCWD), "b"(buf)
                     : "memory");
    (void)ret;
}

static inline int32_t sys_mkdir(const char *path)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_MKDIR), "b"(path)
                     : "memory");
    return ret;
}

static inline int32_t sys_kill(int pid)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_KILL), "b"(pid)
                     : "memory");
    return ret;
}

/* --- GUI: okno aplikacji rysowane na pulpicie --- */

struct win_rect_cmd {
    int32_t x, y, w, h, color;
};
struct win_text_cmd {
    int32_t x, y, color;
    const char *s;
};

/* Tworzy okno o obszarze treści cw x ch; zwraca winid albo -1 */
static inline int32_t win_create(int cw, int ch, const char *title)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_WIN_CREATE), "b"(cw), "c"(ch), "d"(title)
                     : "memory");
    return ret;
}

static inline void win_rect(int winid, int x, int y, int w, int h, int color)
{
    struct win_rect_cmd c = { x, y, w, h, color };
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_WIN_RECT), "b"(winid), "c"(&c)
                     : "memory");
    (void)ret;
}

static inline void win_text(int winid, int x, int y, const char *s, int color)
{
    struct win_text_cmd c = { x, y, color, s };
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_WIN_TEXT), "b"(winid), "c"(&c)
                     : "memory");
    (void)ret;
}

#define WEV_NONE  0
#define WEV_CLICK 1
#define WEV_KEY   2
struct win_event {
    int32_t type;          /* WEV_* */
    int32_t x, y;          /* wspolrzedne wzgledem obszaru treści */
    int32_t val;           /* maska przyciskow */
};

/* Pobiera zdarzenie okna (nieblokujace). Zwraca 1 = jest, 0 = brak. */
static inline int32_t win_poll(int winid, struct win_event *ev)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_WIN_POLL), "b"(winid), "c"(ev)
                     : "memory");
    return ret;
}

static inline int32_t sys_unlink(const char *name)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_UNLINK), "b"(name)
                     : "memory");
    return ret;
}

static inline void sys_time(struct utime *t)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_TIME), "b"(t)
                     : "memory");
    (void)ret;
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
