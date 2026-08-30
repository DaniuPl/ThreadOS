#ifndef PROTONOS_SYSCALL64_H
#define PROTONOS_SYSCALL64_H

#include <stdint.h>

/* Numery wywolan - musza zgadzac sie z user64/usys64.h */
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

/* Siec. Numery gniazd sa numerami w tablicach jadra, nie
   deskryptorami - siec i pliki zyja na razie osobno. */
#define SYS64_NETINFO     23
#define SYS64_UDP_OPEN    24
#define SYS64_UDP_SEND    25
#define SYS64_UDP_RECV    26
#define SYS64_UDP_CLOSE   27
#define SYS64_TCP_CONNECT 28
#define SYS64_TCP_SEND    29
#define SYS64_TCP_RECV    30
#define SYS64_TCP_CLOSE   31

/* Okna pulpitu. Kompozytor jest ten sam, co w jadrze 32-bitowym -
   te wywolania sa tylko droga do niego z pierscienia 3. */
#define SYS64_WIN_CREATE 32  /* a1=cw, a2=ch, a3=tytul -> winid albo -1 */
#define SYS64_WIN_RECT   33  /* a1=winid, a2=&{x,y,w,h,kolor} -> 0/-1 */
#define SYS64_WIN_TEXT   34  /* a1=winid, a2=&{x,y,kolor,napis} -> 0/-1 */
#define SYS64_WIN_POLL   35  /* a1=winid, a2=&zdarzenie -> 1/0/-1 */

/* Pamiec wspoldzielona po kluczu: a1=klucz, a2=rozmiar -> adres albo 0 */
#define SYS64_SHM        36

/* Czas z zegara CMOS: a1=&struct czas -> 0 albo -1 */
#define SYS64_TIME       37

/* Stan maszyny dla programu: a1=&{ram_kib, uzyte_kib, uptime_s} */
#define SYS64_SYSINFO    38

/* Numer wlasnego zadania */
#define SYS64_GETPID     39

/* Plik odwzorowany w pamieci: a1=nazwa, a2=dlugosc nazwy,
   a3=&rozmiar, a4=zapis -> adres albo 0 */
#define SYS64_MMAP_FILE  40

/* Odeslanie brudnych stron zapisywalnych odwzorowan -> ile stron */
#define SYS64_MSYNC      41

/* Numer rdzenia, na ktorym program wlasnie biegnie */
#define SYS64_RDZEN      42

/* Numery zajete od zawsze: klawiatura, konsola, bledy */
#define FD_STDIN   0
#define FD_STDOUT  1

/* Wejscie do jadra przez instrukcje syscall (zamiast bramki przerwania) */
void syscall64_init(void);

/* Stos jadra dla wywolan z ring 3 - ustawiany przed wejsciem w ring 3 */
void syscall64_set_kernel_stack(uint64_t rsp);

/* Skok do ring 3; nie wraca. 'arg' program zastaje w rejestrze RDI. */
void enter_ring3_64(uint64_t rip, uint64_t rsp,
                    uint64_t argc, uint64_t argv);

/* Co program zdazyl powiedziec jadru - do sprawdzenia po jego koncu */
int syscall64_user_ran(void);
uint32_t syscall64_user_cs(void);
int syscall64_user_exited(void);
int syscall64_user_status(void);
int syscall64_user_exits(void);      /* ile programow skonczylo sie samo */
int syscall64_user_failures(void);   /* ile z nich z kodem innym niz zero */
void syscall64_forget_user(void);

#endif
