/* Slownik zgodnosci dla programow 64-bitowych
 *
 * Kryptografia, TLS, HTTP, DNS i parser HTML zyja w naglowkach
 * w user/ i nie wiedza nic o trybie procesora - licza na bajtach.
 * Do jadra mowia jednak kilkoma nazwami: polacz sie, wyslij, odbierz,
 * ktora godzina, ile tyknelo, otworz plik.
 *
 * Te nazwy sa umowa. Co za nimi stoi, jest sprawa jadra - i ten plik
 * jest cala roznica miedzy jedna strona a druga, dokladnie tak samo,
 * jak kernel64/compat64.c po stronie jadra.
 *
 * Roznica w ksztalcie wywolan bierze sie stad, ze w trybie dlugim
 * argumenty ida rejestrami, a nie strukturami wskazywanymi przez ECX:
 * rejestrow jest wiecej i wiekszosc opisow zmiescila sie wprost.
 */

#ifndef PROTONOS_COMPAT64_H
#define PROTONOS_COMPAT64_H

#include "usys64.h"

/* --- Pliki --------------------------------------------------------
 *
 * Jadro 64-bitowe nie ma jeszcze flag otwarcia: plik albo istnieje,
 * albo zaklada sie go przy otwarciu. Tlumaczymy wiec flagi na to
 * jedno pytanie, ktore umie zadac. */
#define O_READ   1
#define O_WRITE  2
#define O_CREATE 4
#define O_TRUNC  8

#define STDIN   FD_STDIN
#define STDOUT  FD_STDOUT

static inline int32_t uz_open(const char *name, int flags)
{
    return (int32_t)sys_open(name, (flags & O_CREATE) ? 1 : 0);
}
#define sys_open(name, flags) uz_open((name), (flags))

/* --- Klawisze bez odpowiednika w ASCII -----------------------------
 *
 * Musza zgadzac sie z kernel/include/keyboard.h - to jadro wklada te
 * kody do kolejki, a tlumaczy je wspolny kernel/kbdmap.c. */
#define KEY_UP    0x11
#define KEY_DOWN  0x12
#define KEY_LEFT  0x13
#define KEY_RIGHT 0x14
#define KEY_PGUP  0x15
#define KEY_PGDN  0x16
#define KEY_HOME  0x17
#define KEY_END   0x18

/* --- Czas ---------------------------------------------------------- */

struct utime {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t min;
    uint8_t sec;
} __attribute__((packed));

/* Ten sam uklad pol, co struct czas64 - inne sa tylko nazwy. Kod
   wspolny mowi po angielsku, bo mowi tak takze X.509; jadro po
   polsku, bo tak mowi cala reszta. Bajty sa te same. */
static inline void uz_time(struct utime *t)
{
    syscall64(SYS64_TIME, (int64_t)(uintptr_t)t, 0);
}
#define sys_time(t) uz_time((struct utime *)(t))

/* Numer wlasnego procesu - do zasiewu generatora liczb losowych */
static inline int32_t sys_getpid(void)
{
    return (int32_t)syscall64(SYS64_GETPID, 0, 0);
}

static inline uint32_t sys_getticks(void)
{
    return (uint32_t)sys_ticks();
}

/* --- Wypisywanie liczb ---------------------------------------------- */

/* Czekanie na dowolne wlasne dziecko. W jadrze 32-bitowym nazywa sie
   to sys_waitany, w 64-bitowym sys_wait - kod wspolny mowi jednym
   slowem, a ten plik tlumaczy je na wlasciwa strone. */
static inline int64_t sys_wait_child(int *code)
{
    return sys_wait(code);
}

static inline void print_dec(uint32_t v)
{
    char buf[24];
    utoa(v, buf);
    print(buf);
}

/* Adres IP w postaci, ktora czyta czlowiek */
static inline void print_ip(uint32_t ip)
{
    for (int i = 3; i >= 0; i--) {
        print_dec((ip >> (i * 8)) & 0xFF);
        if (i)
            print(".");
    }
}

static inline int ustreq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* --- UDP ------------------------------------------------------------
 *
 * Ksztalt jest tu inny: w trybie dlugim gniazdo, adres, port i limit
 * czasu jada w jednym opisie, bo tak bylo czytelniej niz upychac je
 * w rejestrach. Program o tym nie musi wiedziec. */

static inline int32_t udp_open(uint32_t port)
{
    return (int32_t)sys_udp_open((uint16_t)port);
}

static inline int32_t udp_send(int32_t sock, uint32_t ip, uint32_t port,
                               const void *buf, uint32_t len)
{
    struct udp_info o = {(int)sock, ip, (uint16_t)port, 0, 0};
    return (int32_t)sys_udp_send(&o, buf, len);
}

static inline int32_t udp_recv(int32_t sock, void *buf, uint32_t max,
                               uint32_t timeout_ms, uint32_t *from_ip,
                               uint32_t *from_port)
{
    struct udp_info o = {(int)sock, 0, 0, 0, timeout_ms};
    int32_t n = (int32_t)sys_udp_recv(&o, buf, max);
    if (n >= 0) {
        /* Adres nadawcy jadro wpisuje z powrotem w ten sam opis */
        if (from_ip)
            *from_ip = o.ip;
        if (from_port)
            *from_port = o.port;
    }
    return n;
}

static inline void udp_close(int32_t sock)
{
    sys_udp_close((int)sock);
}

/* --- TCP ------------------------------------------------------------ */

static inline int32_t tcp_connect(uint32_t ip, uint32_t port,
                                  uint32_t timeout_ms)
{
    struct udp_info o = {0, ip, (uint16_t)port, 0, timeout_ms};
    return (int32_t)sys_tcp_connect(&o);
}

static inline int32_t tcp_send(int32_t conn, const void *buf, uint32_t len,
                               uint32_t timeout_ms)
{
    (void)timeout_ms;               /* limit ustala jadro */
    return (int32_t)sys_tcp_send((int)conn, buf, len);
}

static inline int32_t tcp_recv(int32_t conn, void *buf, uint32_t max,
                               uint32_t timeout_ms)
{
    (void)timeout_ms;
    return (int32_t)sys_tcp_recv((int)conn, buf, max);
}

static inline void tcp_close(int32_t conn)
{
    sys_tcp_close((int)conn);
}

/* --- Adres wlasny --------------------------------------------------- */

struct netinfo {
    uint32_t ip, mask, gw, dns;
};

static inline int32_t netinfo(struct netinfo *ni)
{
    uint32_t addrs[4];
    if (sys_netinfo(addrs) != 1)
        return -1;
    ni->ip = addrs[0];
    ni->mask = addrs[1];
    ni->gw = addrs[2];
    ni->dns = addrs[3];
    return 0;
}

#endif
