/* A compatibility dictionary for 64-bit programs
 *
 * The cryptography, TLS, HTTP, DNS and the HTML parser live in the
 * headers in user/ and know nothing about the processor mode - they
 * compute on bytes. They do talk to the kernel through a handful of
 * names, though: connect, send, receive, what time is it, how many
 * ticks, open a file.
 *
 * Those names are the contract. What stands behind them is the
 * kernel's business - and this file is the whole difference between
 * one side and the other, exactly like kernel64/compat64.c on the
 * kernel's side.
 *
 * The difference in the shape of the calls comes from the fact that in
 * long mode the arguments travel in registers rather than in structures
 * pointed to by ECX: there are more registers and most descriptions
 * fitted in directly.
 */

#ifndef PROTONOS_COMPAT64_H
#define PROTONOS_COMPAT64_H

#include "usys64.h"

/* --- Files --------------------------------------------------------
 *
 * The 64-bit kernel has no open flags yet: a file either exists, or is
 * created when opened. So we translate the flags into that one question
 * it knows how to ask. */
#define O_READ   1
#define O_WRITE  2
#define O_CREATE 4
#define O_TRUNC  8

#define STDIN   FD_STDIN
#define STDOUT  FD_STDOUT

static inline int32_t u_open(const char *name, int flags)
{
    return (int32_t)sys_open(name, (flags & O_CREATE) ? 1 : 0);
}
#define sys_open(name, flags) u_open((name), (flags))

/* --- Keys with no ASCII equivalent ---------------------------------
 *
 * They have to match kernel/include/keyboard.h - it is the kernel that
 * puts these codes into the queue, and the shared kernel/kbdmap.c that
 * translates them. */
#define KEY_UP    0x11
#define KEY_DOWN  0x12
#define KEY_LEFT  0x13
#define KEY_RIGHT 0x14
#define KEY_PGUP  0x15
#define KEY_PGDN  0x16
#define KEY_HOME  0x17
#define KEY_END   0x18

/* --- Who is at the machine -----------------------------------------
 *
 * The same three calls as in protected mode, only the arguments travel
 * in registers: the password goes with its length, because that is how
 * every text does in long mode. */
#define LEVEL_ADMIN 0
#define LEVEL_PLAIN 1

static inline int32_t sys_auth(const char *password)
{
    return (int32_t)syscall64_3(SYS64_AUTH, (int64_t)(uintptr_t)password,
                                (int64_t)ustrlen(password), 0);
}

static inline int32_t sys_drop(void)
{
    return (int32_t)syscall64(SYS64_DROP, 0, 0);
}

static inline int32_t sys_level(void)
{
    return (int32_t)syscall64(SYS64_LEVEL, 0, 0);
}

/* Where the next read from that file starts */
static inline int32_t sys_seek(int fd, uint32_t pos)
{
    return (int32_t)syscall64(SYS64_SEEK, (int64_t)fd, (int64_t)pos);
}

/* A wait that does not wait: 0 = ended (and 'code' holds the exit
   code), -3 = still running. */
static inline int32_t sys_trywait(int32_t pid, int *code)
{
    return (int32_t)syscall64(SYS64_TRYWAIT, (int64_t)pid,
                              (int64_t)(uintptr_t)code);
}

/* --- Time ---------------------------------------------------------- */

struct utime {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t min;
    uint8_t sec;
} __attribute__((packed));

/* The same field layout as struct time64 - only the names differ. The
   bytes are the same. */
static inline void u_time(struct utime *t)
{
    syscall64(SYS64_TIME, (int64_t)(uintptr_t)t, 0);
}
#define sys_time(t) u_time((struct utime *)(t))

/* Our own process number - for seeding the random number generator */
static inline int32_t sys_getpid(void)
{
    return (int32_t)syscall64(SYS64_GETPID, 0, 0);
}

static inline uint32_t sys_getticks(void)
{
    return (uint32_t)sys_ticks();
}

/* --- Printing numbers ----------------------------------------------- */

/* Waiting for any of our own children. In the 32-bit kernel this is
   called sys_waitany, in the 64-bit one sys_wait - the shared code says
   it with one word, and this file translates it to the right side. */
static inline int64_t sys_wait_child(int *code)
{
    return sys_wait(code);
}

static inline void print_dec(uint32_t v)
{
    char buf[24];
    utoa(v, buf);
    print(buf);
}

/* An IP address in the form a human reads */
static inline void print_ip(uint32_t ip)
{
    for (int i = 3; i >= 0; i--) {
        print_dec((ip >> (i * 8)) & 0xFF);
        if (i)
            print(".");
    }
}

static inline int ustreq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* --- UDP ------------------------------------------------------------
 *
 * The shape here is different: in long mode the socket, the address,
 * the port and the timeout travel in one description, because that read
 * better than packing them into registers. The program does not have to
 * know about it. */

static inline int32_t udp_open(uint32_t port)
{
    return (int32_t)sys_udp_open((uint16_t)port);
}

static inline int32_t udp_send(int32_t sock, uint32_t ip, uint32_t port,
                               const void *buf, uint32_t len)
{
    struct udp_info o = {(int)sock, ip, (uint16_t)port, 0, 0};
    return (int32_t)sys_udp_send(&o, buf, len);
}

static inline int32_t udp_recv(int32_t sock, void *buf, uint32_t max,
                               uint32_t timeout_ms, uint32_t *from_ip,
                               uint32_t *from_port)
{
    struct udp_info o = {(int)sock, 0, 0, 0, timeout_ms};
    int32_t n = (int32_t)sys_udp_recv(&o, buf, max);
    if (n >= 0) {
        /* The kernel writes the sender's address back into the same
   description */
        if (from_ip)
            *from_ip = o.ip;
        if (from_port)
            *from_port = o.port;
    }
    return n;
}

static inline void udp_close(int32_t sock)
{
    sys_udp_close((int)sock);
}

/* --- TCP ------------------------------------------------------------ */

static inline int32_t tcp_connect(uint32_t ip, uint32_t port,
                                  uint32_t timeout_ms)
{
    struct udp_info o = {0, ip, (uint16_t)port, 0, timeout_ms};
    return (int32_t)sys_tcp_connect(&o);
}

static inline int32_t tcp_send(int32_t conn, const void *buf, uint32_t len,
                               uint32_t timeout_ms)
{
    (void)timeout_ms;               /* the limit is set by the kernel */
    return (int32_t)sys_tcp_send((int)conn, buf, len);
}

static inline int32_t tcp_recv(int32_t conn, void *buf, uint32_t max,
                               uint32_t timeout_ms)
{
    (void)timeout_ms;
    return (int32_t)sys_tcp_recv((int)conn, buf, max);
}

static inline void tcp_close(int32_t conn)
{
    sys_tcp_close((int)conn);
}

/* --- Our own address ------------------------------------------------ */

struct netinfo {
    uint32_t ip, mask, gw, dns;
};

static inline int32_t netinfo(struct netinfo *ni)
{
    uint32_t addrs[4];
    if (sys_netinfo(addrs) != 1)
        return -1;
    ni->ip = addrs[0];
    ni->mask = addrs[1];
    ni->gw = addrs[2];
    ni->dns = addrs[3];
    return 0;
}

#endif
