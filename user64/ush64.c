/* ush64 - a shell in user space
 *
 * An ordinary program in ring 3. It has no rights beyond those every
 * other process has: it reads the keyboard with a system call, starts
 * programs through fork and exec, waits for them through wait.
 * Everything it does could be done by any other program - and that is
 * the point.
 *
 * A shell inside the kernel was needed so that there would be anything
 * at all to drive the system with. This one is the proof that it no
 * longer has to be there.
 */

#include "usys64.h"

#define LINE_MAX 80

static char line[LINE_MAX];
static int line_len;
static char out[128];
static volatile int interrupted;

/* The shell does not die of Ctrl+C - it only aborts what was being
   typed. Without a handler of its own the signal would kill it by
   default, and then every accidental Ctrl+C would end the session. */
static void on_signal(int sig)
{
    (void)sig;
    interrupted = 1;
}

static int put(int n, const char *s)
{
    while (*s)
        out[n++] = *s++;
    return n;
}

static int putn(int n, int64_t v)
{
    char tmp[24];

    /* An exit code can be negative - a program killed by a signal
       hands back minus one. Without this that comes out as a
       twenty-digit number. */
    if (v < 0) {
        out[n++] = '-';
        v = -v;
    }
    utoa((uint64_t)v, tmp);
    return put(n, tmp);
}

static void say(const char *a, int64_t v, const char *b)
{
    int n = put(0, a);
    n = putn(n, v);
    n = put(n, b);
    out[n] = 0;
    print(out);
}

static int equal(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static int starts_with(const char *s, const char *p)
{
    while (*p)
        if (*s++ != *p++)
            return 0;
    return 1;
}

/* Starting a program: fork, exec in the child, wait in the parent.
   Exactly the same scheme every shell uses.
   We split the line into words - the first is the program, the rest are
   its arguments, and the shell takes no interest in what comes of
   them. */
#define SH_ARGS 8

static char words[SH_ARGS][32];
static char *argv[SH_ARGS + 1];

static int split(char *text)
{
    int n = 0, i = 0;
    while (text[i] && n < SH_ARGS) {
        while (text[i] == ' ')
            i++;
        if (!text[i])
            break;
        int k = 0;
        while (text[i] && text[i] != ' ' && k < 31)
            words[n][k++] = text[i++];
        words[n][k] = '\0';
        while (text[i] && text[i] != ' ')
            i++;
        argv[n] = words[n];
        n++;
    }
    argv[n] = 0;
    return n;
}

/* Two programs joined by a pipe.
 *
 * The shell creates the pipe, hands its ends out to two children and -
 * most importantly - closes both of its own. If it did not close them,
 * the second program would wait for an end of data that would never
 * come: one writer (the shell itself) would stay alive forever. */
static char words2[SH_ARGS][32];
static char *argv2[SH_ARGS + 1];

static void run_pipe(char *left, char *right)
{
    int fd[2];
    if (sys_pipe(fd) < 0) {
        print("ush64: I have nothing to make a pipe out of\n");
        return;
    }

    /* The right side first: its argv has to be built before splitting
       the left one overwrites the shared buffers */
    int n2 = 0, i = 0;
    while (right[i] && n2 < SH_ARGS) {
        while (right[i] == ' ')
            i++;
        if (!right[i])
            break;
        int k = 0;
        while (right[i] && right[i] != ' ' && k < 31)
            words2[n2][k++] = right[i++];
        words2[n2][k] = '\0';
        argv2[n2] = words2[n2];
        n2++;
    }
    argv2[n2] = 0;
    if (!n2) {
        print("ush64: there is nothing after the bar\n");
        sys_close(fd[0]);
        sys_close(fd[1]);
        return;
    }

    int n1 = split(left);
    if (!n1) {
        print("ush64: there is nothing before the bar\n");
        sys_close(fd[0]);
        sys_close(fd[1]);
        return;
    }

    int64_t a = sys_fork();
    if (a == 0) {
        sys_dup2(fd[1], FD_STDOUT);   /* the output goes into the pipe */
        sys_close(fd[0]);
        sys_close(fd[1]);
        sys_exec(argv[0], argv);
        sys_exit(127);
    }

    int64_t b = sys_fork();
    if (b == 0) {
        sys_dup2(fd[0], FD_STDIN);    /* the input comes from the pipe */
        sys_close(fd[0]);
        sys_close(fd[1]);
        sys_exec(argv2[0], argv2);
        sys_exit(127);
    }

    /* Without this the second program would wait for an end of data
   forever */
    sys_close(fd[0]);
    sys_close(fd[1]);

    int code = 0;
    sys_wait(&code);
    sys_wait(&code);
    (void)a;
    (void)b;
}

static void run(const char *name)
{
    int64_t child = sys_fork();
    if (child < 0) {
        print("ush64: I have nothing to make a process out of\n");
        return;
    }
    if (child == 0) {
        sys_exec(name, argv);
        /* exec returns only when it failed */
        print("ush64: there is no such program\n");
        sys_exit(127);
    }

    int code = 0;
    int64_t who = sys_wait(&code);
    if (who >= 0)
        say("ush64: done, code ", code, "\n");
}

static void execute(void)
{
    if (!line[0])
        return;

    if (equal(line, "help")) {
        print("ush64: help, echo <text>, pid, exit, "
              "or the name of a program from the initrd\n");
    } else if (equal(line, "pid")) {
        /* We learn the process number from what fork tells the parent -
           just like every other program */
        print("ush64: I am a process in ring 3\n");
    } else if (equal(line, "exit")) {
        print("ush64: pa!\n");
        sys_exit(0);
    } else if (starts_with(line, "echo ")) {
        print(line + 5);
        print("\n");
    } else {
        /* A bar splits the line into two programs - everything before it
           writes to what reads everything after it */
        int dash = -1;
        for (int i = 0; line[i]; i++)
            if (line[i] == '|') {
                dash = i;
                break;
            }
        if (dash >= 0) {
            line[dash] = '\0';
            run_pipe(line, line + dash + 1);
        } else {
            int n = split(line);
            if (n)
                run(argv[0]);
        }
    }
}

void _start(void)
{

    sys_signal(SIGINT, on_signal);
    print("ush64 - the user's shell. Type 'help'.\n");

    for (;;) {
        print("ush64$ ");
        line_len = 0;

        for (;;) {
            char c;
            int64_t n = sys_read(FD_STDIN, &c, 1);
            /* Nothing more will come: the input has been closed (Ctrl+D
               in the terminal, or a pipe that has ended). A shell
               reading a stream that has finished has finished too. */
            if (n == 0 && !interrupted) {
                print("\n");
                sys_exit(0);
            }
            if (n <= 0) {
                /* A signal interrupted the wait. The handler has already run -
                   what is left is to abandon the line begun and start
                   over, exactly like every shell. */
                if (interrupted) {
                    interrupted = 0;
                    print("^C\n");
                    line_len = 0;
                    break;
                }
                continue;
            }

            if (c == '\n') {
                print("\n");
                break;
            }
            if (c == '\b') {
                if (line_len) {
                    line_len--;
                    print("\b");
                }
                continue;
            }
            if (c >= ' ' && c < 0x7F && line_len < LINE_MAX - 1) {
                line[line_len++] = c;
                out[0] = c;          /* the echo is done by the reader */
                out[1] = 0;
                print(out);
            }
        }

        line[line_len] = '\0';
        execute();
    }
}
