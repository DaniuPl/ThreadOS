/* ush - the ProtonOS user shell, running in ring 3
 *
 * It reads the keyboard with the read syscall (taking the console over
 * from the kernel's shell), starts programs from the initrd through
 * spawn and waits for them through wait. The 'exit' command hands the
 * console back to the kernel.
 */

#include "usys.h"

#define LINE_MAX 64

static char line[LINE_MAX];

static void print_help(void)
{
    print("ush: wpisz nazwe programu z initrd (np. hello), albo:\n"
          "  help        - ta pomoc\n"
          "  pid         - pokaz pid shella\n"
          "  cd [kat]    - zmien katalog (bez arg = /)\n"
          "  pwd         - pokaz katalog roboczy\n"
          "  cmd > plik  - przekieruj wyjscie do pliku (>> dopisuje)\n"
          "  cmd1 | cmd2 - polacz programy potokiem\n"
          "  exit        - wyjdz do shella jadra\n");
}

static void print_pid(void)
{
    char buf[12];
    int n = 0;
    int32_t pid = sys_getpid();
    do {
        buf[n++] = (char)('0' + pid % 10);
        pid /= 10;
    } while (pid);
    print("ush: moj pid to ");
    while (n)
        sys_write(STDOUT, &buf[--n], 1);
    print("\n");
}

/* Shows the non-zero exit code of the last command (like $? in bash) */
static void report_code(int32_t code)
{
    if (code == 0)
        return;
    print("ush: [kod ");
    if (code < 0) {
        print("-");
        code = -code;
    }
    print_dec((uint32_t)code);
    print("]\n");
}

static uint32_t read_line(void)
{
    uint32_t n = 0;
    for (;;) {
        char c;
        int32_t got = sys_read(STDIN, &c, 1);
        /* Nothing more will come: whoever was typing has closed the
           input (Ctrl+D in the terminal, or a pipe that has ended). A
           shell reading a stream that has finished has finished too -
           spinning here forever is what it used to do. */
        if (got == 0) {
            print("\n");
            sys_exit(0);
        }
        if (got != 1)
            continue;
        if (c == '\n') {
            print("\n");
            break;
        }
        if (c == '\b') {
            if (n) {
                n--;
                print("\b");
            }
            continue;
        }
        if (n < LINE_MAX - 1) {
            line[n++] = c;
            sys_write(STDOUT, &c, 1);   /* the echo is done by the shell, not by the kernel */
        }
    }
    line[n] = '\0';
    return n;
}

void _start(void)
{
    print("\nush - shell uzytkownika (ring 3). Wpisz 'help'.\n");

    for (;;) {
        char cwd[128];
        sys_getcwd(cwd);
        print("ush:");
        print(cwd);
        print("> ");
        if (!read_line())
            continue;

        if (ustreq(line, "exit")) {
            print("ush: pa!\n");
            sys_exit(0);
        }
        if (ustreq(line, "help")) {
            print_help();
            continue;
        }
        if (ustreq(line, "pid")) {
            print_pid();
            continue;
        }
        if (ustreq(line, "pwd")) {
            sys_getcwd(cwd);
            print(cwd);
            print("\n");
            continue;
        }
        /* cd [directory] - built in, because it changes the shell's own cwd */
        if (line[0] == 'c' && line[1] == 'd' &&
            (line[2] == '\0' || line[2] == ' ')) {
            char *arg = line + 2;
            while (*arg == ' ')
                arg++;
            const char *target = *arg ? arg : "/";
            if (sys_chdir(target) < 0) {
                print("ush: nie ma katalogu '");
                print(target);
                print("'\n");
            }
            continue;
        }
        /* kill <pid> - mark a process to be killed */
        if (line[0] == 'k' && line[1] == 'i' && line[2] == 'l' &&
            line[3] == 'l' && (line[4] == ' ' || line[4] == '\0')) {
            char *a = line + 4;
            while (*a == ' ')
                a++;
            if (!*a) {
                print("uzycie: kill <pid>\n");
            } else if (sys_kill(uatoi(a), SIGKILL) < 0) {
                print("ush: nie ma procesu o pid ");
                print(a);
                print("\n");
            }
            continue;
        }

        /* Starting in the background: a trailing '&' (the shell does not wait
   for the process) */
        int bg = 0;
        {
            int e = (int)ustrlen(line) - 1;
            while (e >= 0 && line[e] == ' ')
                e--;
            if (e >= 0 && line[e] == '&') {
                bg = 1;
                line[e] = '\0';
                while (e > 0 && line[e - 1] == ' ')
                    line[--e] = '\0';
            }
        }

        /* A multi-stage pipeline: 'a | b | c' (optionally '... > file').
           The kernel splits the line on '|', so here we only detect '|',
           cut off a trailing '> file' and hand the rest over. */
        int has_pipe = 0;
        for (char *s = line; *s; s++)
            if (*s == '|') {
                has_pipe = 1;
                break;
            }
        if (has_pipe) {
            char *outfile = (void *)0;
            for (char *r = line; *r; r++)
                if (*r == '>') {
                    *r = '\0';
                    char *f = r + 1;
                    if (*f == '>')      /* '>>' in a pipeline: it truncates like '>' */
                        f++;
                    while (*f == ' ')
                        f++;
                    for (char *e = f; *e; e++)
                        if (*e == ' ') {
                            *e = '\0';
                            break;
                        }
                    outfile = f;
                    break;
                }
            int32_t pid = sys_pipeline(line, outfile);
            if (pid < 0)
                print("ush: nie moge uruchomic potoku\n");
            else
                report_code(sys_wait(pid));
            goto next;
        }

        /* Redirection: '> file' truncates, '>> file' appends */
        char *outfile = (void *)0;
        int append = 0;
        for (char *s = line; *s; s++) {
            if (*s == '>') {
                *s = '\0';
                char *f = s + 1;
                if (*f == '>') {        /* '>>' = appending */
                    append = 1;
                    f++;
                }
                while (*f == ' ')
                    f++;
                outfile = f;
                /* trim the spaces at the end of the file name */
                for (char *e = f; *e; e++)
                    if (*e == ' ') {
                        *e = '\0';
                        break;
                    }
                break;
            }
        }

        int32_t pid = sys_spawn(line, outfile, append);
        if (pid < 0) {
            print("ush: nie znam '");
            print(line);
            print("'\n");
            continue;
        }
        if (bg) {                  /* in the background: we do not wait, we show the pid */
            print("ush: w tle, pid=");
            print_dec((uint32_t)pid);
            print("\n");
        } else {
            report_code(sys_wait(pid));
            if (outfile) {
                print("ush: wyjscie zapisane do '");
                print(outfile);
                print("'\n");
            }
        }
    next:;
    }
}
