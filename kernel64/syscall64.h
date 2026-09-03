#ifndef PROTONOS_SYSCALL64_H
#define PROTONOS_SYSCALL64_H

#include <stdint.h>

/* Call numbers - they have to match user64/usys64.h */
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

/* Network. Socket numbers are numbers in the kernel's tables, not
   descriptors - the network and files live separately for now. */
#define SYS64_NETINFO     23
#define SYS64_UDP_OPEN    24
#define SYS64_UDP_SEND    25
#define SYS64_UDP_RECV    26
#define SYS64_UDP_CLOSE   27
#define SYS64_TCP_CONNECT 28
#define SYS64_TCP_SEND    29
#define SYS64_TCP_RECV    30
#define SYS64_TCP_CLOSE   31

/* Desktop windows. The compositor is the same as in the 32-bit
   kernel - these calls are only the way to it from ring 3. */
#define SYS64_WIN_CREATE 32  /* a1=cw, a2=ch, a3=title -> winid or -1 */
#define SYS64_WIN_RECT   33  /* a1=winid, a2=&{x,y,w,h,color} -> 0/-1 */
#define SYS64_WIN_TEXT   34  /* a1=winid, a2=&{x,y,color,text} -> 0/-1 */
#define SYS64_WIN_POLL   35  /* a1=winid, a2=&event -> 1/0/-1 */

/* Shared memory by key: a1=key, a2=size -> address or 0 */
#define SYS64_SHM        36

/* The time from the CMOS clock: a1=&struct rtc_time -> 0 or -1 */
#define SYS64_TIME       37

/* Machine state for a program: a1=&{ram_kib, used_kib, uptime_s} */
#define SYS64_SYSINFO    38

/* Our own task number */
#define SYS64_GETPID     39

/* A file mapped into memory: a1=name, a2=name length,
   a3=&size, a4=write -> address or 0 */
#define SYS64_MMAP_FILE  40

/* Flushing the dirty pages of writable mappings -> how many pages */
#define SYS64_MSYNC      41

/* The number of the core the program is running on right now */
#define SYS64_CORE      42

/* Fresh, zeroed memory in the mapping area: a1=size -> address or 0.
   Only the addresses are reserved; the pages appear on first touch. */
#define SYS64_MMAP      43

/* A thread of this process (the same memory): a1=entry, a2=argument
   -> its task number or -1 */
#define SYS64_THREAD    44

/* Who is at the machine (see kernel/include/user.h) */
#define SYS64_AUTH      45   /* a1=password, a2=length -> 0 / -2 */
#define SYS64_DROP      46   /* back to a plain user; always succeeds */
#define SYS64_LEVEL     47   /* -> 0 = admin, 1 = a plain user */

/* A wait that does not wait: a1=pid, a2=&code -> 0 (ended, the code is
   written) or -3 (still running) */
#define SYS64_TRYWAIT   48

/* Where in the file the next read starts: a1=fd, a2=position */
#define SYS64_SEEK      49

/* The clipboard: a1=text, a2=length -> taken; a1=buffer, a2=size -> length */
#define SYS64_CLIP_SET  50
#define SYS64_CLIP_GET  51
#define SYS64_WIN_KEEP  52  /* a1=winid -> "not closing yet" */
#define SYS64_WIN_TITLE 54
#define SYS64_RENAME   55  /* a1/a2 = from, a3/a4 = to */  /* a1=winid, a2=text */
#define SYS64_PS        53  /* a1=index, a2=buffer -> 0 / -1 */

/* Numbers taken since forever: keyboard, console, errors */
#define FD_STDIN   0
#define FD_STDOUT  1

/* Entry into the kernel through the syscall instruction (instead of an
   interrupt gate) */
void syscall64_init(void);

/* The kernel stack for calls from ring 3 - set before entering ring 3 */
void syscall64_set_kernel_stack(uint64_t rsp);

/* A jump to ring 3; does not return. The program finds 'arg' in the
   RDI register. */
void enter_ring3_64(uint64_t rip, uint64_t rsp,
                    uint64_t argc, uint64_t argv);

/* What a program managed to tell the kernel - to be checked after it
   ends */
int syscall64_user_ran(void);
uint32_t syscall64_user_cs(void);
int syscall64_user_exited(void);
int syscall64_user_status(void);
int syscall64_user_exits(void);      /* how many programs ended by themselves */
int syscall64_user_failures(void);   /* how many of them with a code other than zero */
void syscall64_forget_user(void);

#endif
