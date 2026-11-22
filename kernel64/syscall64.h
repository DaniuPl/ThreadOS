#ifndef THREADOS_SYSCALL64_H
#define THREADOS_SYSCALL64_H

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
#define SYS64_OPEN      13  /* a1/a2 = name, a3 = OPEN64_* flags */
#define OPEN64_CREATE   1
#define OPEN64_TRUNC    2   /* empty it on opening */
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
/* Desktop windows. The compositor is kernel/desktop.c - these calls
   are only the way to it from ring 3. */
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
#define SYS64_RENAME   55
#define SYS64_WIN_BLIT 56
#define SYS64_PALETTE  57  /* a1 = buffer for 256 colours */  /* a1=winid, a2=&cmd */  /* a1/a2 = from, a3/a4 = to */  /* a1=winid, a2=text */
#define SYS64_PS        53  /* a1=index, a2=buffer -> 0 / -1 */
#define SYS64_WIN_TRUECOL 58  /* a1=winid -> 0 / -1 */
#define SYS64_WIN_BLIT32  59  /* a1=winid, a2=&cmd */
/* Installing onto this machine's own disk. Looking changes nothing and
   is allowed to anybody; writing needs an admin, because what is on
   the other side of it is somebody's disk.
   a1=way (0 = alongside, 1 = the whole disk), a2=&struct install_info,
   a3=which disk (from zero; -1 = the one in use)
   -> 1 = it can be done, 0 = it cannot */
#define SYS64_INSTALL_LOOK 60
/* a1=step -> 0 done, 1 nothing to do, -1 failed, -2 not allowed */
#define SYS64_INSTALL_STEP 61
/* Sound: a1=&struct snd_info | a1=volume (-1 keeps), a2=mute (-1 keeps)
   | a1=hertz, a2=milliseconds */
#define SYS64_SND_GET  62
#define SYS64_SND_SET  63
#define SYS64_SND_BEEP 64
#define SYS64_SND_STREAM 79
#define SND_STREAM_BEGIN 0
#define SND_STREAM_WRITE 1
#define SND_STREAM_END 2
/* Counting semaphores, shared by key (kernel/sem.c) */
#define SYS64_SEM_GET  65   /* rdi=key, rsi=initial count -> id or -1 */
#define SYS64_SEM_WAIT 66   /* rdi=id -> 0, or -1 (a bad id, a signal) */
#define SYS64_SEM_POST 67   /* rdi=id -> 0 or -1 */
/* Shapes and TrueType text in a window (kernel/include/desktop.h):
   a1=winid, a2=&cmd */
#define SYS64_WIN_DRAW 68
/* The theme's colours for a program's window: a1=buffer of 11 words,
   a2=which theme (0 the one in use, 1 dark, 2 light) */
#define SYS64_UI_LOOK  69
/* A card at the top right of the screen: a1=title, a2=text */
#define SYS64_NOTIFY   70
/* A background as a small picture: a1=&cmd {bg, dark, w, h, out} */
#define SYS64_BG_PREVIEW 71
/* The settings (kernel/config.c): a1=index, a2=&struct cfg_entry ->
   0/-1; a1=name, a2=value -> 0, -1, or -2 for a plain user */
#define SYS64_CFG_GET  72
#define SYS64_CFG_SET  73
/* What stands behind a descriptor: a1=fd, a2=&{int32 kind; uint32 size;
   uint32 pos} -> 0 / -1. kind is FDK_* from task64.h; for a file size
   and pos are filled in. A C library needs it to tell a console from a
   file (isatty, buffering) and for fstat and lseek(SEEK_END). */
#define SYS64_FDINFO   74
/* a1 = the new FS base (must be a program address) -> 0 / -1 */
#define SYS64_SET_FS   75
/* A file of the initrd, read: a1/a2 = name, a3 = buffer, a4 = its size
   -> how many bytes (the file may be longer), -1 = no such file */
#define SYS64_INITRD_READ 76
/* Update a system file (admin only): a1 = 0 kernel / 1 initrd / 2 loader,
   a2 = data, a3 = length. The previous system is backed up before the
   first write. */
#define SYS64_UPDATE_WRITE 77
/* Whether a trusted installed-system boot marker permits update writes */
#define SYS64_UPDATE_ALLOWED 80
#define SYS64_PCI_INFO 81 /* a1=index, a2=&struct pci_info -> 1/0/-1 */
#define SYS64_UPDATE_ROLLBACK 82 /* restore the previous installed system */
/* Bluetooth: a1 = 0 fills *(struct bt_info *)a2, a1 = 1 starts a search
   in the background -> 0 / -1 */
#define SYS64_BT 99
/* Wi-Fi: a1 = 0 fills *(struct wifi_info *)a2, a1 = 1 starts a scan in
   the background -> 0 / -1 */
#define SYS64_WIFI 100
/* The Device Manager: device a1 of the machine into a2 (struct
   device_info); 1 = there was one, 0 = past the end */
#define SYS64_DEVICE 101
/* The camera: a1 = CAM_INFO, CAM_OPEN, CAM_FRAME, CAM_CLOSE
   (kernel/include/cam_info.h) */
#define SYS64_CAMERA 102
/* The microphone: a1 = MIC_INFO (a2 = struct mic_info *), MIC_OPEN,
   MIC_READ (a2 = buffer, a3 = frames: 16-bit stereo, 48 kHz),
   MIC_CLOSE */
#define SYS64_MIC 103
/* Pages given back: (address, length) - they read as zeros afterwards.
   What madvise(MADV_DONTNEED) and munmap do on Linux. The answer is the
   count of pages it could not drop (shared with another space), which
   the program has to clear itself; -1 for a range not its own. */
#define SYS64_DISCARD 104
/* Unix socket pairs and memfds (kernel/include/ipc.h): (op, a, b, c) */
#define SYS64_IPC 105
/* FD_CLOEXEC of a descriptor: (fd, value) - value -1 asks, 0/1 sets;
   returns the flag, -1 for no such descriptor */
#define SYS64_FDCTL 106
/* The application packages the loader brought (kernel/include/initrd.h):
   SYS64_PACKAGE (index, &struct tapp_info) -> 0, -1 = no such package;
   SYS64_PACKAGE_READ (index, offset, buf, len) -> bytes copied, -1.
   Reading the whole package is how 'tapp verify' checks its signature. */
#define SYS64_PACKAGE      107
#define SYS64_PACKAGE_READ 108
/* SYS64_PACKAGE_INSTALL (buf, len): a whole package written onto the
   installed system and taken (kernel/install.c install_package) - an
   administrator's only: -2 for anybody else */
#define SYS64_PACKAGE_INSTALL 109
/* A thread's signal mask (bit n-1 for signal n), on both kernels:
   SYS64_SIGMASK (how: 0 block, 1 unblock, 2 set, 3 only ask; the bits;
   where to put the old mask, or 0) -> 0, -1. SYS64_SIGSUSPEND (mask):
   that mask until a signal it lets through is handled, then the old one
   again; -1 when it was. SYS64_KILL takes a third argument: 1 says the
   number is a thread's (tkill), 0 a program's (kill). A handler gets
   Linux's frame: the signal, its siginfo_t and ucontext_t, and returns
   through SYS64_SIGRETURN. */
#define SYS64_SIGMASK    110
#define SYS64_SIGSUSPEND 111
/* Pixels at the panel's own resolution, into a window whose logical
   pixels are larger (WEV_SIZE's val gives the scale): a1=winid,
   a2=&{x, y, w, h, pixels} in the panel's pixels from the window's
   corner. At one to one the same as SYS64_WIN_BLIT32. */
#define SYS64_WIN_BLITHI 112
/* A program waits for typing (a1 = winid, a2 = 1) or no longer does
   (a2 = 0): a touchscreen's shell shows its keyboard. The PC's desktop
   has one of wood and plastic, and answers 0. */
#define SYS64_WIN_KEYBOARD 113
/* a1=win, a2=1 the whole screen (a page's video full screen), 0 back
   -> 0, or -1 */
#define SYS64_WIN_FULL 114
/* a1=&struct now_playing (user64/usys64.h): what the program plays, for
   the island's play and pause, which come back as WEV_MEDIA -> 0/-1 */
#define SYS64_NOW_PLAYING 115
/* a1=&struct note64 (user64/usys64.h): a notification with a weight
   (NOTE_* in kernel/include/desktop.h) and up to two actions, which come
   back to the window as WEV_NOTE -> its number (> 0), or -1. SYS64_NOTIFY
   stays as it was, a Normal note with no actions: programs built before
   this pass whatever was in the third register. */
#define SYS64_NOTE     131
/* a1=&struct shuttle64: what the program has going on - a timer, a
   download, a call - on the shuttle; kind SHUTTLE_END takes it away
   -> 0, or -1 */
#define SYS64_SHUTTLE  132
/* a1=win, a2=the theme chosen (0 dark, 1 light) or -1 for none: the
   theme bar in the window's title bar, for a program started with
   --theme-bar; a click comes back as WEV_THEME -> 0, or -1 (and where
   windows have no title bar, as on the phone: the program shows the
   choice itself) */
#define SYS64_WIN_THEMEBAR 133
/* A volume other than the system's - a stick's FAT (kernel/vol.c): a1 =
   VOL64_*, the rest as each says (user64/usys64.h's vol_* wrappers).
   Paths on the volume are absolute ("/MEND/days"), at most 255 bytes. */
#define SYS64_VOLUME 134
#define VOL64_LIST   0   /* a2=&struct vol_info[], a3=max -> how many */
#define VOL64_MOUNT  1   /* a2=disk, a3=lba -> 0/-1 */
#define VOL64_UMOUNT 2
#define VOL64_STAT   3   /* a2=path, a3=&struct vol_entry -> 1/0/-1 */
#define VOL64_DIR    4   /* a2=path, a3=index, a4=&struct vol_entry -> 1/0/-1 */
#define VOL64_READ   5   /* a2=path, a3=&{off, buf, len} -> bytes/-1 */
#define VOL64_WRITE  6   /* a2=path, a3=buf, a4=len -> 0/-1 (a whole file) */
#define VOL64_MKDIR  7   /* a2=path -> 0/-1 */
#define VOL64_UNLINK 8   /* a2=path -> 0/-1 */

/* --- Files with no 63-byte or 4 GiB ceilings (docs/tfs4-plan.md) ---
 * The calls before these keep their shapes for programs already built;
 * these are what a program built now uses. Paths are a1 = pointer,
 * a2 = length, up to FS_PATH_MAX - 1 bytes, relative to the working
 * directory.
 *
 * SYS64_READDIR2  a1/a2 = a directory's path, a3 = &uint64_t cursor (0
 *                 to start; moved past the entry), a4 = &struct
 *                 dirent2_64 -> 1 an entry / 0 the end / -1. The
 *                 working directory stays where it is.
 * SYS64_STAT      a1/a2 = a path, a3 = &struct stat64k -> 0 / -1
 * SYS64_FDINFO2   a1 = fd, a2 = &struct fdinfo2_64 -> 0 / -1:
 *                 SYS64_FDINFO with a 64-bit size and position
 *
 * The structures, the same in user64/usys64.h and the C library's
 * libc/overlay/src/threados/tos.h: */
#define SYS64_READDIR2 116
#define SYS64_STAT     117
#define SYS64_FDINFO2  118

/* --- POSIX's file metadata (docs/tfs4-plan.md, stage 4) ---
 * These answer -errno (Linux's numbers) rather than -1. A path is a
 * pointer and a length; where a call also takes a descriptor, a length
 * of 0 says a1 is one.
 *
 * SYS64_STAT      a4 = flags: STAT64_NOFOLLOW describes a symbolic link
 *                 at the end rather than what it points at
 * SYS64_OPEN2     a1/a2 = path, a3 = OPEN64_* (+ OPEN64_EXCL), a4 = the
 *                 permission bits a new file gets (less the umask)
 *                 -> a descriptor
 * SYS64_MKDIR2    a1/a2 = path, a3 = permission bits
 * SYS64_FDSTAT    a1 = fd, a2 = &struct stat64k
 * SYS64_CHMOD     a1/a2 = path (or fd), a3 = bits
 * SYS64_CHOWN     a1/a2 = path (or fd), a3 = uid, a4 = gid (-1 keeps)
 * SYS64_UTIMES    a1/a2 = path (or fd), a3 = &int64_t[2], access and
 *                 modification, each seconds, UTIME64_NOW or
 *                 UTIME64_OMIT
 * SYS64_LINK      a1/a2 = an existing file, a3/a4 = its new name
 * SYS64_SYMLINK   a1/a2 = the target, a3/a4 = the link's path
 * SYS64_READLINK  a1/a2 = path, a3 = buffer, a4 = its size -> length
 * SYS64_ACCESS    a1/a2 = path, a3 = 4 read | 2 write | 1 run, 0 exists
 * SYS64_UMASK     a1 = the new mask, or -1 to ask -> the old one
 * SYS64_FSERRNO   -> why this task's last file call failed (an errno) */
#define SYS64_OPEN2    119
#define SYS64_MKDIR2   120
#define SYS64_FDSTAT   121
#define SYS64_CHMOD    122
#define SYS64_CHOWN    123
#define SYS64_UTIMES   124
#define SYS64_LINK     125
#define SYS64_SYMLINK  126
#define SYS64_READLINK 127
#define SYS64_ACCESS   128
#define SYS64_UMASK    129
#define SYS64_FSERRNO  130

#define STAT64_NOFOLLOW 1
#define OPEN64_EXCL     4
#define UTIME64_NOW     (-1)
#define UTIME64_OMIT    (-2)

#define DT2_FILE 1
#define DT2_DIR  2
#define DT2_LINK 3
struct dirent2_64 {
    uint64_t size;
    uint64_t ino;                /* the same for every name of a file */
    uint32_t type;               /* DT2_* */
    uint32_t name_len;
    char name[256];              /* with its NUL */
};

/* What a stat call gives a program. mode, gid and the times are 0
   until the file system keeps them (TFS4); the C library supplies what
   it has to then. */
struct stat64k {
    uint32_t type;               /* DT2_* */
    uint32_t mode;               /* permission bits, or 0: not kept */
    uint64_t size;
    uint64_t ino;
    uint32_t uid, gid;
    uint32_t nlink, blksize;
    int64_t atime, mtime, ctime; /* seconds since 1970, or 0 */
    uint64_t blocks;             /* of 512 bytes */
};

struct fdinfo2_64 {
    int32_t kind;                /* FDK_* */
    uint32_t pad;
    uint64_t size, pos;          /* as SYS64_FDINFO's */
    uint64_t ino;                /* a file's, as SYS64_STAT gives it */
};
#define SYS64_USER       78   /* a1=&struct user_ctl -> account operation */

/* --- Threads the way a C library makes them (see kernel64/futex64.h) ---
 *
 * SYS64_CLONE      a1=&struct clone64 (proc64.h) -> the thread's number / -1
 * SYS64_FUTEX      a1=address, a2=FUTEX64_OP_*, then
 *                    WAIT:    a3=the value expected, a4=ms (0 = no limit)
 *                    WAKE:    a3=how many
 *                    REQUEUE: a3=how many to wake | how many to move << 32,
 *                             a4=the word they move to
 *                  -> 0 / how many woke, or minus a Linux errno
 * SYS64_EXIT_GROUP a1=code: every thread of the program ends
 * SYS64_GETTGID    -> the program's number (the first thread's), where
 *                  SYS64_GETPID gives the calling thread's own
 * SYS64_NCORES     -> how many cores are running (how many threads it
 *                  makes sense to compute with)
 */
#define SYS64_CLONE      83
#define SYS64_FUTEX      84
#define SYS64_EXIT_GROUP 85
#define SYS64_GETTGID    86
#define SYS64_NCORES     87

/* A file cut back or grown to a length: a1=fd, a2=bytes -> 0 / -1
   (-2 = not allowed) */
#define SYS64_FTRUNCATE  88

/* Which file stands behind a descriptor: a1=fd -> its inode (a TFS
   slot, the same for every descriptor of that file) / -1. stat's
   st_ino, which a database compares to tell its file was not replaced
   under it. */
#define SYS64_FDINODE    89

/* --- Sockets for a C library (libc/overlay/src/threados/syscall.c) ---
 * A connection or a UDP socket belongs to the program (its tgid).
 *
 * SYS64_TCP_LISTEN  a1=port -> a listening socket / -1
 * SYS64_TCP_ACCEPT  a1=listener, a2=ms to wait (0 = only look)
 *                   -> a connection / -1
 * SYS64_TCP_POLL    a1=connection -> TCP_POLL_* bits (kernel/include/
 *                   tcp.h), bytes waiting in the top 16 / -1
 * SYS64_TCP_START   a1=&udp_info (ip, port) -> a connection whose SYN is
 *                   on its way; SYS64_TCP_POLL says when it is up
 * SYS64_TCP_RECVT   SYS64_TCP_RECV with a4 = ms to wait (0 = only look)
 * SYS64_TCP_ADDR    a1=connection, a2=&{u32 remote ip; u16 remote port;
 *                   u16 local port} -> 0 / -1
 * SYS64_UDP_POLL    a1=socket -> datagrams waiting / -1
 * SYS64_UDP_PORT    a1=socket -> its local port / 0
 */
#define SYS64_TCP_LISTEN 90
#define SYS64_TCP_ACCEPT 91
#define SYS64_TCP_POLL   92
#define SYS64_TCP_START  93
#define SYS64_TCP_RECVT  94
#define SYS64_TCP_ADDR   95
#define SYS64_UDP_POLL   96
#define SYS64_UDP_PORT   97

/* SYS64_GETRANDOM  a1=buffer, a2=length (at most 4096), a3=flags
 *                  -> bytes written / -1 a bad buffer / -2 not seeded
 *                  yet and GETRANDOM_NOWAIT asked not to wait for it
 * Bytes fit for keys, from kernel/random.c. Until the pool is seeded
 * the call waits, as Linux's getrandom does.
 */
#define SYS64_GETRANDOM  98
#define GETRANDOM_NOWAIT 1

#define FUTEX64_OP_WAIT    0
#define FUTEX64_OP_WAKE    1
#define FUTEX64_OP_REQUEUE 3

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
