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

#ifndef THREADOS_COMPAT64_H
#define THREADOS_COMPAT64_H

#include "usys64.h"
#include "../user/userctl.h"

#define SND_STREAM_BEGIN 0
#define SND_STREAM_WRITE 1
#define SND_STREAM_END 2
#define SND_STREAM_MAX_FRAMES 1024

static inline int32_t sys_snd_stream(int op, const int16_t *samples,
                                     uint32_t frames)
{
    return (int32_t)syscall64_3(SYS64_SND_STREAM, op,
                                (int64_t)(uintptr_t)samples, frames);
}

static inline int32_t sys_user(struct user_ctl *ctl)
{
    return (int32_t)syscall64(SYS64_USER, (int64_t)(uintptr_t)ctl, 0);
}

/* --- Files --------------------------------------------------------
 *
 * The 64-bit kernel knows two things about opening: whether a missing
 * file is created (OPEN64_CREATE), and whether an existing one is
 * emptied first (OPEN64_TRUNC). Reading and writing are not flags
 * there - every descriptor does both - so the flags are translated
 * into those two questions. Without the second, saving a shorter text
 * over a longer one left the end of the old one behind it. */
#define O_READ   1
#define O_WRITE  2
#define O_CREATE 4
#define O_TRUNC  8

#define STDIN   FD_STDIN
#define STDOUT  FD_STDOUT

static inline int32_t u_open(const char *name, int flags)
{
    return (int32_t)sys_open(name, ((flags & O_CREATE) ? 1 : 0) |
                                   ((flags & O_TRUNC) ? 2 : 0));
}
#define sys_open(name, flags) u_open((name), (flags))

/* --- Keys with no ASCII equivalent ---------------------------------
 *
 * They have to match kernel/include/keyboard.h - it is the kernel that
 * puts these codes into the queue, and the shared kernel/kbdmap.c that
 * translates them. */
#define KEY_SHOT  0x10      /* Print Screen */
#define KEY_UNDO  0x0E      /* Ctrl+Z */
#define KEY_FIND  0x07      /* Ctrl+F */
#define KEY_SAVE  0x05      /* Ctrl+S */
#define KEY_OPEN  0x06      /* Ctrl+O */
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

static inline int32_t sys_level(void)
{
    return (int32_t)syscall64(SYS64_LEVEL, 0, 0);
}

static inline int32_t sys_update_allowed(void)
{
    return (int32_t)syscall64(SYS64_UPDATE_ALLOWED, 0, 0);
}



/* --- Sound ------------------------------------------------------------
 *
 * Loudness is a knob and not a change to the system, so setting it
 * asks nobody's permission - it is the one setting a program may
 * write down. Playing is the kernel's: a program says what it wants
 * heard, not how.
 *
 * The percentage is of what the codec's amplifier actually has, and
 * its steps are decibels - so thirty per cent is far quieter than a
 * third. That is how loudness works.
 */
struct snd_info {
    uint32_t present;
    uint32_t volume;             /* 0..100 */
    uint32_t muted;
    uint32_t steps;
};

static inline int32_t sys_snd_get(struct snd_info *out)
{
    return (int32_t)syscall64(SYS64_SND_GET, (int64_t)(uintptr_t)out, 0);
}

static inline int32_t sys_snd_set(int32_t volume, int32_t mute)
{
    return (int32_t)syscall64(SYS64_SND_SET, volume, mute);
}

/* Semaphores: the same three calls, the same meaning */
static inline int32_t sys_sem_get(int key, int initial)
{
    return (int32_t)syscall64(SYS64_SEM_GET, key, initial);
}

static inline int32_t sys_sem_wait(int id)
{
    return (int32_t)syscall64(SYS64_SEM_WAIT, id, 0);
}

static inline int32_t sys_sem_post(int id)
{
    return (int32_t)syscall64(SYS64_SEM_POST, id, 0);
}

/* Shapes and TrueType text in a window, and the theme's colours */
struct win_draw_cmd {
    int32_t op, x, y, w, h, r;
    uint32_t rgb, rgb2;
    int32_t a, font, px;
    const char *s;
};

static inline int32_t sys_win_draw(int winid, const struct win_draw_cmd *c)
{
    return (int32_t)syscall64(SYS64_WIN_DRAW, winid, (int64_t)(uintptr_t)c);
}

static inline int32_t sys_ui_look_of(int which, uint32_t *out)
{
    return (int32_t)syscall64(SYS64_UI_LOOK, (int64_t)(uintptr_t)out,
                              which);
}

static inline int32_t sys_ui_look(uint32_t *out)
{
    return sys_ui_look_of(0, out);
}

static inline int32_t sys_notify(const char *title, const char *text)
{
    return (int32_t)syscall64(SYS64_NOTIFY, (int64_t)(uintptr_t)title,
                              (int64_t)(uintptr_t)text);
}

/* The settings - the same record and the same answers as on the 32-bit
   side (usys.h) */
#define CFG_NAME_MAX 32
#define CFG_VAL_MAX  32
#define CFG_HELP_MAX 80
struct cfg_entry {
    char name[CFG_NAME_MAX];
    char value[CFG_VAL_MAX];
    char help[CFG_HELP_MAX];
};

static inline int32_t sys_cfg_get(uint32_t index, struct cfg_entry *out)
{
    return (int32_t)syscall64(SYS64_CFG_GET, (int64_t)index,
                              (int64_t)(uintptr_t)out);
}

static inline int32_t sys_cfg_set(const char *name, const char *value)
{
    return (int32_t)syscall64(SYS64_CFG_SET, (int64_t)(uintptr_t)name,
                              (int64_t)(uintptr_t)value);
}

struct bg_preview_cmd {
    int32_t bg, dark, w, h;
    uint32_t *out;
};

static inline int32_t sys_bg_preview(int bg, int dark, int w, int h,
                                     uint32_t *out)
{
    struct bg_preview_cmd c = { bg, dark, w, h, out };
    return (int32_t)syscall64(SYS64_BG_PREVIEW, (int64_t)(uintptr_t)&c, 0);
}

static inline int32_t sys_snd_beep(uint32_t hz, uint32_t ms)
{
    return (int32_t)syscall64(SYS64_SND_BEEP, (int64_t)hz, (int64_t)ms);
}

/* --- Installing onto this machine's own disk -----------------------
 *
 * Two calls, and the split between them is the point. Looking reads
 * the disk and changes nothing, so anybody may do it - a window that
 * cannot say what it would write is a window nobody should trust.
 * Every step of the writing needs an admin, because on the other side
 * of it is the disk of a machine that boots without us.
 *
 * The layout must match struct install_info in kernel/include/install.h.
 */
#define INST_NAME 24

#define INST_OK        0
#define INST_NO_DISK   1
#define INST_NO_FILES  2
#define INST_NO_TABLE  3
#define INST_NO_ESP    4
#define INST_NOT_FAT   5
#define INST_NO_ROOM   6
#define INST_TOO_SMALL 7

/* Two ways, and the difference between them is everything: alongside
   writes three files and touches nothing else; the whole disk makes a
   new table and everything that was there is gone. */
#define INST_ALONGSIDE 0
#define INST_WHOLE     1

#define INST_STEPS       6
#define INST_STEPS_WHOLE 9

struct install_info {
    uint32_t ok;
    uint32_t problem;
    uint32_t mode;
    uint32_t steps;
    uint32_t can_wipe;
    uint32_t disk;
    uint32_t disks;
    uint32_t esp_index;
    uint32_t esp_lba;
    uint32_t free_kib;
    uint32_t need_kib;
    uint32_t loader_size;
    uint32_t kernel_size;
    uint32_t initrd_size;
    uint32_t fallback_free;
    uint32_t disk_mib;
    uint32_t parts;
    char disk_name[INST_NAME];
    char kind[8];
    char label[INST_NAME];
};

static inline int32_t sys_install_look(int32_t mode, int32_t disk,
                                       struct install_info *out)
{
    return (int32_t)syscall64_3(SYS64_INSTALL_LOOK, mode,
                                (int64_t)(uintptr_t)out, disk);
}

static inline int32_t sys_install_step(int32_t n)
{
    return (int32_t)syscall64(SYS64_INSTALL_STEP, n, 0);
}

/* The same bump allocator as in protected mode: it only allocates,
   which is enough for a program that reads a file into memory once.
   A program built against the C library (libc/, THREADOS_LIBC) has the
   real malloc, and this one would collide with it. */
#ifndef THREADOS_LIBC
static inline void *malloc(uint32_t size)
{
    size = (size + 7) & ~7u;
    void *p = sys_sbrk((int64_t)size);
    return p == (void *)-1 ? (void *)0 : p;
}
#endif

/* The clipboard - the same two calls as in protected mode */
#define CLIP_MAX 512

static inline int32_t sys_clip_set(const char *text, uint32_t len)
{
    return (int32_t)syscall64(SYS64_CLIP_SET, (int64_t)(uintptr_t)text,
                              (int64_t)len);
}

static inline int32_t sys_clip_get(char *out, uint32_t max)
{
    return (int32_t)syscall64(SYS64_CLIP_GET, (int64_t)(uintptr_t)out,
                              (int64_t)max);
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

/* A file of the initrd, read into 'buf': how many bytes, -1 = none */
static inline int64_t sys_initrd_read(const char *name, void *buf,
                                      uint64_t max)
{
    return syscall64_4(SYS64_INITRD_READ, (int64_t)(uintptr_t)name,
                       (int64_t)ustrlen(name), (int64_t)(uintptr_t)buf,
                       (int64_t)max);
}

/* A new kernel (0), initrd (1) or loader (2) written over the installed
   one: 0, -1 failed, -2 not an admin, -3 nothing installed, -4 not
   booted from the installed system, -5 backup failed, -6 rollback needed */
static inline int64_t sys_update_write(int which, const void *data,
                                       uint64_t len)
{
    return syscall64_3(SYS64_UPDATE_WRITE, which,
                       (int64_t)(uintptr_t)data, (int64_t)len);
}

static inline int64_t sys_update_rollback(void)
{
    return syscall64(SYS64_UPDATE_ROLLBACK, 0, 0);
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
