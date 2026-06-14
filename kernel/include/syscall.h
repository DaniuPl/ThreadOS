#ifndef PROTONOS_SYSCALL_H
#define PROTONOS_SYSCALL_H

/* Numery syscalli (w eax przy int 0x80; argumenty: ebx, ecx, edx;
   wynik wraca w eax) */
#define SYS_WRITE    1   /* ebx=fd, ecx=bufor, edx=len -> zapisane bajty */
#define SYS_EXIT     2   /* ebx=kod wyjscia */
#define SYS_SLEEP    3   /* ebx=milisekundy */
#define SYS_GETTICKS 4   /* -> tykniecia timera od startu */
#define SYS_READ     5   /* ebx=fd, ecx=bufor, edx=max -> bajty (blokuje fd 0) */
#define SYS_SPAWN    6   /* ebx=linia polecen, ecx=plik na stdout (0=brak) -> pid */
#define SYS_WAIT     7   /* ebx=pid - blokuje az proces sie skonczy */
#define SYS_GETPID   8   /* -> pid biezacego procesu */
#define SYS_SBRK     9   /* ebx=increment -> stary brk albo -1 */
#define SYS_OPEN     10  /* ebx=nazwa, ecx=flagi O_* -> fd albo -1 */
#define SYS_CLOSE    11  /* ebx=fd -> 0 albo -1 */
#define SYS_SEEK     12  /* ebx=fd, ecx=offset -> nowy offset albo -1 */
#define SYS_PIPELINE 13  /* ebx=linia z '|', ecx=plik na stdout -> pid ostatniego */
#define SYS_READDIR  14  /* ebx=slot, ecx=bufor nazwy -> rozmiar albo -1 */
#define SYS_UNLINK   15  /* ebx=nazwa -> 0 albo -1 */
#define SYS_TIME     16  /* ebx=bufor struct rtc_time -> 0 */

#define SYSCALL_VECTOR 0x80

#ifndef __ASSEMBLER__
struct regs;
void syscall_handler(struct regs *r);
#endif

#endif
