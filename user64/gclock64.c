/* gclock64 - zegar w oknie graficznym (ring 3, jadro 64-bitowe)
 *
 * Co pol sekundy czyta zegar plyty glownej i rysuje godzine w swoim
 * oknie. Pod spodem uptime - liczony z tyknac timera, wiec widac
 * naraz dwa niezalezne zrodla czasu: sprzet i jadro.
 */

#include "usys64.h"

#define C_BG    8
#define C_GREEN 2
#define C_WHITE 4

#define WINDOW_W 100
#define WINDOW_H 34

static void two_digits(char *d, int v)
{
    d[0] = (char)('0' + (v / 10) % 10);
    d[1] = (char)('0' + v % 10);
}

void _start(void)
{
    int w = (int)win_create(WINDOW_W, WINDOW_H, "Zegar");
    if (w < 0) {
        print("gclock64: pulpit nie dal okna\n");
        sys_exit(1);
    }

    for (;;) {
        struct time64 t;
        sys_time(&t);

        char s[9];
        two_digits(s, t.hour);
        s[2] = ':';
        two_digits(s + 3, t.minute);
        s[5] = ':';
        two_digits(s + 6, t.second);
        s[8] = '\0';

        char up[24];
        int n = 0;
        up[n++] = 'u';
        up[n++] = 'p';
        up[n++] = ' ';
        char tmp[24];
        utoa(sys_ticks() / 100, tmp);
        for (int i = 0; tmp[i]; i++)
            up[n++] = tmp[i];
        up[n++] = ' ';
        up[n++] = 's';
        up[n] = '\0';

        win_rect(w, 0, 0, WINDOW_W, WINDOW_H, C_BG);
        win_text(w, 8, 4, s, C_GREEN);
        win_text(w, 8, 20, up, C_WHITE);

        sys_sleep(500);
    }
}
