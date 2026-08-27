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

/* Pokazuje niezerowy kod wyjscia ostatniego polecenia (jak $? w bash) */
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
        /* cd [katalog] - wbudowane, bo zmienia cwd samego shella */
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
        /* kill <pid> - zaznacz proces do zabicia */
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

        /* Uruchomienie w tle: koncowe '&' (shell nie czeka na proces) */
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

        /* Potok wielostopniowy: 'a | b | c' (opcjonalnie '... > plik').
           Jadro rozbija linie po '|', wiec tu tylko wykrywamy '|',
           odcinamy koncowe '> plik' i oddajemy reszte. */
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
                    if (*f == '>')      /* '>>' w potoku: obcina jak '>' */
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

        /* Przekierowanie: '> plik' obcina, '>> plik' dopisuje */
        char *outfile = (void *)0;
        int append = 0;
        for (char *s = line; *s; s++) {
            if (*s == '>') {
                *s = '\0';
                char *f = s + 1;
                if (*f == '>') {        /* '>>' = dopisywanie */
                    append = 1;
                    f++;
                }
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

        int32_t pid = sys_spawn(line, outfile, append);
        if (pid < 0) {
            print("ush: nie znam '");
            print(line);
            print("'\n");
            continue;
        }
        if (bg) {                  /* w tle: nie czekamy, pokazujemy pid */
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
