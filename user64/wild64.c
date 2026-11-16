/* wild64 - a program that reaches for kernel memory
 *
 * Twice, because there are two ways to it. First it asks the kernel to
 * do the reaching for it: every system call that takes a pointer gets
 * one into kernel memory - a buffer to print, a buffer to fill, a file
 * name, a thread's entry point. The kernel has to refuse each one,
 * because it checks whether a pointer belongs to the program at all.
 * A refusal only proves something when the same call with the
 * program's own pointer goes through, so every attempt comes with that
 * honest twin, and the verdict needs both.
 *
 * Then it reaches there itself. That attempt is no longer stopped by
 * any check in the code, but by the processor itself: a kernel page has
 * no access bit for ring 3.
 *
 * 'wild64 all <address>' adds the calls that need more than the
 * program: a file system, a child, a second thread, an address on the
 * network. The address is where the kernel image holds the text
 * "wild64" - the test reads it out of the kernel's ELF file. A name in
 * kernel memory has to name something that exists: a kernel that forgot
 * the check would then find it and say so, while a name made of
 * whatever bytes happen to lie there is "not found" either way.
 */

#include "usys64.h"

/* The Multiboot header at the start of the kernel image, with a page
   to itself: kernel memory nothing reads after the boot. A kernel that
   did write through one of these pointers would not take itself down
   with it, and the attempt could still report the hole. */
#define KERNEL_ADDR 0x100000LL

/* The port the program sends itself a datagram on */
#define WILD_PORT 7790

/* open()'s flags, as kernel64/syscall64.h numbers them */
#define OPEN_CREATE 1
#define OPEN_TRUNC  2

static int proven, holes, unproven;

static void print_signed(int64_t v)
{
    char tmp[24];
    if (v < 0) {
        print("-");
        v = -v;
    }
    utoa((uint64_t)v, tmp);
    print(tmp);
}

/* One attempt and its honest twin, and the verdict on both */
static void verdict(const char *what, int64_t got, int refused, int honest_ok)
{
    print("[wild64] ");
    print(what);
    if (!refused) {
        print(": IT PASSED, the kernel returned ");
        print_signed(got);
        print("\n");
        holes++;
    } else if (!honest_ok) {
        print(": refused, but the honest call failed too - that proves nothing\n");
        unproven++;
    } else {
        print(": refused, and with an honest pointer it works\n");
        proven++;
    }
}

static int same(const char *a, const char *b, int n)
{
    for (int i = 0; i < n; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

static int64_t parse_hex(const char *s)
{
    int64_t v = 0;
    if (s[0] == '0' && s[1] == 'x')
        s += 2;
    if (!*s)
        return 0;
    for (; *s; s++) {
        int d = *s >= '0' && *s <= '9' ? *s - '0'
              : *s >= 'a' && *s <= 'f' ? *s - 'a' + 10 : -1;
        if (d < 0)
            return 0;
        v = v * 16 + d;
    }
    return v;
}

/* Buffers the kernel reads from or writes into. None of these needs
   more than the program itself, so they run wherever it does - at boot
   too, before there is a disk. */
static void buffers(void)
{
    char own[64];
    int64_t got, honest;

    static const char hello[] =
        "[wild64] asking the kernel to print its own memory\n";
    honest = sys_write(FD_STDOUT, hello, sizeof(hello) - 1);
    got = syscall64_3(SYS64_WRITE, FD_STDOUT, KERNEL_ADDR, 16);
    verdict("write(stdout, kernel memory)", got, got == -1,
            honest == (int64_t)(sizeof(hello) - 1));

    /* A pipe is a buffer to read from that needs no keyboard and no
       disk. pipe() itself writes two numbers through its pointer. */
    int fd[2] = { -1, -1 };
    got = syscall64(SYS64_PIPE, KERNEL_ADDR, 0);
    honest = sys_pipe(fd);
    int pipe_ok = honest == 0 && fd[0] >= 0 && fd[1] >= 0;
    verdict("pipe(-> kernel memory)", got, got == -1, pipe_ok);

    /* The honest read comes second and has to find all eight bytes:
       the refused one is not to have taken any of them away. The
       writing end is closed first, so a pipe that a broken refusal
       emptied answers "end of data" instead of waiting forever. */
    static const char eight[8] = { 'e', 'i', 'g', 'h', 't', ' ', 'B', '!' };
    if (pipe_ok)
        sys_write(fd[1], eight, sizeof(eight));
    sys_close(fd[1]);
    got = syscall64_3(SYS64_READ, fd[0], KERNEL_ADDR, sizeof(eight));
    honest = sys_read(fd[0], own, sizeof(eight));
    verdict("read(pipe -> kernel memory)", got, got == -1,
            pipe_ok && honest == (int64_t)sizeof(eight) &&
            same(own, eight, sizeof(eight)));
    sys_close(fd[0]);

    struct time64 t;
    got = syscall64(SYS64_TIME, KERNEL_ADDR, 0);
    honest = sys_time(&t);
    verdict("time(-> kernel memory)", got, got == -1, honest == 0);

    struct sysinfo64 si;
    got = syscall64(SYS64_SYSINFO, KERNEL_ADDR, 0);
    honest = sys_sysinfo(&si);
    verdict("sysinfo(-> kernel memory)", got, got == -1,
            honest == 0 && si.ram_kib > 0);

    honest = sys_getcwd(own, 32);
    int cwd_ok = honest > 0 && own[0] == '/';
    got = syscall64(SYS64_GETCWD, KERNEL_ADDR, 32);
    verdict("getcwd(-> kernel memory)", got, got == -1, cwd_ok);
    got = syscall64(SYS64_GETCWD, 0, 32);
    verdict("getcwd(-> NULL)", got, got == -1, cwd_ok);

    uint32_t addrs[4];
    got = syscall64(SYS64_NETINFO, KERNEL_ADDR, 0);
    honest = sys_netinfo(addrs);
    verdict("netinfo(-> kernel memory)", got, got == -1, honest >= 0);

    /* Its own file from the initrd: an ELF, so the honest copy is easy
       to recognise */
    got = syscall64_4(SYS64_INITRD_READ, (int64_t)(uintptr_t)"wild64", 6,
                      KERNEL_ADDR, 64);
    honest = syscall64_4(SYS64_INITRD_READ, (int64_t)(uintptr_t)"wild64", 6,
                         (int64_t)(uintptr_t)own, 64);
    verdict("initrd_read(-> kernel memory)", got, got == -1,
            honest == 64 && same(own, "\177ELF", 4));
}

/* More of the same: the tables the kernel copies out, and the
   clipboard. They need no more than the program either, but they wait
   for 'all': at boot the kernel gives this program four seconds to die
   in, and every attempt is two calls and a line of text. */
static void tables(void)
{
    char own[64];
    int64_t got, honest;

    /* Row 0 of the task list is the one who asks */
    struct psinfo ps;
    got = syscall64(SYS64_PS, 0, KERNEL_ADDR);
    honest = sys_ps(0, &ps);
    verdict("ps(-> kernel memory)", got, got == -1,
            honest == 0 && ps.pid == (int)syscall64(SYS64_GETPID, 0, 0));

    uint32_t colours[256];
    got = syscall64(SYS64_PALETTE, KERNEL_ADDR, 0);
    honest = sys_palette(colours);
    verdict("palette(-> kernel memory)", got, got == -1, honest == 0);

    /* Eleven numbers: UI_LOOK_N in kernel/include/desktop.h */
    uint32_t look[16];
    got = syscall64(SYS64_UI_LOOK, KERNEL_ADDR, 0);
    honest = syscall64(SYS64_UI_LOOK, (int64_t)(uintptr_t)look, 0);
    verdict("ui_look(-> kernel memory)", got, got == -1, honest == 0);

    /* The first setting: its name, value and description */
    struct { char name[32], value[32], help[80]; } setting;
    got = syscall64(SYS64_CFG_GET, 0, KERNEL_ADDR);
    honest = syscall64(SYS64_CFG_GET, 0, (int64_t)(uintptr_t)&setting);
    verdict("cfg_get(-> kernel memory)", got, got == -1,
            honest == 0 && setting.name[0]);

    /* The first device on the PCI bus: six numbers, the vendor fourth */
    uint32_t device[6] = { 0 };
    got = syscall64(SYS64_PCI_INFO, 0, KERNEL_ADDR);
    honest = syscall64(SYS64_PCI_INFO, 0, (int64_t)(uintptr_t)device);
    verdict("pci_info(-> kernel memory)", got, got == -1,
            honest == 1 && device[3] != 0);

    /* The clipboard reads through a pointer and writes through one.
       What comes back at the end has to be the honest text: a refused
       set is not to have put anything there. */
    static const char clip[] = "wild64 was here";
    int64_t len = sizeof(clip) - 1;
    got = syscall64(SYS64_CLIP_SET, KERNEL_ADDR, 16);
    honest = syscall64(SYS64_CLIP_SET, (int64_t)(uintptr_t)clip, len);
    int set = honest == len;
    verdict("clip_set(kernel memory)", got, got == -1, set);
    got = syscall64(SYS64_CLIP_GET, KERNEL_ADDR, 64);
    honest = syscall64(SYS64_CLIP_GET, (int64_t)(uintptr_t)own, 64);
    verdict("clip_get(-> kernel memory)", got, got == -1,
            set && honest == len && same(own, clip, (int)len));
}

/* A finished child's exit code leaves the kernel through a pointer
   too. Two children, each ending at once with a code of its own: the
   kernel forgets a child the moment somebody asks about it, so the
   refused question uses one up and the honest one needs another. */
static void children(void)
{
    int64_t kid[2];
    for (int i = 0; i < 2; i++) {
        kid[i] = sys_fork();
        if (kid[i] == 0)
            sys_exit(40 + i);
    }
    if (kid[0] < 0 || kid[1] < 0) {
        print("[wild64] trywait(code -> kernel memory): no children to "
              "ask about - that proves nothing\n");
        unproven++;
        return;
    }

    /* -3 is "still running" - not an answer yet */
    int64_t got = -3, honest = -3;
    int code = -1;
    for (int i = 0; i < 300 && got == -3; i++) {
        got = syscall64(SYS64_TRYWAIT, kid[0], KERNEL_ADDR);
        if (got == -3)
            sys_sleep(10);
    }
    for (int i = 0; i < 300 && honest == -3; i++) {
        honest = syscall64(SYS64_TRYWAIT, kid[1], (int64_t)(uintptr_t)&code);
        if (honest == -3)
            sys_sleep(10);
    }
    verdict("trywait(code -> kernel memory)", got, got == -1,
            honest == 0 && code == 41);
}

/* A datagram's bytes, on the way out and on the way in. The honest
   twin sends to the machine's own address, so the system has to have
   one: the test waits for the lease before it starts this. */
static void datagrams(void)
{
    static const char msg[] = "a datagram to myself";
    int64_t len = sizeof(msg) - 1;
    char own[64];
    uint32_t addrs[4] = { 0 };
    sys_netinfo(addrs);
    int64_t sock = sys_udp_open(WILD_PORT);
    struct udp_info to = { (int)sock, addrs[0], WILD_PORT, 0, 0 };

    int64_t got = sys_udp_send(&to, (const void *)(uintptr_t)KERNEL_ADDR, 16);
    int64_t honest = sys_udp_send(&to, msg, (uint64_t)len);
    int sent = sock >= 0 && honest == len;
    verdict("udp_send(kernel memory)", got, got == -1, sent);

    /* The honest receive comes second and has to find the datagram
       whole: the refused one is not to have taken it out of the
       queue */
    struct udp_info from = { (int)sock, 0, 0, 0, 0 };
    got = sys_udp_recv(&from, (void *)(uintptr_t)KERNEL_ADDR, 64);
    from.timeout = 1000;
    honest = sys_udp_recv(&from, own, 64);
    verdict("udp_recv(-> kernel memory)", got, got == -1,
            sent && honest == len && same(own, msg, (int)len));
    sys_udp_close((int)sock);
}

static volatile int thread_ran;

static void honest_thread(void *arg)
{
    thread_ran = (int)(uintptr_t)arg;
    sys_exit(0);
}

/* File names in kernel memory, and the rest of what needs a disk or a
   second thread. 'kname' points at "wild64" in the kernel image: six
   bytes of it name this program in the initrd, the first four the file
   'wild' these calls make in the working directory. Each attempt comes
   while the thing its name names exists, so only the pointer check
   stands between it and success. */
static void names_and_threads(int64_t kname)
{
    char own[64];
    int64_t got, honest;

    got = syscall64_4(SYS64_INITRD_READ, kname, 6,
                      (int64_t)(uintptr_t)own, 64);
    honest = syscall64_4(SYS64_INITRD_READ, (int64_t)(uintptr_t)"wild64", 6,
                         (int64_t)(uintptr_t)own, 64);
    verdict("initrd_read(kernel name)", got, got == -1, honest == 64);

    /* First as a directory: made, entered, removed */
    got = syscall64(SYS64_MKDIR, kname, 4);
    honest = sys_mkdir("wild");
    verdict("mkdir(kernel name)", got, got == -1, honest == 0);

    got = syscall64(SYS64_CHDIR, kname, 4);
    honest = sys_chdir("wild");
    int64_t len = sys_getcwd(own, 64);
    int inside = honest == 0 && len >= 5 && same(own + len - 5, "/wild", 5);
    verdict("chdir(kernel name)", got, got == -1,
            inside && sys_chdir("..") == 0);

    got = syscall64(SYS64_UNLINK, kname, 4);
    honest = sys_unlink("wild");
    verdict("unlink(kernel name)", got, got == -1, honest == 0);

    /* Then as a file of ten bytes */
    int64_t fd = syscall64_3(SYS64_OPEN, (int64_t)(uintptr_t)"wild", 4,
                             OPEN_CREATE | OPEN_TRUNC);
    int made = fd >= 0 && sys_write((int)fd, "ten bytes!", 10) == 10;
    sys_close((int)fd);

    got = syscall64_3(SYS64_OPEN, kname, 4, 0);
    fd = syscall64_3(SYS64_OPEN, (int64_t)(uintptr_t)"wild", 4, 0);
    verdict("open(kernel name)", got, got == -1, made && fd >= 0);

    struct fdinfo64 fi;
    got = syscall64(SYS64_FDINFO, fd, KERNEL_ADDR);
    honest = sys_fdinfo((int)fd, &fi);
    verdict("fdinfo(-> kernel memory)", got, got == -1,
            honest == 0 && fi.kind == FDK64_FILE && fi.size == 10);
    sys_close((int)fd);

    struct dirent64 d;
    got = syscall64(SYS64_READDIR, 0, KERNEL_ADDR);
    honest = sys_readdir(0, &d);
    verdict("readdir(-> kernel memory)", got, got == -1, honest == 0);

    /* A refused mapping is address zero, not -1 */
    uint64_t size = 0;
    got = syscall64_4(SYS64_MMAP_FILE, kname, 4, (int64_t)(uintptr_t)&size, 0);
    honest = syscall64_4(SYS64_MMAP_FILE, (int64_t)(uintptr_t)"wild", 4,
                         (int64_t)(uintptr_t)&size, 0);
    int mapped = honest != 0 && size == 10;
    verdict("mmap_file(kernel name)", got, got == 0, mapped);
    got = syscall64_4(SYS64_MMAP_FILE, (int64_t)(uintptr_t)"wild", 4,
                      KERNEL_ADDR, 0);
    verdict("mmap_file(size -> kernel memory)", got, got == 0, mapped);

    /* Two names, either of them in kernel memory. As the new name the
       kernel's text is taken whole: "wild64" is free here, so the
       rename would go through if nothing looked at the pointer. */
    got = syscall64_4(SYS64_RENAME, kname, 4,
                      (int64_t)(uintptr_t)"wild2", 5);
    int64_t got_to = syscall64_4(SYS64_RENAME, (int64_t)(uintptr_t)"wild", 4,
                                 kname, 6);
    honest = sys_rename("wild", "wild2");
    int renamed = honest == 0 && sys_unlink("wild2") == 0;
    verdict("rename(kernel name)", got, got == -1, renamed);
    verdict("rename(-> kernel name)", got_to, got_to == -1, renamed);

    got = syscall64(SYS64_THREAD, KERNEL_ADDR, 0);
    honest = sys_thread(honest_thread, (void *)7);
    for (int i = 0; i < 300 && thread_ran != 7; i++)
        sys_sleep(10);
    verdict("thread(kernel entry)", got, got == -1,
            honest >= 0 && thread_ran == 7);
}

void _start(int argc, char **argv)
{
    buffers();
    if (argc > 2 && argv[1][0] == 'a' && argv[1][1] == 'l' &&
        argv[1][2] == 'l' && !argv[1][3]) {
        int64_t kname = parse_hex(argv[2]);
        if (kname) {
            tables();
            children();
            names_and_threads(kname);
            datagrams();
        } else
            print("[wild64] ERROR: 'all' needs the address of \"wild64\" "
                  "in the kernel image\n");
    }

    char n[24];
    utoa((uint64_t)proven, n);
    if (!holes && !unproven) {
        print("[wild64] the kernel refused all ");
        print(n);
        print(" kernel pointers - good\n");
    } else {
        print("[wild64] ERROR: ");
        print(n);
        print(" refusals proven, ");
        utoa((uint64_t)holes, n);
        print(n);
        print(" passed, ");
        utoa((uint64_t)unproven, n);
        print(n);
        print(" proved nothing\n");
    }

    print("[wild64] then I will reach in myself - this should kill me\n");
    volatile unsigned char *p = (volatile unsigned char *)(uintptr_t)KERNEL_ADDR;
    unsigned char v = *p;
    (void)v;

    print("[wild64] ERROR: I read the kernel's memory!\n");
    sys_exit(1);
}
