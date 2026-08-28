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

static inline int64_t syscall64(int64_t nr, int64_t a1, int64_t a2)
{
    int64_t ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr), "D"(a1), "S"(a2)
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

static inline void print(const char *s)
{
    syscall64(SYS64_WRITE, (int64_t)(uintptr_t)s, (int64_t)ustrlen(s));
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
   wraca - od nastepnej instrukcji dziala juz tamten program. */
static inline int64_t sys_exec(const char *name)
{
    return syscall64(SYS64_EXEC, (int64_t)(uintptr_t)name,
                     (int64_t)ustrlen(name));
}

/* Czeka na koniec dowolnego wlasnego dziecka. Zwraca jego numer
   i wpisuje kod wyjscia; -1 = nie ma na co czekac. */
static inline int64_t sys_wait(int *code)
{
    return syscall64(SYS64_WAIT, (int64_t)(uintptr_t)code, 0);
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

#endif
