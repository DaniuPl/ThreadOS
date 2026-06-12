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
          "  help - ta pomoc\n"
          "  pid  - pokaz pid shella\n"
          "  exit - wyjdz do shella jadra\n");
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
        sys_write(&buf[--n], 1);
    print("\n");
}

static uint32_t read_line(void)
{
    uint32_t n = 0;
    for (;;) {
        char c;
        if (sys_read(&c, 1) != 1)
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
            sys_write(&c, 1);   /* echo robi shell, nie jadro */
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

        int32_t pid = sys_spawn(line);
        if (pid < 0) {
            print("ush: nie znam '");
            print(line);
            print("'\n");
            continue;
        }
        sys_wait(pid);
    }
}
