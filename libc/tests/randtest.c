/* randtest - the kernel's random bytes, as a C program sees them
 *
 * Through getrandom(), getentropy() and /dev/urandom, the ways libraries
 * ask (Mbed TLS reads the file). It writes a megabyte from getrandom()
 * and a quarter of one from the file for the test to judge on the
 * host, and checks what only the inside can see: that
 * a forked child and its parent do not get the same bytes, and that
 * threads drawing at the same time never get the same bytes either.
 *
 *   randtest <getrandom's megabyte> <256 KiB of /dev/urandom>
 *
 * With "-" for both, on a system with no disk to write (the ARM64
 * kernel, scripts/tests/t_arm64rand.py), the first 64 KiB of each are
 * printed instead, in hex, on lines starting "=rand " and "=dev ".
 *
 * Lines starting with '~' differ from run to run.
 */

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define MEGABYTE (1 << 20)
#define DEVBYTES (256 * 1024)
#define THREADS 4
#define DRAWS 2000

static unsigned char drawn[THREADS][DRAWS][16];

static void hex(const char *tag, const unsigned char *p, int n)
{
    printf("~%s ", tag);
    for (int i = 0; i < n; i++)
        printf("%02x", p[i]);
    printf("\n");
}

static void *drawer(void *arg)
{
    int t = (int)(long)arg;
    for (int i = 0; i < DRAWS; i++)
        if (getrandom(drawn[t][i], 16, 0) != 16)
            return (void *)1;
    return 0;
}

static int cmp16(const void *a, const void *b)
{
    return memcmp(a, b, 16);
}

#define SHOWN (64 * 1024)

/* The bytes into the file named, or - with "-" - the first SHOWN of
   them onto the console, 64 a line */
static int keep(const char *name, const char *tag, const unsigned char *p,
                size_t n)
{
    if (strcmp(name, "-")) {
        FILE *f = fopen(name, "wb");
        return f && fwrite(p, 1, n, f) == n && !fclose(f) ? 0 : -1;
    }
    for (size_t at = 0; at < n && at < SHOWN; at += 64) {
        printf("=%s ", tag);
        for (size_t i = at; i < at + 64 && i < n; i++)
            printf("%02x", p[i]);
        printf("\n");
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        printf("usage: randtest <file> <file>\n");
        return 2;
    }
    int bad = 0;

    /* A megabyte in pieces of every size up to a few pages: the ends
       of the pieces are where a generator that loses its place shows */
    unsigned char *mb = malloc(MEGABYTE);
    size_t have = 0, calls = 0;
    while (have < MEGABYTE) {
        size_t want = (calls * 997) % 9000 + 1;
        if (want > MEGABYTE - have)
            want = MEGABYTE - have;
        ssize_t r = getrandom(mb + have, want, 0);
        if (r != (ssize_t)want) {
            printf("getrandom(%zu) gave %zd\n", want, r);
            bad = 1;
            break;
        }
        have += want;
        calls++;
    }
    if (keep(argv[1], "rand", mb, have)) {
        printf("could not write %s\n", argv[1]);
        return 1;
    }
    printf("wrote %zu bytes to %s\n", have, argv[1]);

    /* The file libraries open when they do not know getrandom() */
    FILE *u = fopen("/dev/urandom", "rb");
    struct stat st;
    if (!u || fstat(fileno(u), &st) || !S_ISCHR(st.st_mode)) {
        printf("/dev/urandom: %s\n", u ? "not a character device"
                                       : strerror(errno));
        bad = 1;
    } else {
        size_t moved = 0;
        while (moved < DEVBYTES) {
            size_t n = fread(mb + moved, 1, 9000, u);
            if (n != 9000)
                break;
            n = n > DEVBYTES - moved ? DEVBYTES - moved : n;
            moved += n;
        }
        fclose(u);
        if (keep(argv[2], "dev", mb, moved))
            printf("could not write %s\n", argv[2]);
        printf("/dev/urandom: a character device, %zu bytes read into %s\n",
               moved, argv[2]);
    }

    unsigned char b[256];
    ssize_t r = getrandom(b, 16, GRND_NONBLOCK);
    printf("GRND_NONBLOCK once seeded: %zd bytes\n", r);
    printf("getentropy of 256 bytes: %s\n",
           getentropy(b, 256) == 0 ? "ok" : strerror(errno));

    /* After fork both sides draw; the child hands its bytes over */
    int fds[2];
    if (pipe(fds) == 0) {
        pid_t pid = fork();
        if (pid == 0) {
            unsigned char c[32];
            getrandom(c, sizeof(c), 0);
            write(fds[1], c, sizeof(c));
            _exit(0);
        }
        unsigned char mine[32], theirs[32];
        getrandom(mine, sizeof(mine), 0);
        size_t got = 0;
        while (got < sizeof(theirs)) {
            ssize_t n = read(fds[0], theirs + got, sizeof(theirs) - got);
            if (n <= 0)
                break;
            got += (size_t)n;
        }
        int status = 0;
        waitpid(pid, &status, 0);
        printf("after fork: %s\n", got != sizeof(theirs) ? "the child sent nothing"
               : memcmp(mine, theirs, 32) ? "the parent's and the child's bytes differ"
               : "the parent and the child got THE SAME bytes");
    }

    pthread_t th[THREADS];
    for (int t = 0; t < THREADS; t++)
        pthread_create(&th[t], 0, drawer, (void *)(long)t);
    int failed = 0;
    for (int t = 0; t < THREADS; t++) {
        void *ret;
        pthread_join(th[t], &ret);
        failed |= ret != 0;
    }
    qsort(drawn, THREADS * DRAWS, 16, cmp16);
    int same = 0;
    for (int i = 1; i < THREADS * DRAWS; i++)
        same += !memcmp(drawn[0][i - 1], drawn[0][i], 16);
    printf("%d threads, %d draws of 16 bytes: %s, %d the same\n", THREADS,
           THREADS * DRAWS, failed ? "some failed" : "all answered", same);

    getrandom(b, 32, 0);
    hex("sample", b, 32);
    printf("done\n");
    return bad;
}
