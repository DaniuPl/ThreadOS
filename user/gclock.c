/* gclock - a clock in a window of its own (ring 3)
 *
 * The time in large figures and the date under it, drawn the way the
 * desktop draws - TrueType, in the theme's colours - so that it sits
 * next to the bar's own clock as one family rather than as a visitor
 * from a machine of eight-by-eight letters.
 *
 * It reads the clock twice a second: often enough that no second is
 * skipped, and it asks the desktop for the theme's colours every time,
 * so it follows a switch between dark and light as soon as the desktop
 * does. One program for both kernels: the system calls it makes have
 * the same names on either side (uapi.h).
 */

#include "udraw.h"
#include "ulocale.h"

#define W 132                 /* the content */
#define H 52
#define TIME_PX 20
#define DATE_PX 11
#define TIME_Y 6              /* the band the time is centred in */
#define TIME_H 24

static void pad2(char *d, int v)
{
    d[0] = (char)('0' + (v / 10) % 10);
    d[1] = (char)('0' + v % 10);
}

/* The day of the week, 0 = Sunday (Sakamoto's method) */
static int weekday(int y, int m, int d)
{
    static const int t[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 3)
        y -= 1;
    return (y + y / 4 - y / 100 + y / 400 + t[(m - 1) % 12] + d) % 7;
}

static int put(char *d, const char *s)
{
    int n = 0;
    while (s[n]) {
        d[n] = s[n];
        n++;
    }
    return n;
}

void _start(void)
{
    static const char *days_en[7] = { "Sunday", "Monday", "Tuesday",
                                      "Wednesday", "Thursday", "Friday",
                                      "Saturday" };
    static const char *days_pl[7] = { "Ndz", "Pon", "Wt", "Śr", "Czw",
                                      "Pt", "Sob" };
    static const char *months_en[12] = { "January", "February", "March",
                                         "April", "May", "June", "July",
                                         "August", "September", "October",
                                         "November", "December" };
    static const char *months_pl[12] = { "sty", "lut", "mar", "kwi", "maj",
                                         "cze", "lip", "sie", "wrz", "paź",
                                         "lis", "gru" };
    int w = (int)win_create(W, H,
                            ulocale_language_polish() ? "Zegar" : "Clock");
    if (w < 0)
        sys_exit(1);

    for (;;) {
        /* The desktop may ask us to end - a window with a close button
           that does nothing is a window that lies. */
        ulocale_window_title(w, "Clock", "Zegar");
        struct win_event ev;
        while (win_poll(w, &ev) == 1)
            if (ev.type == WEV_CLOSE)
                sys_exit(0);

        struct utime t;
        sys_time(&t);
        int polish = ulocale_language_polish();
        const char *const *days = polish ? days_pl : days_en;
        const char *const *months = polish ? months_pl : months_en;
        struct ud_look look;
        ud_look(&look);

        char s[9];
        pad2(s, t.hour);
        s[2] = ':';
        pad2(s + 3, t.min);
        s[5] = ':';
        pad2(s + 6, t.sec);
        s[8] = '\0';

        char date[40];
        int n = 0;
        if (t.month >= 1 && t.month <= 12 && t.day >= 1 && t.day <= 31) {
            n += put(date + n, days[weekday(t.year, t.month, t.day)]);
            date[n++] = ',';
            date[n++] = ' ';
            if (t.day >= 10)
                date[n++] = (char)('0' + t.day / 10);
            date[n++] = (char)('0' + t.day % 10);
            date[n++] = ' ';
            n += put(date + n, months[t.month - 1]);
        }
        date[n] = '\0';

        ud_fill(w, 0, 0, W, H, look.bg);
        ud_text_mid(w, 0, TIME_Y, W, TIME_H, s, look.text, UD_BOLD, TIME_PX);
        ud_text_mid(w, 0, 32, W, 14, date, look.dim, UD_SANS, DATE_PX);

        sys_sleep(500);
    }
}
