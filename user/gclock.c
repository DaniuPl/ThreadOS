/* gclock - zegar w oknie graficznym (ring 3)
 *
 * Co sekunde czyta RTC (sys_time) i rysuje godzine HH:MM:SS w swoim
 * oknie. Druga aplikacja GUI uruchamiana z menu pulpitu.
 */

#include "usys.h"

#define C_BG    8
#define C_WHITE 4
#define C_GREEN 2

static void pad2(char *d, int v)
{
    d[0] = (char)('0' + (v / 10) % 10);
    d[1] = (char)('0' + v % 10);
}

void _start(void)
{
    int w = win_create(80, 20, "Zegar");
    if (w < 0)
        sys_exit(1);

    for (;;) {
        struct utime t;
        sys_time(&t);

        char s[9];
        pad2(s, t.hour);
        s[2] = ':';
        pad2(s + 3, t.min);
        s[5] = ':';
        pad2(s + 6, t.sec);
        s[8] = '\0';

        win_rect(w, 0, 0, 80, 20, C_BG);
        win_text(w, 8, 6, s, C_GREEN);

        sys_sleep(500);
    }
}
