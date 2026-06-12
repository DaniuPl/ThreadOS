#ifndef PROTONOS_SYSCALL_H
#define PROTONOS_SYSCALL_H

/* Numery syscalli (w eax przy int 0x80; argumenty: ebx, ecx, edx;
   wynik wraca w eax) */
#define SYS_WRITE    1   /* ebx=wskaznik, ecx=dlugosc -> liczba znakow */
#define SYS_EXIT     2   /* ebx=kod wyjscia */
#define SYS_SLEEP    3   /* ebx=milisekundy */
#define SYS_GETTICKS 4   /* -> tykniecia timera od startu */
#define SYS_READ     5   /* ebx=bufor, ecx=max -> liczba znakow (blokuje) */
#define SYS_SPAWN    6   /* ebx=nazwa programu z initrd -> pid albo -1 */
#define SYS_WAIT     7   /* ebx=pid - blokuje az proces sie skonczy */
#define SYS_GETPID   8   /* -> pid biezacego procesu */

#define SYSCALL_VECTOR 0x80

#ifndef __ASSEMBLER__
struct regs;
void syscall_handler(struct regs *r);
#endif

#endif
