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
#define SYS_READDIR  14  /* ebx=index, ecx=bufor nazwy -> rozmiar (dir: -2) albo -1 */
#define SYS_UNLINK   15  /* ebx=sciezka -> 0 albo -1 */
#define SYS_TIME     16  /* ebx=bufor struct rtc_time -> 0 */
#define SYS_CHDIR    17  /* ebx=sciezka -> 0 albo -1 */
#define SYS_GETCWD   18  /* ebx=bufor (>=128) -> 0 */
#define SYS_MKDIR    19  /* ebx=sciezka -> 0 albo -1 */
#define SYS_KILL     20  /* ebx=pid -> 0 albo -1 */
#define SYS_WIN_CREATE 21 /* ebx=cw, ecx=ch, edx=tytul -> winid albo -1 */
#define SYS_WIN_RECT   22 /* ebx=winid, ecx=&{x,y,w,h,color} -> 0/-1 */
#define SYS_WIN_TEXT   23 /* ebx=winid, ecx=&{x,y,color,str} -> 0/-1 */
#define SYS_WIN_POLL   24 /* ebx=winid, ecx=&event -> 1 (jest), 0 (brak), -1 */
#define SYS_MMAP       25 /* ebx=rozmiar -> adres albo 0 */
#define SYS_SHM        26 /* ebx=klucz, ecx=rozmiar -> adres wspoldzielony */

#define SYSCALL_VECTOR 0x80

#ifndef __ASSEMBLER__
struct regs;
void syscall_handler(struct regs *r);
#endif

#endif
