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
#define SYS_THREAD     28
#define SYS_SETPRIO    29
#define SYS_UDP_OPEN   30
#define SYS_UDP_SEND   31
#define SYS_UDP_RECV   32
#define SYS_UDP_CLOSE  33
#define SYS_NETINFO    34
#define SYS_TCP_CONNECT 35
#define SYS_TCP_LISTEN  36
#define SYS_TCP_ACCEPT  37
#define SYS_TCP_SEND    38
#define SYS_TCP_RECV    39
#define SYS_TCP_CLOSE   40
#define SYS_FORK        41
#define SYS_EXEC        42
#define SYS_WAITANY     43
#define SYS_SIGNAL      44
#define SYS_SIGRETURN   45
#define SYS_MMAP_FILE   46
#define SYS_MSYNC       47

#define FS_NAME_MAX  63

/* Klawisze bez odpowiednika w ASCII - musza zgadzac sie z
   kernel/include/keyboard.h */
#define KEY_UP    0x11
#define KEY_DOWN  0x12
#define KEY_LEFT  0x13
#define KEY_RIGHT 0x14
#define KEY_PGUP  0x15
#define KEY_PGDN  0x16
#define KEY_HOME  0x17
#define KEY_END   0x18

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

/* Rozwidla proces. Zwraca pid dziecka rodzicowi, zero dziecku,
   -1 przy bledzie. Dziecko dostaje wlasna kopie pamieci (fizycznie
   te sama az do pierwszego zapisu - kopiowanie przy zapisie). */
static inline int32_t sys_fork(void)
{
    int32_t ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(SYS_FORK) : "memory");
    return ret;
}

/* Czeka na dowolne wlasne dziecko. Zwraca jego pid, a kod wyjscia
   wpisuje pod 'code' (moze byc NULL). -1 = nie ma juz dzieci. */
static inline int32_t sys_waitany(int32_t *code)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_WAITANY), "b"(code)
                     : "memory");
    return ret;
}

/* Podmienia program biezacego procesu (pid i deskryptory zostaja).
   Przy powodzeniu nie wraca; -1 oznacza, ze programu nie da sie
   uruchomic i stary obraz dziala dalej. */
static inline int32_t sys_exec(const char *cmdline)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_EXEC), "b"(cmdline)
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

/* Plik widziany jako pamiec.
 *
 * Zwraca adres, pod ktorym widac zawartosc pliku, i wpisuje jego
 * rozmiar. Zadna strona nie powstaje od razu - bajty przychodza
 * z dysku pod palcami, wiec odwzorowanie duzego pliku kosztuje tyle,
 * co spis jego blokow. Ta wersja jest tylko do odczytu: proba zapisu
 * konczy sie bledem ochrony, a nie cichym zgubieniem danych. */
static inline const void *sys_mmap_file(const char *nazwa, uint32_t *rozmiar)
{
    const void *ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_MMAP_FILE), "b"(nazwa), "c"(rozmiar), "d"(0)
                     : "memory");
    return ret;
}

/* To samo, ale z prawem zapisu: zmiany w pamieci wracaja do pliku.
 * Nie od razu - dopiero przy sys_msync albo na wyjsciu procesu. */
static inline void *sys_mmap_file_w(const char *nazwa, uint32_t *rozmiar)
{
    void *ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_MMAP_FILE), "b"(nazwa), "c"(rozmiar), "d"(1)
                     : "memory");
    return ret;
}

/* Odsyla do plikow zmienione strony zapisywalnych odwzorowan.
 * Zwraca, ile stron poszlo na dysk - strona nietkniete od ostatniego
 * razu nie kosztuje nic, bo o tym, ktore sa brudne, mowi procesor. */
static inline int sys_msync(void)
{
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(SYS_MSYNC) : "memory");
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

/* thread: tworzy watek w tym samym procesie (wspolna pamiec). entry to
   funkcja void(*)(void*). Zwraca tid albo -1. */
static inline int32_t sys_thread(void (*entry)(void *), void *arg)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_THREAD), "b"(entry), "c"(arg)
                     : "memory");
    return ret;
}

/* setprio: priorytet biezacego zadania (0=wysoki, 1=normalny, 2=niski) */
static inline void sys_setprio(int level)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_SETPRIO), "b"(level)
                     : "memory");
    (void)ret;
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

/* Sygnaly - numery musza zgadzac sie z kernel/include/signal.h */
#define SIGINT   2      /* przerwanie z klawiatury (Ctrl+C) */
#define SIGKILL  9      /* zabicie nie do przechwycenia */
#define SIGTERM 15      /* prosba o zakonczenie */
#define SIGCHLD 17      /* dziecko sie skonczylo */

#define SIG_DFL 0       /* domyslna reakcja jadra */
#define SIG_IGN 1       /* zignoruj sygnal */

typedef void (*sighandler_t)(int32_t sig);

/* Wysyla sygnal do procesu; sig=0 znaczy po prostu SIGKILL */
static inline int32_t sys_kill(int pid, int32_t sig)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_KILL), "b"(pid), "c"(sig)
                     : "memory");
    return ret;
}

/* Trampolina powrotu z procedury obslugi sygnalu.
 *
 * Jadro odklada jej adres jako adres powrotu handlera, wiec gdy ten
 * konczy sie zwyklym 'ret', sterowanie trafia tutaj, a stad przez
 * syscall z powrotem do jadra - to ono odtwarza stan sprzed sygnalu.
 * Handler nie moze wiec po prostu wrocic do przerwanego kodu sam. */
__attribute__((unused, naked))
static void __sig_tramp(void)
{
    __asm__ volatile("movl %0, %%eax\n\t"
                     "int $0x80" : : "i"(SYS_SIGRETURN));
}

/* Ustawia procedure obslugi sygnalu (SIG_DFL / SIG_IGN / wlasna).
   SIGKILL-a przechwycic sie nie da. Zwraca 0 albo -1. */
static inline int32_t sys_signal(int32_t sig, sighandler_t handler)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_SIGNAL), "b"(sig), "c"(handler),
                       "d"(__sig_tramp)
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

/* --- siec: gniazda UDP -------------------------------------------------
   Adresy IPv4 sa w kolejnosci hosta (10.0.2.3 == 0x0A000203). */

struct netinfo {
    uint32_t ip, mask, gw, dns;
};

struct udp_send_cmd {
    uint32_t ip, port, buf, len;
};

struct udp_recv_cmd {
    uint32_t buf, max, timeout_ms;
    uint32_t from_ip, from_port;    /* wypelnia jadro */
};

/* Otwiera gniazdo UDP na porcie (0 = przydziel dowolny wolny). */
static inline int32_t udp_open(uint32_t port)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_UDP_OPEN), "b"(port)
                     : "memory");
    return ret;
}

static inline int32_t udp_send(int32_t sock, uint32_t ip, uint32_t port,
                               const void *buf, uint32_t len)
{
    struct udp_send_cmd c = { ip, port, (uint32_t)buf, len };
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_UDP_SEND), "b"(sock), "c"(&c)
                     : "memory");
    return ret;
}

/* Czeka do timeout_ms na datagram; zwraca dlugosc albo -1.
   from_ip/from_port moga byc zerowe, gdy nadawca nas nie interesuje. */
static inline int32_t udp_recv(int32_t sock, void *buf, uint32_t max,
                               uint32_t timeout_ms, uint32_t *from_ip,
                               uint32_t *from_port)
{
    struct udp_recv_cmd c = { (uint32_t)buf, max, timeout_ms, 0, 0 };
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_UDP_RECV), "b"(sock), "c"(&c)
                     : "memory");
    if (ret >= 0) {
        if (from_ip)
            *from_ip = c.from_ip;
        if (from_port)
            *from_port = c.from_port;
    }
    return ret;
}

static inline void udp_close(int32_t sock)
{
    __asm__ volatile("int $0x80"
                     :
                     : "a"(SYS_UDP_CLOSE), "b"(sock)
                     : "memory");
}

/* Konfiguracja sieci (z DHCP). 0 = OK, -1 = system nie ma jeszcze adresu. */
static inline int32_t netinfo(struct netinfo *ni)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_NETINFO), "b"(ni)
                     : "memory");
    return ret;
}

/* Wypisuje adres IPv4 w postaci a.b.c.d */
static inline void print_ip(uint32_t ip)
{
    for (int i = 3; i >= 0; i--) {
        print_dec((ip >> (i * 8)) & 0xFF);
        if (i)
            print(".");
    }
}

/* --- siec: polaczenia TCP --------------------------------------------- */

struct tcp_io_cmd {
    uint32_t buf, len, timeout_ms;
};

/* Nawiazuje polaczenie; zwraca uchwyt albo -1 */
static inline int32_t tcp_connect(uint32_t ip, uint32_t port,
                                  uint32_t timeout_ms)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_TCP_CONNECT), "b"(ip), "c"(port), "d"(timeout_ms)
                     : "memory");
    return ret;
}

/* Otwiera gniazdo nasluchujace na porcie */
static inline int32_t tcp_listen(uint32_t port)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_TCP_LISTEN), "b"(port)
                     : "memory");
    return ret;
}

/* Odbiera nawiazane polaczenie z kolejki gniazda nasluchujacego */
static inline int32_t tcp_accept(int32_t listener, uint32_t timeout_ms)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_TCP_ACCEPT), "b"(listener), "c"(timeout_ms)
                     : "memory");
    return ret;
}

/* Wysyla dane i czeka na potwierdzenie drugiej strony */
static inline int32_t tcp_send(int32_t conn, const void *buf, uint32_t len,
                               uint32_t timeout_ms)
{
    struct tcp_io_cmd c = { (uint32_t)buf, len, timeout_ms };
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_TCP_SEND), "b"(conn), "c"(&c)
                     : "memory");
    return ret;
}

/* Odbiera dane: >0 = bajty, 0 = druga strona zamknela, -1 = blad/timeout */
static inline int32_t tcp_recv(int32_t conn, void *buf, uint32_t max,
                               uint32_t timeout_ms)
{
    struct tcp_io_cmd c = { (uint32_t)buf, max, timeout_ms };
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_TCP_RECV), "b"(conn), "c"(&c)
                     : "memory");
    return ret;
}

static inline void tcp_close(int32_t conn)
{
    __asm__ volatile("int $0x80"
                     :
                     : "a"(SYS_TCP_CLOSE), "b"(conn)
                     : "memory");
}

/* "10.0.2.2" -> 0x0A000202 (0 = zly zapis) */
static inline uint32_t uparse_ip(const char *s)
{
    uint32_t ip = 0, part = 0;
    int digits = 0, parts = 0;

    for (; *s; s++) {
        if (*s == '.') {
            if (!digits || parts == 3)
                return 0;
            ip = (ip << 8) | (part & 0xFF);
            part = 0;
            digits = 0;
            parts++;
        } else if (*s >= '0' && *s <= '9') {
            part = part * 10 + (uint32_t)(*s - '0');
            if (part > 255 || ++digits > 3)
                return 0;
        } else {
            return 0;
        }
    }
    if (parts != 3 || !digits)
        return 0;
    return (ip << 8) | (part & 0xFF);
}

#endif
