/* The ProtonOS user program library - int 0x80 syscalls */

#ifndef PROTONOS_USYS_H
#define PROTONOS_USYS_H

#include <stdint.h>

#define SYS_WRITE    1
#define SYS_EXIT     2
#define SYS_SLEEP    3
#define SYS_GETTICKS 4
#define SYS_READ     5
#define SYS_SPAWN    6
#define SYS_WAIT     7
#define SYS_GETPID   8
#define SYS_SBRK     9
#define SYS_OPEN     10
#define SYS_CLOSE    11
#define SYS_SEEK     12
#define SYS_PIPELINE 13
#define SYS_READDIR  14
#define SYS_UNLINK   15
#define SYS_TIME     16
#define SYS_CHDIR    17
#define SYS_GETCWD   18
#define SYS_MKDIR    19
#define SYS_KILL     20
#define SYS_WIN_CREATE 21
#define SYS_WIN_RECT   22
#define SYS_WIN_TEXT   23
#define SYS_WIN_POLL   24
#define SYS_MMAP       25
#define SYS_SHM        26
#define SYS_SYSINFO    27
#define SYS_THREAD     28
#define SYS_SETPRIO    29
#define SYS_UDP_OPEN   30
#define SYS_UDP_SEND   31
#define SYS_UDP_RECV   32
#define SYS_UDP_CLOSE  33
#define SYS_NETINFO    34
#define SYS_TCP_CONNECT 35
#define SYS_TCP_LISTEN  36
#define SYS_TCP_ACCEPT  37
#define SYS_TCP_SEND    38
#define SYS_TCP_RECV    39
#define SYS_TCP_CLOSE   40
#define SYS_FORK        41
#define SYS_EXEC        42
#define SYS_WAITANY     43
#define SYS_SIGNAL      44
#define SYS_SIGRETURN   45
#define SYS_MMAP_FILE   46
#define SYS_MSYNC       47
#define SYS_CORE       48
#define SYS_CFG_GET    49
#define SYS_CFG_SET    50
#define SYS_AUTH       51
#define SYS_DROP       52
#define SYS_LEVEL      53
#define SYS_TRYWAIT    54

#define FS_NAME_MAX  63

/* Keys with no ASCII equivalent - they have to match
   kernel/include/keyboard.h */
#define KEY_UP    0x11
#define KEY_DOWN  0x12
#define KEY_LEFT  0x13
#define KEY_RIGHT 0x14
#define KEY_PGUP  0x15
#define KEY_PGDN  0x16
#define KEY_HOME  0x17
#define KEY_END   0x18

struct sysinfo {
    uint32_t ram_kib;
    uint32_t used_kib;
    uint32_t uptime_s;
};

/* The field layout has to match struct rtc_time in the kernel */
struct utime {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t min;
    uint8_t sec;
} __attribute__((packed));

/* open() flags */
#define O_READ   1
#define O_WRITE  2
#define O_CREATE 4
#define O_TRUNC  8

/* Standard descriptors */
#define STDIN  0
#define STDOUT 1
#define STDERR 2

static inline int32_t sys_write(int fd, const void *buf, uint32_t len)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_WRITE), "b"(fd), "c"(buf), "d"(len)
                     : "memory");
    return ret;
}

static inline void sys_exit(int code)
{
    __asm__ volatile("int $0x80" : : "a"(SYS_EXIT), "b"(code) : "memory");
    __builtin_unreachable();
}

static inline void sys_sleep(uint32_t ms)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_SLEEP), "b"(ms)
                     : "memory");
    (void)ret;
}

static inline uint32_t sys_getticks(void)
{
    uint32_t ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(SYS_GETTICKS) : "memory");
    return ret;
}

static inline int32_t sys_read(int fd, void *buf, uint32_t len)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_READ), "b"(fd), "c"(buf), "d"(len)
                     : "memory");
    return ret;
}

/* Runs a program; outfile != NULL redirects stdout to a file
   (append=1 adds at the end instead of truncating) */
static inline int32_t sys_spawn(const char *cmdline, const char *outfile,
                                int append)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_SPAWN), "b"(cmdline), "c"(outfile), "d"(append)
                     : "memory");
    return ret;
}

/* A pipeline 'a | b | c'; outfile != NULL redirects the last one's
   stdout to a file */
static inline int32_t sys_pipeline(const char *pipeline, const char *outfile)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_PIPELINE), "b"(pipeline), "c"(outfile)
                     : "memory");
    return ret;
}

/* Waits for process 'pid' and returns its exit code */
static inline int32_t sys_wait(int32_t pid)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_WAIT), "b"(pid)
                     : "memory");
    return ret;
}

/* Forks the process. Returns the child's pid to the parent, zero to
   the child, -1 on error. The child gets its own copy of memory
   (physically the same one until the first write - copy on write). */
static inline int32_t sys_fork(void)
{
    int32_t ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(SYS_FORK) : "memory");
    return ret;
}

/* Waits for any of our own children. Returns its pid and writes the
   exit code to 'code' (which may be NULL). -1 = there are no children
   left. */
static inline int32_t sys_waitany(int32_t *code)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_WAITANY), "b"(code)
                     : "memory");
    return ret;
}

/* Waiting for any of our own children - one word on both sides (in
   long mode this is sys_wait; see user64/compat64.h) */
static inline int32_t sys_wait_child(int *code)
{
    return sys_waitany((int32_t *)code);
}

/* Replaces the current process's program (the pid and the descriptors
   stay). On success it does not return; -1 means the program cannot be
   started and the old image carries on running. */
static inline int32_t sys_exec(const char *cmdline)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_EXEC), "b"(cmdline)
                     : "memory");
    return ret;
}

static inline int32_t sys_getpid(void)
{
    int32_t ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(SYS_GETPID) : "memory");
    return ret;
}

static inline void *sys_sbrk(int32_t increment)
{
    void *ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_SBRK), "b"(increment)
                     : "memory");
    return ret;
}

/* mmap: fresh, zeroed process memory; returns the address or NULL */
static inline void *sys_mmap(uint32_t size)
{
    void *ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_MMAP), "b"(size)
                     : "memory");
    return ret;
}

/* A file seen as memory.
 *
 * Returns the address at which the file's contents are visible and
 * writes out its size. No page is created right away - the bytes come
 * from disk under your fingers, so mapping a large file costs as much
 * as a list of its blocks. This version is read-only: an attempted
 * write ends in a protection fault rather than in data quietly getting
 * lost. */
static inline const void *sys_mmap_file(const char *name, uint32_t *size)
{
    const void *ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_MMAP_FILE), "b"(name), "c"(size), "d"(0)
                     : "memory");
    return ret;
}

/* The same, but with write permission: changes in memory go back to
 * the file. Not right away - only at sys_msync or when the process
 * exits. */
static inline void *sys_mmap_file_w(const char *name, uint32_t *size)
{
    void *ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_MMAP_FILE), "b"(name), "c"(size), "d"(1)
                     : "memory");
    return ret;
}

/* Which processor core the program is running on right now.
 * It has no way of checking that itself - the local APIC lies outside
 * its address space. */
static inline int sys_core(void)
{
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(SYS_CORE) : "memory");
    return ret;
}

/* Flushes to their files the changed pages of writable mappings.
 * Returns how many pages went out to disk - a page untouched since
 * last time costs nothing, because it is the processor that says which
 * ones are dirty. */
static inline int sys_msync(void)
{
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(SYS_MSYNC) : "memory");
    return ret;
}

/* shm: shared memory by key; the same key = the same memory */
static inline void *sys_shm(int key, uint32_t size)
{
    void *ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_SHM), "b"(key), "c"(size)
                     : "memory");
    return ret;
}

/* thread: creates a thread in the same process (shared memory). entry
   is a void(*)(void*) function. Returns a tid or -1. */
static inline int32_t sys_thread(void (*entry)(void *), void *arg)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_THREAD), "b"(entry), "c"(arg)
                     : "memory");
    return ret;
}

/* setprio: the current task's priority (0=high, 1=normal, 2=low) */
static inline void sys_setprio(int level)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_SETPRIO), "b"(level)
                     : "memory");
    (void)ret;
}

/* sysinfo: system statistics (RAM, uptime) into the given buffer */
static inline void sys_sysinfo(struct sysinfo *si)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_SYSINFO), "b"(si)
                     : "memory");
    (void)ret;
}

/* The simplest bump allocator on top of sbrk: it only allocates, it
   never frees. That is entirely enough for toy programs. */
static inline void *malloc(uint32_t size)
{
    size = (size + 7) & ~7u;
    void *p = sys_sbrk((int32_t)size);
    return p == (void *)-1 ? (void *)0 : p;
}

static inline int32_t sys_open(const char *name, int flags)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_OPEN), "b"(name), "c"(flags)
                     : "memory");
    return ret;
}

static inline int32_t sys_close(int fd)
{
    int32_t ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(SYS_CLOSE), "b"(fd) : "memory");
    return ret;
}

static inline int32_t sys_seek(int fd, uint32_t off)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_SEEK), "b"(fd), "c"(off)
                     : "memory");
    return ret;
}

/* Enumerating the working directory: the next entry with number
   'index'. Returns the file size, -2 for a directory, or -1 when there
   are no more entries. */
static inline int32_t sys_readdir(int index, char *name)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_READDIR), "b"(index), "c"(name)
                     : "memory");
    return ret;
}

static inline int32_t sys_chdir(const char *path)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_CHDIR), "b"(path)
                     : "memory");
    return ret;
}

static inline void sys_getcwd(char *buf)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_GETCWD), "b"(buf)
                     : "memory");
    (void)ret;
}

static inline int32_t sys_mkdir(const char *path)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_MKDIR), "b"(path)
                     : "memory");
    return ret;
}

/* Signals - the numbers have to match kernel/include/signal.h */
#define SIGINT   2      /* an interrupt from the keyboard (Ctrl+C) */
#define SIGKILL  9      /* a kill that cannot be caught */
#define SIGTERM 15      /* a request to finish */
#define SIGCHLD 17      /* a child has ended */

#define SIG_DFL 0       /* the kernel's default reaction */
#define SIG_IGN 1       /* ignore the signal */

typedef void (*sighandler_t)(int32_t sig);

/* Sends a signal to a process; sig=0 simply means SIGKILL */
static inline int32_t sys_kill(int pid, int32_t sig)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_KILL), "b"(pid), "c"(sig)
                     : "memory");
    return ret;
}

/* The trampoline that returns from a signal handler.
 *
 * The kernel puts its address down as the handler's return address, so
 * when the handler ends with an ordinary 'ret', control lands here,
 * and from here goes through a syscall back into the kernel - it is
 * the kernel that restores the state from before the signal. So a
 * handler cannot simply return to the interrupted code on its own. */
__attribute__((unused, naked))
static void __sig_tramp(void)
{
    __asm__ volatile("movl %0, %%eax\n\t"
                     "int $0x80" : : "i"(SYS_SIGRETURN));
}

/* Sets a signal handler (SIG_DFL / SIG_IGN / your own). SIGKILL cannot
   be caught. Returns 0 or -1. */
static inline int32_t sys_signal(int32_t sig, sighandler_t handler)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_SIGNAL), "b"(sig), "c"(handler),
                       "d"(__sig_tramp)
                     : "memory");
    return ret;
}

/* --- GUI: an application window drawn on the desktop --- */

struct win_rect_cmd {
    int32_t x, y, w, h, color;
};
struct win_text_cmd {
    int32_t x, y, color;
    const char *s;
};

/* Creates a window with a content area of cw x ch; returns a winid or
   -1 */
static inline int32_t win_create(int cw, int ch, const char *title)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_WIN_CREATE), "b"(cw), "c"(ch), "d"(title)
                     : "memory");
    return ret;
}

static inline void win_rect(int winid, int x, int y, int w, int h, int color)
{
    struct win_rect_cmd c = { x, y, w, h, color };
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_WIN_RECT), "b"(winid), "c"(&c)
                     : "memory");
    (void)ret;
}

static inline void win_text(int winid, int x, int y, const char *s, int color)
{
    struct win_text_cmd c = { x, y, color, s };
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_WIN_TEXT), "b"(winid), "c"(&c)
                     : "memory");
    (void)ret;
}

#define WEV_NONE  0
#define WEV_CLICK 1
#define WEV_KEY   2
/* The window is being closed - the program is being ASKED to end. It
   has about two seconds to do it; after that the desktop stops asking
   and kills it, because a window nobody can close is worse than a
   program that loses its say. */
#define WEV_CLOSE 3
/* The content area has a new size: x and y carry it. Sent once, when
   the pulling of the corner ends. */
#define WEV_SIZE  4
struct win_event {
    int32_t type;          /* WEV_* */
    int32_t x, y;          /* coordinates relative to the content area */
    int32_t val;           /* a mask of buttons */
};

/* Fetches a window event (non-blocking). Returns 1 = there is one,
   0 = none. */
static inline int32_t win_poll(int winid, struct win_event *ev)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_WIN_POLL), "b"(winid), "c"(ev)
                     : "memory");
    return ret;
}

static inline int32_t sys_unlink(const char *name)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_UNLINK), "b"(name)
                     : "memory");
    return ret;
}

static inline void sys_time(struct utime *t)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_TIME), "b"(t)
                     : "memory");
    (void)ret;
}

static inline int ustreq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static inline uint32_t ustrlen(const char *s)
{
    uint32_t n = 0;
    while (s[n])
        n++;
    return n;
}

/* --- System settings ------------------------------------------------
 *
 * What the machine remembers about itself: its name, the name server,
 * the colour of the prompt, the desktop's background. They live in the
 * kernel and in a file on the disk, so a program only asks - and a
 * change it makes is there after the next boot. */
#define CFG_NAME_MAX 32
#define CFG_VAL_MAX  32
#define CFG_HELP_MAX 80

struct cfg_entry {
    char name[CFG_NAME_MAX];
    char value[CFG_VAL_MAX];
    char help[CFG_HELP_MAX];
};

/* The i-th setting. Returns 0, or -1 past the last one. */
static inline int32_t sys_cfg_get(uint32_t index, struct cfg_entry *out)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_CFG_GET), "b"(index), "c"(out)
                     : "memory");
    return ret;
}

/* Changes a setting and writes them all back to the disk. 0 or -1. */
/* --- who is at the machine -----------------------------------------
 *
 * USER_ADMIN may change the system - the settings the kernel reads at
 * boot, and the files it keeps them in. Everybody else may do
 * everything else.
 *
 * A program goes up by a password and down for nothing. The way down
 * is never checked: giving up a right is always allowed. */
#define LEVEL_ADMIN 0
#define LEVEL_PLAIN 1

/* 0 = an admin from now on; -2 = wrong password (or too many tries too
   quickly, which the kernel answers the same way) */
static inline int32_t sys_auth(const char *password)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_AUTH), "b"(password)
                     : "memory");
    return ret;
}

static inline int32_t sys_drop(void)
{
    int32_t ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(SYS_DROP) : "memory");
    return ret;
}

static inline int32_t sys_level(void)
{
    int32_t ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(SYS_LEVEL) : "memory");
    return ret;
}

/* A wait that does not wait: 0 = it has ended (and 'code' holds its
   exit code), -3 = still running. For a program that watches another
   one and has something else to do meanwhile - drawing, for
   instance. */
static inline int32_t sys_trywait(int32_t pid, int *code)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_TRYWAIT), "b"(pid), "c"(code)
                     : "memory");
    return ret;
}

static inline int32_t sys_cfg_set(const char *name, const char *value)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_CFG_SET), "b"(name), "c"(value)
                     : "memory");
    return ret;
}

/* Does the substring 'needle' occur in 'hay' */
static inline int ustrstr(const char *hay, const char *needle)
{
    if (!*needle)
        return 1;
    for (; *hay; hay++) {
        const char *h = hay, *n = needle;
        while (*h && *n && *h == *n) {
            h++;
            n++;
        }
        if (!*n)
            return 1;
    }
    return 0;
}

static inline int uatoi(const char *s)
{
    int v = 0;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return v;
}

/* Prints a decimal number to stdout */
static inline void print_dec(uint32_t v)
{
    char buf[12];
    int n = 0;
    do {
        buf[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        sys_write(STDOUT, &buf[--n], 1);
}

static inline void print(const char *s)
{
    sys_write(STDOUT, s, ustrlen(s));
}

/* --- network: UDP sockets ----------------------------------------------
   IPv4 addresses are in host order (10.0.2.3 == 0x0A000203). */

struct netinfo {
    uint32_t ip, mask, gw, dns;
};

struct udp_send_cmd {
    uint32_t ip, port, buf, len;
};

struct udp_recv_cmd {
    uint32_t buf, max, timeout_ms;
    uint32_t from_ip, from_port;    /* filled in by the kernel */
};

/* Opens a UDP socket on a port (0 = allocate any free one). */
static inline int32_t udp_open(uint32_t port)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_UDP_OPEN), "b"(port)
                     : "memory");
    return ret;
}

static inline int32_t udp_send(int32_t sock, uint32_t ip, uint32_t port,
                               const void *buf, uint32_t len)
{
    struct udp_send_cmd c = { ip, port, (uint32_t)buf, len };
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_UDP_SEND), "b"(sock), "c"(&c)
                     : "memory");
    return ret;
}

/* Waits up to timeout_ms for a datagram; returns the length or -1.
   from_ip/from_port may be zero when the sender does not interest
   us. */
static inline int32_t udp_recv(int32_t sock, void *buf, uint32_t max,
                               uint32_t timeout_ms, uint32_t *from_ip,
                               uint32_t *from_port)
{
    struct udp_recv_cmd c = { (uint32_t)buf, max, timeout_ms, 0, 0 };
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_UDP_RECV), "b"(sock), "c"(&c)
                     : "memory");
    if (ret >= 0) {
        if (from_ip)
            *from_ip = c.from_ip;
        if (from_port)
            *from_port = c.from_port;
    }
    return ret;
}

static inline void udp_close(int32_t sock)
{
    __asm__ volatile("int $0x80"
                     :
                     : "a"(SYS_UDP_CLOSE), "b"(sock)
                     : "memory");
}

/* The network configuration (from DHCP). 0 = OK, -1 = the system has
   no address yet. */
static inline int32_t netinfo(struct netinfo *ni)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_NETINFO), "b"(ni)
                     : "memory");
    return ret;
}

/* Prints an IPv4 address in the form a.b.c.d */
static inline void print_ip(uint32_t ip)
{
    for (int i = 3; i >= 0; i--) {
        print_dec((ip >> (i * 8)) & 0xFF);
        if (i)
            print(".");
    }
}

/* --- network: TCP connections ----------------------------------------- */

struct tcp_io_cmd {
    uint32_t buf, len, timeout_ms;
};

/* Establishes a connection; returns a handle or -1 */
static inline int32_t tcp_connect(uint32_t ip, uint32_t port,
                                  uint32_t timeout_ms)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_TCP_CONNECT), "b"(ip), "c"(port), "d"(timeout_ms)
                     : "memory");
    return ret;
}

/* Opens a listening socket on a port */
static inline int32_t tcp_listen(uint32_t port)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_TCP_LISTEN), "b"(port)
                     : "memory");
    return ret;
}

/* Accepts an established connection from a listening socket's queue */
static inline int32_t tcp_accept(int32_t listener, uint32_t timeout_ms)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_TCP_ACCEPT), "b"(listener), "c"(timeout_ms)
                     : "memory");
    return ret;
}

/* Sends data and waits for the other side's acknowledgement */
static inline int32_t tcp_send(int32_t conn, const void *buf, uint32_t len,
                               uint32_t timeout_ms)
{
    struct tcp_io_cmd c = { (uint32_t)buf, len, timeout_ms };
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_TCP_SEND), "b"(conn), "c"(&c)
                     : "memory");
    return ret;
}

/* Receives data: >0 = bytes, 0 = the other side closed, -1 = error or
   timeout */
static inline int32_t tcp_recv(int32_t conn, void *buf, uint32_t max,
                               uint32_t timeout_ms)
{
    struct tcp_io_cmd c = { (uint32_t)buf, max, timeout_ms };
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(SYS_TCP_RECV), "b"(conn), "c"(&c)
                     : "memory");
    return ret;
}

static inline void tcp_close(int32_t conn)
{
    __asm__ volatile("int $0x80"
                     :
                     : "a"(SYS_TCP_CLOSE), "b"(conn)
                     : "memory");
}

/* "10.0.2.2" -> 0x0A000202 (0 = a bad spelling) */
static inline uint32_t uparse_ip(const char *s)
{
    uint32_t ip = 0, part = 0;
    int digits = 0, parts = 0;

    for (; *s; s++) {
        if (*s == '.') {
            if (!digits || parts == 3)
                return 0;
            ip = (ip << 8) | (part & 0xFF);
            part = 0;
            digits = 0;
            parts++;
        } else if (*s >= '0' && *s <= '9') {
            part = part * 10 + (uint32_t)(*s - '0');
            if (part > 255 || ++digits > 3)
                return 0;
        } else {
            return 0;
        }
    }
    if (parts != 3 || !digits)
        return 0;
    return (ip << 8) | (part & 0xFF);
}

#endif
