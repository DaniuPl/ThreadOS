/* ush64 - shell w przestrzeni uzytkownika
 *
 * Zwykly program w ring 3. Nie ma zadnych uprawnien ponad te, ktore ma
 * kazdy inny proces: czyta klawiature wywolaniem systemowym, uruchamia
 * programy przez fork i exec, czeka na nie przez wait. Wszystko, co
 * robi, moglby zrobic dowolny inny program - i to jest sedno.
 *
 * Shell w jadrze byl potrzebny, zeby w ogole bylo czym sterowac
 * systemem. Ten jest dowodem, ze juz nie musi tam byc.
 */

#include "usys64.h"

#define LINE_MAX 80

static char line[LINE_MAX];
static int line_len;
static char out[128];
static volatile int interrupted;

/* Shell nie ginie od Ctrl+C - przerywa tylko to, co wpisywal.
   Bez wlasnej procedury sygnal zabilby go domyslnie, a wtedy kazde
   przypadkowe Ctrl+C konczyloby sesje. */
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

    /* Kod wyjscia bywa ujemny - program zabity sygnalem oddaje minus
       jeden. Bez tego wychodzi z tego liczba o dwudziestu cyfrach. */
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

/* Uruchomienie programu: fork, exec w dziecku, wait w rodzicu.
   Dokladnie ten sam schemat, ktorego uzywa kazdy shell.
   Linie dzielimy na slowa - pierwsze to program, reszta to jego
   argumenty, i shell nie interesuje sie tym, co z nich wyniknie. */
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

/* Dwa programy polaczone potokiem.
 *
 * Shell zaklada potok, rozdaje jego konce dwojce dzieci i - co
 * najwazniejsze - zamyka oba u siebie. Gdyby ich nie zamknal, drugi
 * program czekalby na koniec danych, ktory nigdy by nie nadszedl:
 * jeden piszacy (sam shell) zostalby przy zyciu na zawsze. */
static char words2[SH_ARGS][32];
static char *argv2[SH_ARGS + 1];

static void run_pipe(char *left, char *right)
{
    int fd[2];
    if (sys_pipe(fd) < 0) {
        print("ush64: nie mam z czego zrobic potoku\n");
        return;
    }

    /* Prawa strona najpierw: jej argv trzeba zbudowac, zanim podzial
       lewej nadpisze wspolne bufory */
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
        print("ush64: po kresce nic nie ma\n");
        sys_close(fd[0]);
        sys_close(fd[1]);
        return;
    }

    int n1 = split(left);
    if (!n1) {
        print("ush64: przed kreska nic nie ma\n");
        sys_close(fd[0]);
        sys_close(fd[1]);
        return;
    }

    int64_t a = sys_fork();
    if (a == 0) {
        sys_dup2(fd[1], FD_STDOUT);   /* wyjscie idzie do potoku */
        sys_close(fd[0]);
        sys_close(fd[1]);
        sys_exec(argv[0], argv);
        sys_exit(127);
    }

    int64_t b = sys_fork();
    if (b == 0) {
        sys_dup2(fd[0], FD_STDIN);    /* wejscie bierze sie z potoku */
        sys_close(fd[0]);
        sys_close(fd[1]);
        sys_exec(argv2[0], argv2);
        sys_exit(127);
    }

    /* Bez tego drugi program czekalby na koniec danych bez konca */
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
        print("ush64: nie mam z czego zrobic procesu\n");
        return;
    }
    if (child == 0) {
        sys_exec(name, argv);
        /* exec wraca tylko wtedy, gdy sie nie udal */
        print("ush64: nie ma takiego programu\n");
        sys_exit(127);
    }

    int code = 0;
    int64_t who = sys_wait(&code);
    if (who >= 0)
        say("ush64: koniec, kod ", code, "\n");
}

static void execute(void)
{
    if (!line[0])
        return;

    if (equal(line, "help")) {
        print("ush64: help, echo <tekst>, pid, exit, "
              "albo nazwa programu z initrd\n");
    } else if (equal(line, "pid")) {
        /* Numer procesu poznajemy po tym, co fork mowi rodzicowi -
           tak samo jak kazdy inny program */
        print("ush64: jestem procesem w ring 3\n");
    } else if (equal(line, "exit")) {
        print("ush64: pa!\n");
        sys_exit(0);
    } else if (starts_with(line, "echo ")) {
        print(line + 5);
        print("\n");
    } else {
        /* Kreska dzieli linie na dwa programy - wszystko przed nia
           pisze do tego, co czyta wszystko za nia */
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
    print("ush64 - shell uzytkownika. Wpisz 'help'.\n");

    for (;;) {
        print("ush64$ ");
        line_len = 0;

        for (;;) {
            char c;
            int64_t n = sys_read(FD_STDIN, &c, 1);
            if (n <= 0) {
                /* Czekanie przerwal sygnal. Procedura obslugi juz
                   byla - zostaje porzucic zaczeta linie i zaczac od
                   nowa, dokladnie jak kazdy shell. */
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
                out[0] = c;          /* echo robi czytajacy */
                out[1] = 0;
                print(out);
            }
        }

        line[line_len] = '\0';
        execute();
    }
}
