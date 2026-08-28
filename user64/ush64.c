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

static int put(int n, const char *s)
{
    while (*s)
        out[n++] = *s++;
    return n;
}

static int putn(int n, int64_t v)
{
    char tmp[24];
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
   Dokladnie ten sam schemat, ktorego uzywa kazdy shell. */
static void uruchom(const char *nazwa)
{
    int64_t dziecko = sys_fork();
    if (dziecko < 0) {
        print("ush64: nie mam z czego zrobic procesu\n");
        return;
    }
    if (dziecko == 0) {
        sys_exec(nazwa);
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
        uruchom(line);
    }
}

void _start(long id)
{
    (void)id;

    print("ush64 - shell uzytkownika. Wpisz 'help'.\n");

    for (;;) {
        print("ush64$ ");
        line_len = 0;

        for (;;) {
            char c;
            int64_t n = sys_read(&c, 1);
            if (n <= 0)
                continue;            /* sygnal - procedura obslugi juz byla */

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
