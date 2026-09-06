/* gclock - zegar w oknie graficznym (ring 3)
 *
 * Co sekunde czyta RTC (sys_time) i rysuje godzine HH:MM:SS w swoim
 * oknie. Druga aplikacja GUI uruchamiana z menu pulpitu.
 */

#include "usys.h"

#define C_BG    8
#define C_WHITE 4
#define C_GREEN 2
/* Dark text on a light surface, light text on a dark one: the window
   is the system's light grey, so its text is black. */
#define C_TEXT  0
#define C_DIM  15
#define C_HEAD  3

static void pad2(char *d, int v)
{
    d[0] = (char)('0' + (v / 10) % 10);
    d[1] = (char)('0' + v % 10);
}

void _start(void)
{
    int w = win_create(80, 20, "Clock");
    if (w < 0)
        sys_exit(1);

    for (;;) {
        /* The desktop may ask us to end - a window with a close button
           that does nothing is a window that lies. Nothing else comes
           this way, so one look per frame is enough. */
        struct win_event ev;
        while (win_poll(w, &ev) == 1)
            if (ev.type == WEV_CLOSE)
                sys_exit(0);

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
        win_text(w, 8, 6, s, C_TEXT);

        sys_sleep(500);
    }
}
