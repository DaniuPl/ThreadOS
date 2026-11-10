/* heap64 - the heap as seen from a program
 *
 * Until now every buffer had to be hard-coded into the program. This
 * one asks the kernel for memory as it runs and uses it.
 *
 * The most interesting part is what that request costs. The program
 * asks for four megabytes and then touches only four pages - and
 * exactly that much memory should be gone, because pages appear only
 * under your fingers. The program does not see that itself; the kernel
 * does, and says so afterwards.
 *
 * Memory the kernel hands out has to come zeroed: the frame under a
 * fresh page held somebody else's data a moment ago - another
 * program's, or the kernel's own. So before it writes anything the
 * program checks that every byte it was given reads as zero, on the
 * heap and in the mapping area alike.
 *
 * On a machine that has just booted that proves little. The kernel
 * hands frames out going round in a circle, and the frames ahead of it
 * have never held anything. 'heap64 reuse' goes round that circle
 * first: it takes, dirties and gives back more pages than the machine
 * has frames, so the frames it checks afterwards are ones it dirtied
 * itself, and a kernel that skipped the zeroing would hand its own
 * dirt back to it.
 *
 * A page has two ways in, and each clears its frame on its own. The
 * program's first touch is a page fault. The kernel's first touch is
 * the check it makes on a buffer before a system call writes into it:
 * memory asked for and never touched is still the program's, so the
 * kernel brings the page in there and then. The program has it do that
 * once on the heap and once in the mapping area - a read() from a pipe
 * into a page it has not touched - and looks at the bytes around what
 * was read. That is one page more on the bill of each.
 *
 * It checks itself: if any byte came back different, it ends with an
 * error rather than a message somebody has to read.
 */

#include "usys64.h"

#define REQUEST  (4 * 1024 * 1024)
#define PAGE_MARK  4096
#define TOUCH 4
/* The mapping area: how much is asked for and how much of it touched */
#define MAP_ASK 64
#define MAP_TOUCH 2
/* How many more pages of each the kernel brings in for a read() */
#define BY_KERNEL 1
/* What it reads there and where in the page: clear of the page's ends
   and of the bytes the round dirties, so that everything around it is
   still what the frame came with */
#define MSG "read by the kernel"
#define MSG_LEN ((int)sizeof(MSG) - 1)
#define MSG_AT 100
/* Where the dirt goes in every page of the round: one byte in each
   eighth of it is enough for a check that reads every byte */
#define DIRT_STEP 512

static char out[128];

static int put(int n, const char *s)
{
    while (*s)
        out[n++] = *s++;
    return n;
}

static int putn(int n, uint64_t v)
{
    char tmp[24];
    utoa(v, tmp);
    return put(n, tmp);
}

static int puth(int n, uint64_t v)
{
    char tmp[24];
    uhtoa(v, tmp);
    return put(n, tmp);
}

static void say(int n)
{
    out[n] = 0;
    print(out);
}

static void fail_with(const char *what)
{
    print("[heap64] ERROR: ");
    print(what);
    print("\n");
    sys_exit(1);
}

/* Every byte of 'p' from 'from' up to 'to' has to read as zero. On
   pages nobody has touched, reading is what brings them in, so this is
   the first thing that ever sees them. */
static void expect_zeros(const volatile char *p, int from, int to,
                         const char *where)
{
    for (int i = from; i < to; i++)
        if (p[i]) {
            int n = put(0, "[heap64] ERROR: a fresh page ");
            n = put(n, where);
            n = put(n, " held ");
            n = puth(n, (uint8_t)p[i]);
            n = put(n, " at byte ");
            n = putn(n, (uint64_t)i);
            n = put(n, " - somebody's old data\n");
            say(n);
            sys_exit(1);
        }
}

static void write_and_check(char *p, int pages)
{
    for (int i = 0; i < pages; i++)
        for (int j = 0; j < PAGE_MARK; j++)
            p[i * PAGE_MARK + j] = (char)(i * 31 + j);

    for (int i = 0; i < pages; i++)
        for (int j = 0; j < PAGE_MARK; j++)
            if (p[i * PAGE_MARK + j] != (char)(i * 31 + j))
                fail_with("the memory gives back something other than what was written");
}

/* A page nobody has touched, and the kernel is the first to: a read()
   into the middle of it. The call has to go through - the page is the
   program's own - and around the bytes it read the page has to be as
   clean as one the program brought in itself. */
static void kernel_brings_in(char *page, const char *where)
{
    int fd[2];
    if (sys_pipe(fd) < 0 ||
        sys_write(fd[1], MSG, MSG_LEN) != MSG_LEN)
        fail_with("no pipe to read from");
    if (sys_read(fd[0], page + MSG_AT, MSG_LEN) != MSG_LEN)
        fail_with("the kernel turned away a page of mine I had not touched yet");
    sys_close(fd[0]);
    sys_close(fd[1]);

    expect_zeros(page, 0, MSG_AT, where);
    for (int i = 0; i < MSG_LEN; i++)
        if (page[MSG_AT + i] != MSG[i])
            fail_with("the page does not hold what the kernel read into it");
    expect_zeros(page, MSG_AT + MSG_LEN, PAGE_MARK, where);

    int n = put(0, "[heap64] a page ");
    n = put(n, where);
    n = put(n, ": zeros around what it read\n");
    say(n);
}

/* Takes REQUEST, dirties every page of it and gives it back, until
   more pages have gone through than the machine has frames. The kernel
   hands frames out from where it stopped last time and never steps
   back, so after that many it has been all the way round, and what
   lies ahead of it is what this loop left there. Returns how many
   pages went through. */
static uint64_t go_round(uint64_t frames)
{
    uint64_t cycled = 0;
    while (cycled <= frames + REQUEST / PAGE_MARK) {
        volatile char *p = (volatile char *)sys_sbrk(REQUEST);
        if (!p)
            fail_with("sbrk refused while going round");
        for (int i = 0; i < REQUEST; i += DIRT_STEP)
            p[i] = (char)(0xA5 ^ (i / DIRT_STEP % 8));
        if (!sys_sbrk(-REQUEST))
            fail_with("I cannot give the memory back while going round");
        cycled += REQUEST / PAGE_MARK;
    }
    return cycled;
}

void _start(int argc, char **argv)
{
    int reuse = argc > 1 && argv[1][0] == 'r';

    void *start = sys_sbrk(0);
    if (!start)
        fail_with("I have no heap");

    if (reuse) {
        struct sysinfo64 si;
        if (sys_sysinfo(&si) != 0 || !si.ram_kib)
            fail_with("the kernel does not say how much memory there is");
        uint64_t frames = si.ram_kib / 4;
        uint64_t cycled = go_round(frames);
        int n = put(0, "[heap64] went round: ");
        n = putn(n, cycled);
        n = put(n, " pages taken, dirtied and given back; the machine has ");
        n = putn(n, frames);
        n = put(n, " frames\n");
        say(n);
    }

    int n = put(0, "[heap64] the heap starts at ");
    n = puth(n, (uint64_t)(uintptr_t)start);
    n = put(n, ", asking for ");
    n = putn(n, REQUEST / 1024);
    n = put(n, " KiB (");
    n = putn(n, REQUEST / PAGE_MARK);
    n = put(n, " pages)\n");
    say(n);

    char *memory = (char *)sys_sbrk(REQUEST);
    if (!memory)
        fail_with("sbrk refused");

    /* We touch only a few pages - the rest is not to exist */
    expect_zeros(memory, 0, TOUCH * PAGE_MARK, "on the heap");
    n = put(0, "[heap64] the ");
    n = putn(n, TOUCH);
    n = put(n, " heap pages read as zeros before I wrote to them\n");
    say(n);
    write_and_check(memory, TOUCH);

    n = put(0, "[heap64] I touched ");
    n = putn(n, TOUCH);
    n = put(n, " of them, and every one gave back what I wrote\n");
    say(n);

    /* The next page along is still nobody's */
    kernel_brings_in(memory + TOUCH * PAGE_MARK,
                     "the kernel brought in on the heap");

    /* Shrinking gives the memory back at once */
    if (!sys_sbrk(-REQUEST))
        fail_with("I cannot give the memory back");
    if (sys_sbrk(0) != start)
        fail_with("the break did not come back where it was");
    print("[heap64] I gave it all back, and the break came home\n");

    /* A request beyond our own memory has to be rejected */
    if (sys_sbrk(-4096))
        fail_with("the kernel let me go below the heap");
    print("[heap64] below its own heap the kernel does not let go\n");

    /* The mapping area: the other place fresh pages come from */
    char *mapped = (char *)sys_mmap(MAP_ASK * PAGE_MARK);
    if (!mapped)
        fail_with("mmap refused");
    expect_zeros(mapped, 0, MAP_TOUCH * PAGE_MARK, "in the mapping area");
    write_and_check(mapped, MAP_TOUCH);
    n = put(0, "[heap64] the mapping area: asked for ");
    n = putn(n, MAP_ASK);
    n = put(n, " pages, touched ");
    n = putn(n, MAP_TOUCH);
    n = put(n, " - zeros at first, then what I wrote\n");
    say(n);
    kernel_brings_in(mapped + MAP_TOUCH * PAGE_MARK,
                     "the kernel brought in in the mapping area");

    sys_exit(0);
}
