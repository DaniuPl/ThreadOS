#ifndef THREADOS_SYSCALL_H
#define THREADOS_SYSCALL_H

/* Syscall numbers (in eax for int 0x80; arguments: ebx, ecx, edx;
   the result comes back in eax) */
#define SYS_WRITE    1   /* ebx=fd, ecx=buffer, edx=len -> bytes written */
#define SYS_EXIT     2   /* ebx=exit code */
#define SYS_SLEEP    3   /* ebx=milliseconds */
#define SYS_GETTICKS 4   /* -> timer ticks since startup */
#define SYS_READ     5   /* ebx=fd, ecx=buffer, edx=max -> bytes (blocks on fd 0) */
#define SYS_SPAWN    6   /* ebx=command line, ecx=file for stdout (0=none) -> pid */
#define SYS_WAIT     7   /* ebx=pid - blocks until the process ends */
#define SYS_GETPID   8   /* -> pid of the current process */
#define SYS_SBRK     9   /* ebx=increment -> old brk or -1 */
#define SYS_OPEN     10  /* ebx=name, ecx=O_* flags -> fd or -1 */
#define SYS_CLOSE    11  /* ebx=fd -> 0 or -1 */
#define SYS_SEEK     12  /* ebx=fd, ecx=offset -> new offset or -1 */
#define SYS_PIPELINE 13  /* ebx=line with '|', ecx=file for stdout -> pid of the last stage */
#define SYS_READDIR  14  /* ebx=index, ecx=name buffer -> size (dir: -2) or -1 */
#define SYS_UNLINK   15  /* ebx=path -> 0 or -1 */
#define SYS_TIME     16  /* ebx=struct rtc_time buffer -> 0 */
#define SYS_CHDIR    17  /* ebx=path -> 0 or -1 */
#define SYS_GETCWD   18  /* ebx=buffer (>=128) -> 0 */
#define SYS_MKDIR    19  /* ebx=path -> 0 or -1 */
#define SYS_KILL     20  /* ebx=pid, ecx=signal (0=SIGKILL) -> 0 or -1 */
#define SYS_WIN_CREATE 21 /* ebx=cw, ecx=ch, edx=title -> winid or -1 */
#define SYS_WIN_RECT   22 /* ebx=winid, ecx=&{x,y,w,h,color} -> 0/-1 */
#define SYS_WIN_TEXT   23 /* ebx=winid, ecx=&{x,y,color,str} -> 0/-1 */
#define SYS_WIN_POLL   24 /* ebx=winid, ecx=&event -> 1 (got one), 0 (none), -1 */
#define SYS_MMAP       25 /* ebx=size -> address or 0 */
#define SYS_SHM        26 /* ebx=key, ecx=size -> shared address */
#define SYS_SYSINFO    27 /* ebx=buffer {ram_kib,used_kib,uptime_s} -> 0 */
#define SYS_THREAD     28 /* ebx=entry, ecx=arg -> thread tid or -1 */
#define SYS_SETPRIO    29 /* ebx=level (0/1/2) -> 0 */
#define SYS_UDP_OPEN   30 /* ebx=port (0=ephemeral) -> socket or -1 */
#define SYS_UDP_SEND   31 /* ebx=socket, ecx=&{ip,port,buf,len} -> bytes/-1 */
#define SYS_UDP_RECV   32 /* ebx=socket, ecx=&{buf,max,ms,ip,port} -> bytes/-1 */
#define SYS_UDP_CLOSE  33 /* ebx=socket -> 0 */
#define SYS_NETINFO    34 /* ebx=&{ip,mask,gw,dns} -> 0 or -1 (no network) */
#define SYS_TCP_CONNECT 35 /* ebx=ip, ecx=port, edx=timeout_ms -> connection/-1 */
#define SYS_TCP_LISTEN  36 /* ebx=port -> listening socket or -1 */
#define SYS_TCP_ACCEPT  37 /* ebx=socket, ecx=timeout_ms -> connection/-1 */
#define SYS_TCP_SEND    38 /* ebx=connection, ecx=&{buf,len,timeout} -> bytes/-1 */
#define SYS_TCP_RECV    39 /* ebx=connection, ecx=&{buf,max,timeout} -> bytes/0/-1 */
#define SYS_TCP_CLOSE   40 /* ebx=connection -> 0 */
#define SYS_FORK        41 /* -> child pid (parent) / 0 (child) / -1 */
#define SYS_EXEC        42 /* ebx=command line; never returns, or -1 */
#define SYS_WAITANY     43 /* ebx=&code -> child pid or -1 (no children) */
#define SYS_SIGNAL      44 /* ebx=signal, ecx=handler, edx=trampoline -> 0/-1 */
#define SYS_SIGRETURN   45 /* return from a signal handler (no arguments) */
#define SYS_MMAP_FILE   46 /* ebx=name, ecx=&size, edx=writable -> address or 0 */
#define SYS_MSYNC       47 /* writes back dirty pages of mappings -> how many pages */
#define SYS_CORE       48 /* number of the core the program is running on right now */

/* System settings. The i-th one by number (name, value, description
   into the program's buffer), and a change by name. What the machine
   remembers about itself lives in the kernel, so a program can only
   ask about it - and that is the whole point: one place decides. */
#define SYS_CFG_GET    49 /* ebx=index, ecx=&struct cfg_entry -> 0/-1 */
#define SYS_CFG_SET    50 /* ebx=name, ecx=value -> 0/-1 */
#define SYS_AUTH       51 /* ebx=password -> 0 = admin from now on, -2 = no */
#define SYS_DROP       52 /* back to a plain user; always succeeds */
#define SYS_LEVEL      53 /* -> 0 = admin, 1 = a plain user */
/* A wait that does not wait: ebx=pid, ecx=&code.
   0 = it has ended (the code is written through ecx), -3 = still
   running. Without it a program watching another one has to choose
   between blocking and not knowing. */
#define SYS_TRYWAIT    54
/* Starts a program and keeps a way to TALK to it: ebx=command line,
   ecx=file for its stdout, edx=&fd. The program's stdin is a pipe
   whose write end comes back as a descriptor of ours - closing it is
   how the program is told there will be no more input. -> pid */
#define SYS_SPAWN_TALK 55
#define SYS_USER       78  /* ebx=&struct user_ctl -> account operation */
/* The clipboard: ebx=text, ecx=length -> how much was taken;
   and ebx=buffer, ecx=size -> how much is there */
#define SYS_CLIP_SET   56
#define SYS_CLIP_GET   57
/* "I heard the close request and I am not closing yet" */
#define SYS_WIN_KEEP   58
/* One row of the task list: ebx=index, ecx=buffer -> 0 or -1 */
#define SYS_PS         59
/* The window's name: ebx=winid, ecx=text */
#define SYS_WIN_TITLE  60
/* A new name for something that exists: ebx=from, ecx=to -> 0 or -1 */
#define SYS_RENAME     61
/* A block of pixels into a window: ebx=winid, ecx=&struct win_blit_cmd */
#define SYS_WIN_BLIT   62
/* The screen's palette, so a program can turn colours into numbers:
   ebx=buffer for 256 entries of 0x00RRGGBB */
#define SYS_PALETTE    63
/* "My pixels are colours from now on": ebx=winid -> 0 or -1 */
#define SYS_WIN_TRUECOL 64
/* A block of colours into a window: ebx=winid, ecx=&struct win_blit_cmd */
#define SYS_WIN_BLIT32 65
/* Installing onto this machine's own disk. Looking changes nothing and
   is allowed to anybody; every step of the writing needs an admin,
   because what is on the other side of it is somebody's disk.
   ebx=way (0 = alongside, 1 = the whole disk), ecx=&struct install_info,
   edx=which disk (from zero; -1 = the one in use)
   -> 1 = it can be done, 0 = it cannot */
#define SYS_INSTALL_LOOK 66
/* ebx=step number -> 0 done, 1 nothing to do, -1 failed, -2 not allowed */
#define SYS_INSTALL_STEP 67
/* Sound: how loud it is, how loud it should be, and a tone.
   ebx=&struct snd_info | ebx=volume (-1 keeps), ecx=mute (-1 keeps)
   | ebx=hertz, ecx=milliseconds */
#define SYS_SND_GET  68
#define SYS_SND_SET  69
#define SYS_SND_BEEP 70
#define SYS_SND_STREAM 79
#define SND_STREAM_BEGIN 0
#define SND_STREAM_WRITE 1
#define SND_STREAM_END 2
/* Counting semaphores, shared by key (kernel/sem.c) */
#define SYS_SEM_GET  71  /* ebx=key, ecx=initial count -> id or -1 */
#define SYS_SEM_WAIT 72  /* ebx=id -> 0, or -1 (a bad id, being killed) */
#define SYS_SEM_POST 73  /* ebx=id -> 0 or -1 */
/* Drawing as the desktop draws - shapes and TrueType text (see
   desktop.h, struct win_draw): ebx=winid, ecx=&cmd */
#define SYS_WIN_DRAW 74
/* The theme's colours for a program's window: ebx=buffer of 11 words,
   ecx=which theme (0 the one in use, 1 dark, 2 light) */
#define SYS_UI_LOOK  75
/* A card at the top right of the screen: ebx=title, ecx=text */
#define SYS_NOTIFY   76
/* A background as a small picture: ebx=&struct bg_preview_cmd */
#define SYS_BG_PREVIEW 77

/* One setting as the program sees it. The layout has to match struct
   cfg_entry in user/usys.h. */
#define CFG_NAME_MAX 32
#define CFG_VAL_MAX  32
#define CFG_HELP_MAX 80


struct cfg_entry {
    char name[CFG_NAME_MAX];
    char value[CFG_VAL_MAX];
    char help[CFG_HELP_MAX];
};

#define SYSCALL_VECTOR 0x80

#ifndef __ASSEMBLER__
#include <stdint.h>
struct regs;
void syscall_handler(struct regs *r);
/* May this call be made without the big kernel lock */
int syscall_lockless(uint32_t nr);
#endif

#endif
