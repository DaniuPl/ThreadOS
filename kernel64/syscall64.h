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
