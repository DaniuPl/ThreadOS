/* argtest - long command lines, and programs named by their path
 *
 * A compiler hands its linker some thirty paths, and finds that linker
 * by a path of its own: /usr/bin/ld.lld, beside itself. Both used to be
 * lost on the way into the kernel - at most sixteen arguments of 255
 * characters reached a program, the rest dropped without a word, and
 * execve passed on only the path's last part, which the kernel looked
 * for in the initrd and in the working directory.
 *
 * Run from the disk, this starts a copy of itself that lies deep in
 * it (scripts/tests/t_args.py puts it at /usr/libexec/threados/argecho
 * - a name in neither place) with three hundred arguments, eleven
 * kilobytes of them, one of 3000 characters. The copy says how many it
 * got, how many bytes they were and an FNV-1a hash of them; the test
 * works the same three out by itself. Then it asks for 70 KB of
 * arguments, past the 64 KiB the kernel takes, and is to be told so
 * (E2BIG) rather than have the program started with part of them.
 *
 *   argtest           the whole of the above
 *   argtest count ... prints how many arguments it got and the last
 */

#include <errno.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

#define ECHO_PATH "/usr/libexec/threados/argecho"
#define MANY 300
#define LONG_ARG 3000

static uint32_t fnv(uint32_t h, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        h ^= (unsigned char)s[i];
        h *= 16777619u;
    }
    return h;
}

/* Every argument with its zero, as they lie in memory */
static void summary(const char *who, int argc, char **argv)
{
    uint32_t h = 2166136261u;
    size_t bytes = 0;
    for (int i = 0; i < argc; i++) {
        size_t n = strlen(argv[i]) + 1;
        h = fnv(h, argv[i], n);
        bytes += n;
    }
    printf("%s: argc=%d bytes=%zu fnv=%08x\n", who, argc, bytes, h);
}

static int spawn_wait(char **args, int *status)
{
    pid_t pid;
    int e = posix_spawn(&pid, args[0], 0, 0, args, 0);
    if (e)
        return e;
    if (waitpid(pid, status, 0) != pid)
        return errno;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 0 && strstr(argv[0], "argecho")) {
        printf("argecho: argv[0]=%s\n", argv[0]);
        summary("argecho", argc, argv);
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "count")) {
        printf("argtest: argc=%d last=%s\n", argc, argv[argc - 1]);
        return 0;
    }

    /* The many: arg001-x ... arg299-xxx..., then one 3000 'L's long */
    static char store[MANY][64];
    static char longarg[LONG_ARG + 1];
    char *args[MANY + 2];
    args[0] = ECHO_PATH;
    for (int i = 1; i < MANY; i++) {
        int n = snprintf(store[i], sizeof(store[i]), "arg%03d-", i);
        memset(store[i] + n, 'x', (size_t)(i % 40));
        store[i][n + i % 40] = '\0';
        args[i] = store[i];
    }
    memset(longarg, 'L', LONG_ARG);
    args[MANY] = longarg;
    args[MANY + 1] = 0;
    summary("argtest", MANY + 1, args);
    fflush(stdout);

    int status = -1;
    int e = spawn_wait(args, &status);
    printf("argtest: started by its path: %s\n", e ? strerror(e) : "yes");
    printf("argtest: and it ended with %d\n",
           WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    fflush(stdout);

    /* 1100 arguments of 63 characters and their zeros: 70400 bytes */
    enum { BIG = 1100 };
    static char big[64];
    static char *bigargs[BIG + 1];
    memset(big, 'B', 63);
    bigargs[0] = ECHO_PATH;
    for (int i = 1; i < BIG; i++)
        bigargs[i] = big;
    bigargs[BIG] = 0;
    e = spawn_wait(bigargs, &status);
    printf("argtest: 70 KB of arguments refused with E2BIG: %s (%s)\n",
           e == E2BIG ? "yes" : "no", e ? strerror(e) : "it started");
    return 0;
}
