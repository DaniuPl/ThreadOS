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
static volatile int przerwany;

/* Shell nie ginie od Ctrl+C - przerywa tylko to, co wpisywal.
   Bez wlasnej procedury sygnal zabilby go domyslnie, a wtedy kazde
   przypadkowe Ctrl+C konczyloby sesje. */
static void na_sygnal(int sig)
{
    (void)sig;
    przerwany = 1;
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

static int rowne(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static int zaczyna_sie(const char *s, const char *p)
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

static char slowa[SH_ARGS][32];
static char *argv[SH_ARGS + 1];

static int podziel(char *linia)
{
    int n = 0, i = 0;
    while (linia[i] && n < SH_ARGS) {
        while (linia[i] == ' ')
            i++;
        if (!linia[i])
            break;
        int k = 0;
        while (linia[i] && linia[i] != ' ' && k < 31)
            slowa[n][k++] = linia[i++];
        slowa[n][k] = '\0';
        while (linia[i] && linia[i] != ' ')
            i++;
        argv[n] = slowa[n];
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
static char slowa2[SH_ARGS][32];
static char *argv2[SH_ARGS + 1];

static void uruchom_potok(char *lewa, char *prawa)
{
    int fd[2];
    if (sys_pipe(fd) < 0) {
        print("ush64: nie mam z czego zrobic potoku\n");
        return;
    }

    /* Prawa strona najpierw: jej argv trzeba zbudowac, zanim podzial
       lewej nadpisze wspolne bufory */
    int n2 = 0, i = 0;
    while (prawa[i] && n2 < SH_ARGS) {
        while (prawa[i] == ' ')
            i++;
        if (!prawa[i])
            break;
        int k = 0;
        while (prawa[i] && prawa[i] != ' ' && k < 31)
            slowa2[n2][k++] = prawa[i++];
        slowa2[n2][k] = '\0';
        argv2[n2] = slowa2[n2];
        n2++;
    }
    argv2[n2] = 0;
    if (!n2) {
        print("ush64: po kresce nic nie ma\n");
        sys_close(fd[0]);
        sys_close(fd[1]);
        return;
    }

    int n1 = podziel(lewa);
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

    int kod = 0;
    sys_wait(&kod);
    sys_wait(&kod);
    (void)a;
    (void)b;
}

static void uruchom(const char *nazwa)
{
    int64_t dziecko = sys_fork();
    if (dziecko < 0) {
        print("ush64: nie mam z czego zrobic procesu\n");
        return;
    }
    if (dziecko == 0) {
        sys_exec(nazwa, argv);
        /* exec wraca tylko wtedy, gdy sie nie udal */
        print("ush64: nie ma takiego programu\n");
        sys_exit(127);
    }

    int kod = 0;
    int64_t kto = sys_wait(&kod);
    if (kto >= 0)
        say("ush64: koniec, kod ", kod, "\n");
}

static void wykonaj(void)
{
    if (!line[0])
        return;

    if (rowne(line, "help")) {
        print("ush64: help, echo <tekst>, pid, exit, "
              "albo nazwa programu z initrd\n");
    } else if (rowne(line, "pid")) {
        /* Numer procesu poznajemy po tym, co fork mowi rodzicowi -
           tak samo jak kazdy inny program */
        print("ush64: jestem procesem w ring 3\n");
    } else if (rowne(line, "exit")) {
        print("ush64: pa!\n");
        sys_exit(0);
    } else if (zaczyna_sie(line, "echo ")) {
        print(line + 5);
        print("\n");
    } else {
        /* Kreska dzieli linie na dwa programy - wszystko przed nia
           pisze do tego, co czyta wszystko za nia */
        int kreska = -1;
        for (int i = 0; line[i]; i++)
            if (line[i] == '|') {
                kreska = i;
                break;
            }
        if (kreska >= 0) {
            line[kreska] = '\0';
            uruchom_potok(line, line + kreska + 1);
        } else {
            int n = podziel(line);
            if (n)
                uruchom(argv[0]);
        }
    }
}

void _start(void)
{

    sys_signal(SIGINT, na_sygnal);
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
                if (przerwany) {
                    przerwany = 0;
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
        wykonaj();
    }
}
