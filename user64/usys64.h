/* Biblioteczka programow ProtonOS 64 - wywolania przez instrukcje syscall
 *
 * W trybie dlugim do jadra nie wchodzi sie juz bramka przerwania.
 * Instrukcja syscall zabiera numer w RAX, argumenty w RDI i RSI,
 * a wynik oddaje w RAX. Niszczy przy tym RCX i R11 - procesor zostawia
 * w nich adres powrotu i flagi - wiec trzeba je wymienic w liscie
 * rejestrow psutych, inaczej kompilator uzna, ze przezyly wywolanie.
 *
 * Poza tymi dwoma i RAX z wyniku wszystkie rejestry wracaja
 * nietkniete: jadro odklada je u siebie. To jest czesc umowy i na niej
 * opiera sie ta lista.
 */

#ifndef PROTONOS_USYS64_H
#define PROTONOS_USYS64_H

#include <stdint.h>

#define SYS64_WRITE 1
#define SYS64_EXIT  2
#define SYS64_RING  3
#define SYS64_SLEEP 4
#define SYS64_TICKS 5
#define SYS64_FORK  6
#define SYS64_EXEC  7
#define SYS64_WAIT  8
#define SYS64_SIGNAL    9
#define SYS64_SIGRETURN 10
#define SYS64_KILL      11
#define SYS64_READ      12
#define SYS64_OPEN      13
#define SYS64_CLOSE     14
#define SYS64_SBRK      15
#define SYS64_MKDIR     16
#define SYS64_CHDIR     17
#define SYS64_GETCWD    18
#define SYS64_READDIR   19
#define SYS64_UNLINK    20
#define SYS64_PIPE      21
#define SYS64_DUP2      22
#define SYS64_NETINFO     23
#define SYS64_UDP_OPEN    24
#define SYS64_UDP_SEND    25
#define SYS64_UDP_RECV    26
#define SYS64_UDP_CLOSE   27
#define SYS64_TCP_CONNECT 28
#define SYS64_TCP_SEND    29
#define SYS64_TCP_RECV    30
#define SYS64_TCP_CLOSE   31
#define SYS64_WIN_CREATE 32
#define SYS64_WIN_RECT   33
#define SYS64_WIN_TEXT   34
#define SYS64_WIN_POLL   35
#define SYS64_SHM        36
#define SYS64_TIME       37
#define SYS64_SYSINFO    38
#define SYS64_GETPID     39
#define SYS64_MMAP_FILE  40

#define NAME_MAX64 63

/* Numery zajete od zawsze: zero to klawiatura, jeden to konsola */
#define FD_STDIN   0
#define FD_STDOUT  1

#define SIGINT   2
#define SIGKILL  9
#define SIGTERM 15

static inline int64_t syscall64(int64_t nr, int64_t a1, int64_t a2)
{
    int64_t ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr), "D"(a1), "S"(a2)
                     : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t syscall64_3(int64_t nr, int64_t a1, int64_t a2,
                                  int64_t a3)
{
    int64_t ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return ret;
}

static inline uint64_t ustrlen(const char *s)
{
    uint64_t n = 0;
    while (s[n])
        n++;
    return n;
}

/* Wypisanie na konsole to zwykly zapis do deskryptora numer jeden -
   dokladnie taki sam, jak zapis do pliku */
static inline void print(const char *s)
{
    syscall64_3(SYS64_WRITE, FD_STDOUT, (int64_t)(uintptr_t)s,
                (int64_t)ustrlen(s));
}

static inline int64_t sys_write(int fd, const void *buf, uint64_t len)
{
    return syscall64_3(SYS64_WRITE, fd, (int64_t)(uintptr_t)buf,
                       (int64_t)len);
}

/* Otwiera plik w katalogu glownym. create=1 zaklada go, gdy nie ma. */
static inline int64_t sys_open(const char *nazwa, int create)
{
    return syscall64_3(SYS64_OPEN, (int64_t)(uintptr_t)nazwa,
                       (int64_t)ustrlen(nazwa), create);
}

static inline int64_t sys_close(int fd)
{
    return syscall64(SYS64_CLOSE, fd, 0);
}

static inline void sys_exit(int code)
{
    syscall64(SYS64_EXIT, code, 0);
    for (;;)
        ;                            /* jadro tu nie wroci */
}

static inline void sys_sleep(uint32_t ms)
{
    syscall64(SYS64_SLEEP, (int64_t)ms, 0);
}

static inline uint64_t sys_ticks(void)
{
    return (uint64_t)syscall64(SYS64_TICKS, 0, 0);
}

/* Selektor kodu zna tylko procesor, a jego dwa najnizsze bity mowia,
   w ktorym pierscieniu program NAPRAWDE dziala. Program nie ma jak
   tego zmyslic - moze tylko przekazac to, co przeczytal. */
static inline void sys_report_ring(void)
{
    uint64_t cs;
    __asm__ volatile("movw %%cs, %%ax; movzwq %%ax, %0" : "=r"(cs) :: "rax");
    syscall64(SYS64_RING, (int64_t)cs, 0);
}

/* Liczba dziesietna do bufora; zwraca dlugosc */
static inline int utoa(uint64_t v, char *buf)
{
    char tmp[24];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v);
    for (int i = 0; i < n; i++)
        buf[i] = tmp[n - 1 - i];
    buf[n] = 0;
    return n;
}

/* Powiela proces. W rodzicu zwraca numer dziecka, w dziecku zero.
   Od tego miejsca sa to dwa programy: ta sama pamiec, ale juz
   skopiowana - zapis jednego nie jest widoczny dla drugiego. */
static inline int64_t sys_fork(void)
{
    return syscall64(SYS64_FORK, 0, 0);
}

/* Podmienia wlasny obraz na program z initrd. Przy powodzeniu nie
   wraca - od nastepnej instrukcji dziala juz tamten program.
   argv jest tablica wskaznikow zakonczona zerem (moze byc NULL). */
static inline int64_t sys_exec(const char *name, char *const argv[])
{
    return syscall64_3(SYS64_EXEC, (int64_t)(uintptr_t)name,
                       (int64_t)ustrlen(name), (int64_t)(uintptr_t)argv);
}

/* --- Siec ---------------------------------------------------------
 *
 * Opis gniazda w jednym miejscu, bo trzy rejestry na argumenty to za
 * malo na gniazdo, adres, port, dlugosc i limit czasu naraz. Uklad
 * musi zgadzac sie z tym w kernel64/syscall64.c.
 *
 * Przy odbiorze jadro wpisuje tu z powrotem adres i port nadawcy.
 */
struct udp_opis {
    int sock;
    uint32_t ip;
    uint16_t port;
    uint16_t _wyrownanie;
    uint32_t timeout;
};

/* Wlasny adres, maska, brama i DNS. Zwraca 1, gdy system ma juz
   adres z sieci. */
static inline int64_t sys_netinfo(uint32_t adresy[4])
{
    return syscall64(SYS64_NETINFO, (int64_t)(uintptr_t)adresy, 0);
}

static inline int64_t sys_udp_open(uint16_t port)
{
    return syscall64(SYS64_UDP_OPEN, port, 0);
}

static inline int64_t sys_udp_send(struct udp_opis *o, const void *buf,
                                   uint64_t len)
{
    return syscall64_3(SYS64_UDP_SEND, (int64_t)(uintptr_t)o,
                       (int64_t)(uintptr_t)buf, (int64_t)len);
}

static inline int64_t sys_udp_recv(struct udp_opis *o, void *buf,
                                   uint64_t max)
{
    return syscall64_3(SYS64_UDP_RECV, (int64_t)(uintptr_t)o,
                       (int64_t)(uintptr_t)buf, (int64_t)max);
}

static inline int64_t sys_udp_close(int sock)
{
    return syscall64(SYS64_UDP_CLOSE, sock, 0);
}

static inline int64_t sys_tcp_connect(struct udp_opis *o)
{
    return syscall64(SYS64_TCP_CONNECT, (int64_t)(uintptr_t)o, 0);
}

static inline int64_t sys_tcp_send(int conn, const void *buf, uint64_t len)
{
    return syscall64_3(SYS64_TCP_SEND, conn, (int64_t)(uintptr_t)buf,
                       (int64_t)len);
}

static inline int64_t sys_tcp_recv(int conn, void *buf, uint64_t max)
{
    return syscall64_3(SYS64_TCP_RECV, conn, (int64_t)(uintptr_t)buf,
                       (int64_t)max);
}

static inline int64_t sys_tcp_close(int conn)
{
    return syscall64(SYS64_TCP_CLOSE, conn, 0);
}

/* Adres z napisu "10.0.2.2"; -1 gdy to nie adres */
static inline int64_t uparse_ip(const char *s)
{
    uint32_t ip = 0;
    for (int czlon = 0; czlon < 4; czlon++) {
        int v = 0, cyfr = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (*s++ - '0');
            cyfr++;
        }
        if (!cyfr || v > 255)
            return -1;
        ip = (ip << 8) | (uint32_t)v;
        if (czlon < 3 && *s++ != '.')
            return -1;
    }
    return *s ? -1 : (int64_t)ip;
}

/* Zaklada potok. Wpisuje dwa numery: fd[0] do czytania, fd[1] do
   pisania. Dopoki proces trzyma oba, potok nie skonczy sie nigdy. */
static inline int64_t sys_pipe(int fd[2])
{
    return syscall64(SYS64_PIPE, (int64_t)(uintptr_t)fd, 0);
}

/* Przepisuje numer deskryptora - tedy przekierowuje sie wejscie
   i wyjscie programu, ktory o niczym nie wie */
static inline int64_t sys_dup2(int stary, int nowy)
{
    return syscall64(SYS64_DUP2, stary, nowy);
}

/* Jeden wpis katalogu. Uklad musi zgadzac sie z tym, co wypelnia
   jadro w obsludze SYS64_READDIR. */
struct dirent64 {
    char name[NAME_MAX64 + 1];
    uint32_t size;
    int is_dir;
};

static inline int64_t sys_mkdir(const char *nazwa)
{
    return syscall64(SYS64_MKDIR, (int64_t)(uintptr_t)nazwa,
                     (int64_t)ustrlen(nazwa));
}

static inline int64_t sys_chdir(const char *nazwa)
{
    return syscall64(SYS64_CHDIR, (int64_t)(uintptr_t)nazwa,
                     (int64_t)ustrlen(nazwa));
}

static inline int64_t sys_getcwd(char *buf, uint64_t len)
{
    return syscall64(SYS64_GETCWD, (int64_t)(uintptr_t)buf, (int64_t)len);
}

/* Kolejne wpisy katalogu roboczego: pytamy o numery po kolei,
   dopoki dostajemy odpowiedz */
static inline int64_t sys_readdir(int index, struct dirent64 *out)
{
    return syscall64(SYS64_READDIR, index, (int64_t)(uintptr_t)out);
}

static inline int64_t sys_unlink(const char *nazwa)
{
    return syscall64(SYS64_UNLINK, (int64_t)(uintptr_t)nazwa,
                     (int64_t)ustrlen(nazwa));
}

/* Przesuwa granice wlasnej sterty i zwraca STARA - czyli adres,
   od ktorego wolno pisac. Zero pyta tylko "gdzie jestem".
   Pamiec robi sie prawdziwa dopiero przy pierwszym dotknieciu, wiec
   prosba o duzo nie kosztuje nic, dopoki sie tego nie uzyje. */
static inline void *sys_sbrk(int64_t delta)
{
    int64_t r = syscall64(SYS64_SBRK, delta, 0);
    return r < 0 ? (void *)0 : (void *)(uintptr_t)r;
}

/* Liczba z napisu; -1 gdy to nie liczba */
static inline int64_t uatoi(const char *s)
{
    int64_t v = 0;
    if (!*s)
        return -1;
    for (; *s; s++) {
        if (*s < '0' || *s > '9')
            return -1;
        v = v * 10 + (*s - '0');
    }
    return v;
}

/* Czeka na koniec dowolnego wlasnego dziecka. Zwraca jego numer
   i wpisuje kod wyjscia; -1 = nie ma na co czekac. */
static inline int64_t sys_wait(int *code)
{
    return syscall64(SYS64_WAIT, (int64_t)(uintptr_t)code, 0);
}

/* Czyta z klawiatury. Blokuje do pierwszego znaku; -1 znaczy, ze
   czekanie przerwal sygnal. Bez echa - echo robi czytajacy. */
static inline int64_t sys_read(int fd, void *buf, uint64_t len)
{
    return syscall64_3(SYS64_READ, fd, (int64_t)(uintptr_t)buf,
                       (int64_t)len);
}

/* Powrot z procedury obslugi sygnalu.
 *
 * Musi lezec w programie, a nie w jadrze: po zakonczeniu procedury
 * trzeba wykonac instrukcje, a jedyne strony wykonywalne, jakie
 * proces ma, to jego wlasny kod. Jadro wsuwa adres tej etykiety pod
 * procedure jako adres powrotu - zwykly ret trafia wiec tutaj,
 * a stad jednym wywolaniem z powrotem do jadra po ramke. */
__asm__(".globl protonos_sigreturn\n"
        "protonos_sigreturn:\n"
        "    movq $10, %rax\n"          /* SYS64_SIGRETURN */
        "    syscall\n");
extern void protonos_sigreturn(void);

/* Ustawia procedure obslugi sygnalow (0 = zabij program, tak jest
   domyslnie). Adres powrotu podaje biblioteka, bo jadro nie ma go
   skad wziac. */
static inline void sys_signal(int sig, void (*handler)(int))
{
    syscall64_3(SYS64_SIGNAL, sig, (int64_t)(uintptr_t)handler,
                (int64_t)(uintptr_t)protonos_sigreturn);
}

static inline int64_t sys_kill(int64_t id, int sig)
{
    return syscall64(SYS64_KILL, id, sig);
}

/* Liczba szesnastkowa - adresy czyta sie tylko tak */
static inline int uhtoa(uint64_t v, char *buf)
{
    static const char digits[] = "0123456789abcdef";
    char tmp[24];
    int n = 0;
    do {
        tmp[n++] = digits[v & 0xF];
        v >>= 4;
    } while (v);
    for (int i = 0; i < n; i++)
        buf[i] = tmp[n - 1 - i];
    buf[n] = 0;
    return n;
}

/* --- Okna pulpitu -------------------------------------------------
 *
 * Program nie rysuje na ekranie: rysuje w buforze swojego okna,
 * a kompozytor jadra sklada z tych buforow klatke. Wspolrzedne sa
 * wzgledem obszaru tresci okna, kolory to indeksy palety pulpitu.
 */
struct win_rect_cmd {
    int32_t x, y, w, h, kolor;
};
struct win_text_cmd {
    int32_t x, y, kolor;
    const char *s;
};

#define WEV_NONE  0
#define WEV_CLICK 1
#define WEV_KEY   2
struct win_event {
    int32_t type;          /* WEV_* */
    int32_t x, y;          /* wspolrzedne wzgledem obszaru tresci */
    int32_t val;           /* maska przyciskow albo kod znaku */
};

/* Czas z zegara CMOS. Uklad pol musi zgadzac sie ze struct rtc_time
   w jadrze - idzie przez wywolanie systemowe jako goly blok bajtow. */
struct czas64 {
    uint16_t rok;
    uint8_t miesiac;
    uint8_t dzien;
    uint8_t godzina;
    uint8_t minuta;
    uint8_t sekunda;
} __attribute__((packed));

static inline int64_t sys_time(struct czas64 *t)
{
    return syscall64(SYS64_TIME, (int64_t)(uintptr_t)t, 0);
}

/* Stan maszyny: ile pamieci jest, ile zajete, jak dlugo dziala.
   Uklad musi zgadzac sie z tym w kernel64/syscall64.c. */
struct sysinfo64 {
    uint32_t ram_kib;
    uint32_t uzyte_kib;
    uint32_t uptime_s;
};

static inline int64_t sys_sysinfo(struct sysinfo64 *out)
{
    return syscall64(SYS64_SYSINFO, (int64_t)(uintptr_t)out, 0);
}

/* Plik widziany jako pamiec.
 *
 * Zwraca adres, pod ktorym widac zawartosc pliku, i wpisuje jego
 * rozmiar. Zadna strona nie powstaje od razu - bajty przychodza
 * z dysku pod palcami, wiec odwzorowanie duzego pliku kosztuje tyle,
 * co spis jego blokow. Tylko do odczytu: proba zapisu konczy sie
 * bledem ochrony, a nie cichym zgubieniem danych. */
static inline const void *sys_mmap_file(const char *nazwa, uint64_t *rozmiar)
{
    int64_t r = syscall64_3(SYS64_MMAP_FILE, (int64_t)(uintptr_t)nazwa,
                            (int64_t)ustrlen(nazwa),
                            (int64_t)(uintptr_t)rozmiar);
    return r <= 0 ? (const void *)0 : (const void *)(uintptr_t)r;
}

/* Pamiec wspoldzielona po kluczu.
 *
 * Ten sam klucz to ta sama pamiec fizyczna - dwa procesy dostaja ja
 * pod swoimi adresami, ale pisza w to samo miejsce. Zwraca adres albo
 * zero, gdy sie nie udalo. */
static inline void *sys_shm(int klucz, uint64_t rozmiar)
{
    int64_t r = syscall64(SYS64_SHM, klucz, (int64_t)rozmiar);
    return r <= 0 ? (void *)0 : (void *)(uintptr_t)r;
}

/* Tworzy okno o obszarze tresci cw x ch. Zwraca numer okna albo -1. */
static inline int64_t win_create(int cw, int ch, const char *tytul)
{
    return syscall64_3(SYS64_WIN_CREATE, cw, ch,
                       (int64_t)(uintptr_t)tytul);
}

static inline void win_rect(int win, int x, int y, int w, int h, int kolor)
{
    struct win_rect_cmd c = {x, y, w, h, kolor};
    syscall64(SYS64_WIN_RECT, win, (int64_t)(uintptr_t)&c);
}

static inline void win_text(int win, int x, int y, const char *s, int kolor)
{
    struct win_text_cmd c = {x, y, kolor, s};
    syscall64(SYS64_WIN_TEXT, win, (int64_t)(uintptr_t)&c);
}

/* Pobiera zdarzenie okna (nie blokuje). 1 = jest, 0 = brak, -1 = blad. */
static inline int64_t win_poll(int win, struct win_event *ev)
{
    return syscall64(SYS64_WIN_POLL, win, (int64_t)(uintptr_t)ev);
}

#endif
