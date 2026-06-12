#ifndef PROTONOS_SYSCALL_H
#define PROTONOS_SYSCALL_H

/* Numery syscalli (w eax przy int 0x80; argumenty: ebx, ecx, edx;
   wynik wraca w eax) */
#define SYS_WRITE    1   /* ebx=wskaznik, ecx=dlugosc -> liczba znakow */
#define SYS_EXIT     2   /* ebx=kod wyjscia */
#define SYS_SLEEP    3   /* ebx=milisekundy */
#define SYS_GETTICKS 4   /* -> tykniecia timera od startu */

#define SYSCALL_VECTOR 0x80

#ifndef __ASSEMBLER__
struct regs;
void syscall_handler(struct regs *r);
#endif

#endif
