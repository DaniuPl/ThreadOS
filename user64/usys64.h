/* The ProtonOS 64 program library - calls through the syscall
 * instruction
 *
 * In long mode the kernel is no longer entered through an interrupt
 * gate. The syscall instruction takes the number in RAX, the arguments
 * in RDI and RSI, and hands the result back in RAX. It destroys RCX
 * and R11 along the way - the processor leaves the return address and
 * the flags in them - so they have to be named in the clobber list,
 * otherwise the compiler will assume they survived the call.
 *
 * Apart from those two and the RAX holding the result, all registers
 * come back untouched: the kernel saves them at its end. That is part
 * of the contract and this list rests on it.
 */

#ifndef PROTONOS_USYS64_H
#define PROTONOS_USYS64_H

#include <stdint.h>

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
#define SYS64_NETINFO     23
#define SYS64_UDP_OPEN    24
#define SYS64_UDP_SEND    25
#define SYS64_UDP_RECV    26
#define SYS64_UDP_CLOSE   27
#define SYS64_TCP_CONNECT 28
#define SYS64_TCP_SEND    29
#define SYS64_TCP_RECV    30
#define SYS64_TCP_CLOSE   31
#define SYS64_WIN_CREATE 32
#define SYS64_WIN_RECT   33
#define SYS64_WIN_TEXT   34
#define SYS64_WIN_POLL   35
#define SYS64_SHM        36
#define SYS64_TIME       37
#define SYS64_SYSINFO    38
#define SYS64_GETPID     39
#define SYS64_MMAP_FILE  40
#define SYS64_MSYNC      41
#define SYS64_CORE      42
#define SYS64_MMAP      43
#define SYS64_THREAD    44

#define NAME_MAX64 63

/* Numbers taken since forever: zero is the keyboard, one is the
   console */
#define FD_STDIN   0
#define FD_STDOUT  1

#define SIGINT   2
#define SIGKILL  9
#define SIGTERM 15

static inline int64_t syscall64(int64_t nr, int64_t a1, int64_t a2)
{
    int64_t ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr), "D"(a1), "S"(a2)
                     : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t syscall64_3(int64_t nr, int64_t a1, int64_t a2,
                                  int64_t a3)
{
    int64_t ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return ret;
}

/* The fourth argument travels in R10, because RCX is taken by the
   syscall instruction itself for the return address */
static inline int64_t syscall64_4(int64_t nr, int64_t a1, int64_t a2,
                                  int64_t a3, int64_t a4)
{
    int64_t ret;
    register int64_t r10 __asm__("r10") = a4;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(r10)
                     : "rcx", "r11", "memory");
    return ret;
}

static inline uint64_t ustrlen(const char *s)
{
    uint64_t n = 0;
    while (s[n])
        n++;
    return n;
}

/* Printing to the console is an ordinary write to descriptor number
   one - exactly the same as a write to a file */
static inline void print(const char *s)
{
    syscall64_3(SYS64_WRITE, FD_STDOUT, (int64_t)(uintptr_t)s,
                (int64_t)ustrlen(s));
}

static inline int64_t sys_write(int fd, const void *buf, uint64_t len)
{
    return syscall64_3(SYS64_WRITE, fd, (int64_t)(uintptr_t)buf,
                       (int64_t)len);
}

/* Opens a file in the root directory. create=1 creates it when there
   is none. */
static inline int64_t sys_open(const char *name, int create)
{
    return syscall64_3(SYS64_OPEN, (int64_t)(uintptr_t)name,
                       (int64_t)ustrlen(name), create);
}

static inline int64_t sys_close(int fd)
{
    return syscall64(SYS64_CLOSE, fd, 0);
}

static inline void sys_exit(int code)
{
    syscall64(SYS64_EXIT, code, 0);
    for (;;)
        ;                            /* the kernel will not come back here */
}

static inline void sys_sleep(uint32_t ms)
{
    syscall64(SYS64_SLEEP, (int64_t)ms, 0);
}

static inline uint64_t sys_ticks(void)
{
    return (uint64_t)syscall64(SYS64_TICKS, 0, 0);
}

/* Only the processor knows the code selector, and its two lowest bits
   say which ring the program is REALLY running in. The program has no
   way of making that up - it can only pass on what it read. */
static inline void sys_report_ring(void)
{
    uint64_t cs;
    __asm__ volatile("movw %%cs, %%ax; movzwq %%ax, %0" : "=r"(cs) :: "rax");
    syscall64(SYS64_RING, (int64_t)cs, 0);
}

/* A decimal number into a buffer; returns the length */
static inline int utoa(uint64_t v, char *buf)
{
    char tmp[24];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v);
    for (int i = 0; i < n; i++)
        buf[i] = tmp[n - 1 - i];
    buf[n] = 0;
    return n;
}

/* Duplicates the process. In the parent it returns the child's number,
   in the child zero. From this point on these are two programs: the
   same memory, but copied already - one's write is not visible to the
   other. */
static inline int64_t sys_fork(void)
{
    return syscall64(SYS64_FORK, 0, 0);
}

/* Replaces our own image with a program from the initrd. On success it
   does not return - from the next instruction on it is that program
   running. argv is a zero-terminated array of pointers (it may be
   NULL). */
static inline int64_t sys_exec(const char *name, char *const argv[])
{
    return syscall64_3(SYS64_EXEC, (int64_t)(uintptr_t)name,
                       (int64_t)ustrlen(name), (int64_t)(uintptr_t)argv);
}

/* --- Network ------------------------------------------------------
 *
 * The socket description in one place, because three registers for
 * arguments are too few for a socket, an address, a port, a length and
 * a timeout at once. The layout has to match the one in
 * kernel64/syscall64.c.
 *
 * On a receive the kernel writes the sender's address and port back in
 * here.
 */
struct udp_info {
    int sock;
    uint32_t ip;
    uint16_t port;
    uint16_t _padding;
    uint32_t timeout;
};

/* Our own address, the mask, the gateway and the DNS. Returns 1 when
   the system already has an address from the network. */
static inline int64_t sys_netinfo(uint32_t addrs[4])
{
    return syscall64(SYS64_NETINFO, (int64_t)(uintptr_t)addrs, 0);
}

static inline int64_t sys_udp_open(uint16_t port)
{
    return syscall64(SYS64_UDP_OPEN, port, 0);
}

static inline int64_t sys_udp_send(struct udp_info *o, const void *buf,
                                   uint64_t len)
{
    return syscall64_3(SYS64_UDP_SEND, (int64_t)(uintptr_t)o,
                       (int64_t)(uintptr_t)buf, (int64_t)len);
}

static inline int64_t sys_udp_recv(struct udp_info *o, void *buf,
                                   uint64_t max)
{
    return syscall64_3(SYS64_UDP_RECV, (int64_t)(uintptr_t)o,
                       (int64_t)(uintptr_t)buf, (int64_t)max);
}

static inline int64_t sys_udp_close(int sock)
{
    return syscall64(SYS64_UDP_CLOSE, sock, 0);
}

static inline int64_t sys_tcp_connect(struct udp_info *o)
{
    return syscall64(SYS64_TCP_CONNECT, (int64_t)(uintptr_t)o, 0);
}

static inline int64_t sys_tcp_send(int conn, const void *buf, uint64_t len)
{
    return syscall64_3(SYS64_TCP_SEND, conn, (int64_t)(uintptr_t)buf,
                       (int64_t)len);
}

static inline int64_t sys_tcp_recv(int conn, void *buf, uint64_t max)
{
    return syscall64_3(SYS64_TCP_RECV, conn, (int64_t)(uintptr_t)buf,
                       (int64_t)max);
}

static inline int64_t sys_tcp_close(int conn)
{
    return syscall64(SYS64_TCP_CLOSE, conn, 0);
}

/* An address from the string "10.0.2.2"; -1 when that is not an
   address */
static inline int64_t uparse_ip(const char *s)
{
    uint32_t ip = 0;
    for (int part = 0; part < 4; part++) {
        int v = 0, digits = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (*s++ - '0');
            digits++;
        }
        if (!digits || v > 255)
            return -1;
        ip = (ip << 8) | (uint32_t)v;
        if (part < 3 && *s++ != '.')
            return -1;
    }
    return *s ? -1 : (int64_t)ip;
}

/* Creates a pipe. It writes two numbers: fd[0] for reading, fd[1] for
   writing. As long as the process holds both, the pipe will never
   end. */
static inline int64_t sys_pipe(int fd[2])
{
    return syscall64(SYS64_PIPE, (int64_t)(uintptr_t)fd, 0);
}

/* Copies a descriptor number - that is how input and output are
   redirected for a program that knows nothing about it */
static inline int64_t sys_dup2(int old, int fresh_one)
{
    return syscall64(SYS64_DUP2, old, fresh_one);
}

/* One directory entry. The layout has to match what the kernel fills
   in in the SYS64_READDIR handler. */
struct dirent64 {
    char name[NAME_MAX64 + 1];
    uint32_t size;
    int is_dir;
};

static inline int64_t sys_mkdir(const char *name)
{
    return syscall64(SYS64_MKDIR, (int64_t)(uintptr_t)name,
                     (int64_t)ustrlen(name));
}

static inline int64_t sys_chdir(const char *name)
{
    return syscall64(SYS64_CHDIR, (int64_t)(uintptr_t)name,
                     (int64_t)ustrlen(name));
}

static inline int64_t sys_getcwd(char *buf, uint64_t len)
{
    return syscall64(SYS64_GETCWD, (int64_t)(uintptr_t)buf, (int64_t)len);
}

/* Successive entries of the working directory: we ask for the numbers
   in turn as long as we keep getting an answer */
static inline int64_t sys_readdir(int index, struct dirent64 *out)
{
    return syscall64(SYS64_READDIR, index, (int64_t)(uintptr_t)out);
}

static inline int64_t sys_unlink(const char *name)
{
    return syscall64(SYS64_UNLINK, (int64_t)(uintptr_t)name,
                     (int64_t)ustrlen(name));
}

/* Moves the limit of our own heap and returns the OLD one - that is,
   the address we may start writing from. Zero merely asks "where am
   I".
   Memory becomes real only on the first touch, so asking for a lot
   costs nothing until it is used. */
static inline void *sys_sbrk(int64_t delta)
{
    int64_t r = syscall64(SYS64_SBRK, delta, 0);
    return r < 0 ? (void *)0 : (void *)(uintptr_t)r;
}

/* A number from a string; -1 when that is not a number */
static inline int64_t uatoi(const char *s)
{
    int64_t v = 0;
    if (!*s)
        return -1;
    for (; *s; s++) {
        if (*s < '0' || *s > '9')
            return -1;
        v = v * 10 + (*s - '0');
    }
    return v;
}

/* Waits for any of our own children to end. Returns its number and
   writes out the exit code; -1 = there is nothing to wait for. */
static inline int64_t sys_wait(int *code)
{
    return syscall64(SYS64_WAIT, (int64_t)(uintptr_t)code, 0);
}

/* Reads from the keyboard. Blocks until the first character; -1 means
   a signal interrupted the wait. No echo - the echo is done by the
   reader. */
static inline int64_t sys_read(int fd, void *buf, uint64_t len)
{
    return syscall64_3(SYS64_READ, fd, (int64_t)(uintptr_t)buf,
                       (int64_t)len);
}

/* The return from a signal handler.
 *
 * It has to lie in the program, not in the kernel: once the handler
 * finishes, instructions have to be executed, and the only executable
 * pages the process has are its own code. The kernel slips the address
 * of this label under the handler as the return address - so an
 * ordinary ret lands here, and from here one call goes back into the
 * kernel for the frame. */
__asm__(".globl protonos_sigreturn\n"
        "protonos_sigreturn:\n"
        "    movq $10, %rax\n"          /* SYS64_SIGRETURN */
        "    syscall\n");
extern void protonos_sigreturn(void);

/* Sets the signal handler (0 = kill the program, which is the
   default). The return address is supplied by the library, because the
   kernel has nowhere to get it from. */
static inline void sys_signal(int sig, void (*handler)(int))
{
    syscall64_3(SYS64_SIGNAL, sig, (int64_t)(uintptr_t)handler,
                (int64_t)(uintptr_t)protonos_sigreturn);
}

static inline int64_t sys_kill(int64_t id, int sig)
{
    return syscall64(SYS64_KILL, id, sig);
}

/* A hexadecimal number - addresses are read no other way */
static inline int uhtoa(uint64_t v, char *buf)
{
    static const char digits[] = "0123456789abcdef";
    char tmp[24];
    int n = 0;
    do {
        tmp[n++] = digits[v & 0xF];
        v >>= 4;
    } while (v);
    for (int i = 0; i < n; i++)
        buf[i] = tmp[n - 1 - i];
    buf[n] = 0;
    return n;
}

/* --- Desktop windows ----------------------------------------------
 *
 * A program does not draw on the screen: it draws in its window's
 * buffer, and the kernel's compositor assembles a frame out of those
 * buffers. Coordinates are relative to the window's content area,
 * colours are indices into the desktop's palette.
 */
struct win_rect_cmd {
    int32_t x, y, w, h, color;
};
struct win_text_cmd {
    int32_t x, y, color;
    const char *s;
};

#define WEV_NONE  0
#define WEV_CLICK 1
#define WEV_KEY   2
struct win_event {
    int32_t type;          /* WEV_* */
    int32_t x, y;          /* coordinates relative to the content area */
    int32_t val;           /* a mask of buttons or a character code */
};

/* The time from the CMOS clock. The field layout has to match struct
   rtc_time in the kernel - it travels through the system call as a bare
   block of bytes. */
struct time64 {
    uint16_t rok;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
} __attribute__((packed));

static inline int64_t sys_time(struct time64 *t)
{
    return syscall64(SYS64_TIME, (int64_t)(uintptr_t)t, 0);
}

/* The machine's state: how much memory there is, how much is used, how
   long it has been running. The layout has to match the one in
   kernel64/syscall64.c. */
struct sysinfo64 {
    uint32_t ram_kib;
    uint32_t used_kib;
    uint32_t uptime_s;
};

static inline int64_t sys_sysinfo(struct sysinfo64 *out)
{
    return syscall64(SYS64_SYSINFO, (int64_t)(uintptr_t)out, 0);
}

/* A file seen as memory.
 *
 * Returns the address at which the file's contents are visible and
 * writes out its size. No page is created right away - the bytes come
 * from disk under your fingers, so mapping a large file costs as much
 * as a list of its blocks. This version is read-only: an attempted
 * write ends in a protection fault rather than in data quietly getting
 * lost. */
static inline const void *sys_mmap_file(const char *name, uint64_t *size)
{
    int64_t r = syscall64_4(SYS64_MMAP_FILE, (int64_t)(uintptr_t)name,
                            (int64_t)ustrlen(name),
                            (int64_t)(uintptr_t)size, 0);
    return r <= 0 ? (const void *)0 : (const void *)(uintptr_t)r;
}

/* The same, but with write permission: changes in memory go back to
 * the file. Not right away - only at sys_msync or when the process
 * exits. */
static inline void *sys_mmap_file_w(const char *name, uint64_t *size)
{
    int64_t r = syscall64_4(SYS64_MMAP_FILE, (int64_t)(uintptr_t)name,
                            (int64_t)ustrlen(name),
                            (int64_t)(uintptr_t)size, 1);
    return r <= 0 ? (void *)0 : (void *)(uintptr_t)r;
}

/* Which processor core the program is running on right now */
static inline int64_t sys_core(void)
{
    return syscall64(SYS64_CORE, 0, 0);
}

/* Fresh, zeroed memory of the process: returns the address or NULL.
 * Nothing is allocated on asking - only the addresses are reserved,
 * and the pages appear the moment they are touched. */
static inline void *sys_mmap(uint64_t size)
{
    return (void *)(uintptr_t)syscall64(SYS64_MMAP, (int64_t)size, 0);
}

/* A thread in the same process: it sees the same memory, and has its
 * own stack. 'entry' is a void(*)(void*) function of this program.
 * Returns its task number or -1.
 *
 * A thread carries the process's right to run on any core, so two of
 * them really can stand in one address space at once - and a mapping
 * changed by one of them reaches the cores running the others. */
static inline int64_t sys_thread(void (*entry)(void *), void *arg)
{
    return syscall64(SYS64_THREAD, (int64_t)(uintptr_t)entry,
                     (int64_t)(uintptr_t)arg);
}

/* Flushes to their files the changed pages of writable mappings.
 * Returns how many pages went out to disk - a page untouched since
 * last time costs nothing, because it is the processor that says which
 * ones are dirty. */
static inline int64_t sys_msync(void)
{
    return syscall64(SYS64_MSYNC, 0, 0);
}

/* Shared memory by key.
 *
 * The same key means the same physical memory - two processes get it
 * at their own addresses but write into the same place. Returns the
 * address, or zero when it failed. */
static inline void *sys_shm(int key, uint64_t size)
{
    int64_t r = syscall64(SYS64_SHM, key, (int64_t)size);
    return r <= 0 ? (void *)0 : (void *)(uintptr_t)r;
}

/* Creates a window with a content area of cw x ch. Returns the window
   number or -1. */
static inline int64_t win_create(int cw, int ch, const char *title)
{
    return syscall64_3(SYS64_WIN_CREATE, cw, ch,
                       (int64_t)(uintptr_t)title);
}

static inline void win_rect(int win, int x, int y, int w, int h, int color)
{
    struct win_rect_cmd c = {x, y, w, h, color};
    syscall64(SYS64_WIN_RECT, win, (int64_t)(uintptr_t)&c);
}

static inline void win_text(int win, int x, int y, const char *s, int color)
{
    struct win_text_cmd c = {x, y, color, s};
    syscall64(SYS64_WIN_TEXT, win, (int64_t)(uintptr_t)&c);
}

/* Fetches a window event (non-blocking). 1 = there is one, 0 = none,
   -1 = error. */
static inline int64_t win_poll(int win, struct win_event *ev)
{
    return syscall64(SYS64_WIN_POLL, win, (int64_t)(uintptr_t)ev);
}

#endif
