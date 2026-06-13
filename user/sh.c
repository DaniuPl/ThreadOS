/* ush - shell uzytkownika ProtonOS, dziala w ring 3
 *
 * Czyta klawiature syscallem read (przejmujac konsole od shella
 * jadra), uruchamia programy z initrd przez spawn i czeka na nie
 * przez wait. Komenda 'exit' oddaje konsole jadru.
 */

#include "usys.h"

#define LINE_MAX 64

static char line[LINE_MAX];

static void print_help(void)
{
    print("ush: wpisz nazwe programu z initrd (np. hello), albo:\n"
          "  help        - ta pomoc\n"
          "  pid         - pokaz pid shella\n"
          "  cmd > plik  - przekieruj wyjscie programu do pliku\n"
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

static uint32_t read_line(void)
{
    uint32_t n = 0;
    for (;;) {
        char c;
        if (sys_read(STDIN, &c, 1) != 1)
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
            sys_write(STDOUT, &c, 1);   /* echo robi shell, nie jadro */
        }
    }
    line[n] = '\0';
    return n;
}

void _start(void)
{
    print("\nush - shell uzytkownika (ring 3). Wpisz 'help'.\n");

    for (;;) {
        print("ush> ");
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

        /* Potok: 'cmd1 | cmd2' (opcjonalnie 'cmd2 > plik') */
        for (char *s = line; *s; s++) {
            if (*s == '|') {
                *s = '\0';
                char *right = s + 1;
                while (*right == ' ')
                    right++;
                /* opcjonalne '>' po prawej stronie potoku */
                char *outfile = (void *)0;
                for (char *r = right; *r; r++)
                    if (*r == '>') {
                        *r = '\0';
                        char *f = r + 1;
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
                int32_t pid = sys_pipeline(line, right, outfile);
                if (pid < 0)
                    print("ush: nie moge uruchomic potoku\n");
                else
                    sys_wait(pid);
                goto next;
            }
        }

        /* Przekierowanie: 'cmd args > plik' kieruje stdout do pliku */
        char *outfile = (void *)0;
        for (char *s = line; *s; s++) {
            if (*s == '>') {
                *s = '\0';
                char *f = s + 1;
                while (*f == ' ')
                    f++;
                outfile = f;
                /* utnij spacje na koncu nazwy pliku */
                for (char *e = f; *e; e++)
                    if (*e == ' ') {
                        *e = '\0';
                        break;
                    }
                break;
            }
        }

        int32_t pid = sys_spawn(line, outfile);
        if (pid < 0) {
            print("ush: nie znam '");
            print(line);
            print("'\n");
            continue;
        }
        sys_wait(pid);
        if (outfile) {
            print("ush: wyjscie zapisane do '");
            print(outfile);
            print("'\n");
        }
    next:;
    }
}
