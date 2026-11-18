/* hostile64 - a program that tries what a kernel audit said it could
 *
 * Each attempt is something an ordinary ring 3 program could ask the
 * kernel for, and each comes with an honest twin - the same call made
 * properly - because a refusal proves something only when the honest
 * version goes through (wild64 works the same way). The verdicts are
 * lines on the console that scripts/tests/t_hostile.py reads:
 *
 *   a background preview whose width and height multiply past 32 bits
 *   to a small number (the check once ran on that number)
 *
 *   a signal handler, and its return, pointed at kernel memory and at
 *   the program's own data instead of its code
 *
 *   a thread made with SYS64_CLONE whose number (parent_tid), or the
 *   zero written when it ends (clear_tid), is to go into the program's
 *   read-only data: the kernel writing there faulted in ring 0, and the
 *   machine stopped
 *
 *   an installation step from a program nobody authorized, on a
 *   system started from a stick
 *
 *   programs written to the disk and started: one honest, one with a
 *   segment both writable and executable, one whose segment's offset
 *   wraps round 2^64 to a small sum, and one whose program header
 *   table does the same
 *
 * 'hostile64 hold' leaves a child behind that sleeps for ever and says
 * its number; 'hostile64 kill N' signals N - for the test of whether
 * one person's program can kill another's. 'hostile64 guess PASSWORD'
 * guesses wrong three times, waits out the lock, guesses wrong three
 * times more, and gives the right password six seconds later and then
 * eleven: a lock that grows refuses the first. 'hostile64 listen PORT
 * SECONDS' listens on a TCP port for that long, so that the test can
 * read the kernel's first sequence numbers off its SYN-ACKs; and
 * 'hostile64 conns N IP PORT' opens N connections at once and says how
 * many it got.
 */

#include "usys64.h"

#define SYS64_INSTALL_STEP_NR 61
#define SYS64_BG_PREVIEW_NR   71
#define SYS64_CLONE_NR        83
#define OPEN_CREATE 1

static int holes, proven, unproven;

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

static void verdict(const char *what, int64_t got, int refused, int honest_ok)
{
    print("[hostile64] ");
    print(what);
    if (!refused) {
        print(": IT PASSED, the kernel returned ");
        print_signed(got);
        print("\n");
        holes++;
    } else if (!honest_ok) {
        print(": refused, but so was the honest call - nothing proven\n");
        unproven++;
    } else {
        print(": refused (");
        print_signed(got);
        print(")\n");
        proven++;
    }
}

static int same(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* --- the background preview ------------------------------------------ */

static uint32_t small_out[200 * 150];

struct bg_preview {
    int32_t bg, dark, w, h;
    uint64_t out;
};

static void preview(void)
{
    struct bg_preview honest = { 1, 0, 20, 10, (uint64_t)(uintptr_t)small_out };
    int64_t ok = syscall64(SYS64_BG_PREVIEW_NR, (int64_t)(uintptr_t)&honest, 0);
    /* 65536 * 65536 is 2^32: zero in 32 bits */
    struct bg_preview wraps = { 1, 0, 65536, 65536,
                                (uint64_t)(uintptr_t)small_out };
    int64_t got = syscall64(SYS64_BG_PREVIEW_NR, (int64_t)(uintptr_t)&wraps, 0);
    verdict("a 65536 x 65536 preview into a small buffer", got, got < 0,
            ok == 0);
}

/* --- the signal handler ------------------------------------------------ */

static void handler(int sig)
{
    (void)sig;
}

static uint8_t not_code[64];

static void signal_handler(void)
{
    int64_t ok = syscall64_3(SYS64_SIGNAL, SIGINT,
                             (int64_t)(uintptr_t)handler,
                             (int64_t)(uintptr_t)threados_sigreturn);
    int64_t got = syscall64_3(SYS64_SIGNAL, SIGINT, 0x100000,
                              (int64_t)(uintptr_t)threados_sigreturn);
    verdict("a signal handler in kernel memory", got, got < 0, ok == 0);
    got = syscall64_3(SYS64_SIGNAL, SIGINT, (int64_t)(uintptr_t)not_code,
                      (int64_t)(uintptr_t)threados_sigreturn);
    verdict("a signal handler in the program's data", got, got < 0, ok == 0);
    got = syscall64_3(SYS64_SIGNAL, SIGINT, (int64_t)(uintptr_t)handler,
                      0x100000);
    verdict("a handler's return into kernel memory", got, got < 0, ok == 0);
    syscall64_3(SYS64_SIGNAL, SIGINT, 0, 0);
}

/* --- a thread's numbers ------------------------------------------------ */

/* The kernel writes into the program's memory where SYS64_CLONE says:
   the new thread's number at once, zero when the thread ends. The
   honest twin hands it two words of its own data, and the thread it
   makes ends at once - so both words are written before the honest
   call is judged. */
struct clone64 {
    uint64_t entry, stack, tls, parent_tid, clear_tid;
};

static const uint32_t read_only_word = 7;        /* in .rodata */
static volatile uint32_t number_word, end_word;
static uint8_t thread_stacks[3][4096] __attribute__((aligned(16)));

static void thread_ends(void)
{
    sys_exit(0);                     /* this thread, not the program */
}

static int64_t clone_thread(int n, const volatile void *parent_tid,
                            const volatile void *clear_tid)
{
    /* eight below a sixteen-byte boundary, as after a call */
    struct clone64 c = {
        (uint64_t)(uintptr_t)thread_ends,
        (uint64_t)(uintptr_t)(thread_stacks[n] + sizeof(thread_stacks[n])) - 8,
        0, (uint64_t)(uintptr_t)parent_tid, (uint64_t)(uintptr_t)clear_tid,
    };
    return syscall64(SYS64_CLONE_NR, (int64_t)(uintptr_t)&c, 0);
}

static void thread_numbers(void)
{
    number_word = 0;
    end_word = 1;
    int64_t tid = clone_thread(0, &number_word, &end_word);
    for (int i = 0; i < 500 && end_word; i++)
        sys_sleep(10);
    int ok = tid > 0 && number_word == (uint32_t)tid && end_word == 0;
    print("[hostile64] the honest thread's number went where it was asked: ");
    print_signed(ok);
    print("\n");

    int64_t got = clone_thread(1, &read_only_word, 0);
    verdict("a thread's number written into read-only memory", got,
            got < 0, ok);
    got = clone_thread(2, 0, &read_only_word);
    for (int i = 0; got >= 0 && i < 20; i++)
        sys_sleep(10);               /* (had it been taken: its end) */
    verdict("a thread's end written into read-only memory", got,
            got < 0, ok);
}

/* --- installing --------------------------------------------------------- */

static void install_step(void)
{
    int64_t got = syscall64(SYS64_INSTALL_STEP_NR, 0, 0);
    /* -2 is "not yours to do"; anything else means it looked further */
    verdict("an installation step from an unauthorized program", got,
            got == -2, 1);
}

/* --- programs off the disk --------------------------------------------- */

/* The smallest program: one segment holding the headers and five
   instructions - exit(42) */
#define BASE 0x40000000ULL
#define CODE_AT 120

static uint8_t image[CODE_AT + 16];

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16)); }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }

static uint32_t make_program(uint32_t flags, uint64_t offset, uint64_t phoff)
{
    for (unsigned i = 0; i < sizeof(image); i++)
        image[i] = 0;
    image[0] = 0x7F; image[1] = 'E'; image[2] = 'L'; image[3] = 'F';
    image[4] = 2;                     /* 64-bit */
    image[5] = 1;                     /* little-endian */
    image[6] = 1;
    put16(image + 16, 2);             /* ET_EXEC */
    put16(image + 18, 62);            /* x86-64 */
    put32(image + 20, 1);
    put64(image + 24, BASE + CODE_AT);    /* e_entry */
    put64(image + 32, phoff);         /* e_phoff */
    put16(image + 52, 64);            /* e_ehsize */
    put16(image + 54, 56);            /* e_phentsize */
    put16(image + 56, 1);             /* e_phnum */
    uint8_t *ph = image + 64;
    put32(ph, 1);                     /* PT_LOAD */
    put32(ph + 4, flags);
    put64(ph + 8, offset);
    put64(ph + 16, BASE);
    put64(ph + 24, BASE);
    /* the size reaching from the offset to the end of the file, so
       that offset + size is exactly the file - or, from an offset near
       2^64, wraps round to it */
    put64(ph + 32, sizeof(image) - offset);
    put64(ph + 40, 0x2000);
    put64(ph + 48, 0x1000);
    static const uint8_t code[] = {
        0xB8, 0x02, 0x00, 0x00, 0x00,    /* mov eax, SYS64_EXIT */
        0xBF, 0x2A, 0x00, 0x00, 0x00,    /* mov edi, 42 */
        0x0F, 0x05,                      /* syscall */
        0xEB, 0xFE                       /* jmp . */
    };
    for (unsigned i = 0; i < sizeof(code); i++)
        image[CODE_AT + i] = code[i];
    return sizeof(image);
}

/* Writes the program, starts it in a child and says how the child
   ended: 42 is the program having run; -1 a start the kernel refused
   once the old image was gone (proc64_exec ends the process), 70 one
   refused before that */
static int64_t run_program(const char *name, uint32_t size)
{
    int fd = (int)sys_open(name, OPEN_CREATE);
    if (fd < 0)
        return -100;
    sys_write(fd, image, size);
    sys_close(fd);
    int64_t pid = sys_fork();
    if (pid == 0) {
        sys_exec(name, 0);
        sys_exit(70);
    }
    if (pid < 0)
        return -101;
    int code = -1;
    sys_wait(&code);
    return code;
}

static void programs(void)
{
    int64_t honest = run_program("honest.elf", make_program(5, 0, 64));
    print("[hostile64] the honest program ended with ");
    print_signed(honest);
    print("\n");
    int ok = honest == 42;

    int64_t got = run_program("rwx.elf", make_program(7, 0, 64));
    verdict("a segment both writable and executable", got,
            got == -1 || got == 70, ok);

    /* -0x1000 + (size + 0x1000) wraps to the size */
    got = run_program("wrap.elf", make_program(5, (uint64_t)-0x1000, 64));
    verdict("a segment whose offset wraps round 2^64", got,
            got == -1 || got == 70, ok);

    /* -56 + 56 wraps to 0 */
    got = run_program("phoff.elf", make_program(5, 0, (uint64_t)-56));
    verdict("a header table whose offset wraps round 2^64", got,
            got == -1 || got == 70, ok);
}

void _start(int argc, char **argv)
{
    if (argc > 1 && same(argv[1], "hold")) {
        int64_t pid = sys_fork();
        if (pid == 0) {
            for (;;)
                sys_sleep(1000);
        }
        print("[hostile64] holding, pid ");
        print_signed(pid);
        print("\n");
        sys_exit(0);
    }
    if (argc > 2 && same(argv[1], "guess")) {
        for (int round = 0; round < 2; round++) {
            for (int i = 0; i < 3; i++)
                syscall64(SYS64_AUTH, (int64_t)(uintptr_t)"not-the-password", 0);
            if (round == 0)
                sys_sleep(5500);     /* the first lock is five seconds */
        }
        sys_sleep(6000);
        int64_t early = syscall64(SYS64_AUTH, (int64_t)(uintptr_t)argv[2], 0);
        sys_sleep(5000);
        int64_t late = syscall64(SYS64_AUTH, (int64_t)(uintptr_t)argv[2], 0);
        print("[hostile64] the right password 6 s into the second lock: ");
        print_signed(early);
        print(", 11 s into it: ");
        print_signed(late);
        print("\n");
        sys_exit(0);
    }
    if (argc > 3 && same(argv[1], "listen")) {
        int64_t port = 0, secs = 0;
        for (const char *s = argv[2]; *s >= '0' && *s <= '9'; s++)
            port = port * 10 + (*s - '0');
        for (const char *s = argv[3]; *s >= '0' && *s <= '9'; s++)
            secs = secs * 10 + (*s - '0');
        int64_t sock = syscall64(SYS64_TCP_LISTEN, port, 0);
        print("[hostile64] listening on ");
        print_signed(port);
        print(sock < 0 ? ": refused\n" : "\n");
        sys_sleep((uint32_t)secs * 1000);
        print("[hostile64] listened\n");
        sys_exit(0);
    }
    if (argc > 4 && same(argv[1], "conns")) {
        int64_t want = 0, port = 0, got = 0;
        for (const char *s = argv[2]; *s >= '0' && *s <= '9'; s++)
            want = want * 10 + (*s - '0');
        for (const char *s = argv[4]; *s >= '0' && *s <= '9'; s++)
            port = port * 10 + (*s - '0');
        struct udp_info o;
        o.sock = 0;
        o.ip = (uint32_t)uparse_ip(argv[3]);
        o.port = (uint16_t)port;
        o._padding = 0;
        o.timeout = 5000;
        for (int64_t i = 0; i < want; i++)
            if (sys_tcp_connect(&o) >= 0)
                got++;
        print("[hostile64] connections open at once: ");
        print_signed(got);
        print(" of ");
        print_signed(want);
        print("\n");
        sys_sleep(1000);
        sys_exit(0);
    }
    if (argc > 2 && same(argv[1], "kill")) {
        int64_t pid = 0;
        for (const char *s = argv[2]; *s >= '0' && *s <= '9'; s++)
            pid = pid * 10 + (*s - '0');
        int64_t got = sys_kill(pid, SIGKILL);
        print("[hostile64] kill ");
        print_signed(pid);
        print(": ");
        print_signed(got);
        print("\n");
        sys_exit(0);
    }

    preview();
    signal_handler();
    thread_numbers();
    install_step();
    programs();
    print("[hostile64] done: ");
    print_signed(proven);
    print(" refused, ");
    print_signed(holes);
    print(" holes, ");
    print_signed(unproven);
    print(" unproven\n");
    sys_exit(holes ? 1 : 0);
}
