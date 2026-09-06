/* gmon - a resource monitor (ring 3)
 *
 * Every half second it asks the kernel for statistics (sys_sysinfo) and
 * draws RAM usage and uptime live in a window, with a memory usage bar.
 */

#include "usys.h"

#define C_BG    8
#define C_WHITE 4
#define C_GREEN 2
#define C_RED   1
#define C_BLUE  3
/* Dark text on a light surface, light text on a dark one: the window
   is the system's light grey, so its text is black. */
#define C_TEXT  0
#define C_DIM  15
#define C_HEAD  3

/* Draws a decimal number in the window */
static void draw_num(int w, int x, int y, uint32_t v, int col)
{
    char tmp[12], s[12];
    int n = 0, j = 0;
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        s[j++] = tmp[--n];
    s[j] = '\0';
    win_text(w, x, y, s, col);
}

void _start(void)
{
    int w = win_create(170, 80, "Monitor");
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

        struct sysinfo si;
        sys_sysinfo(&si);

        win_rect(w, 0, 0, 170, 80, C_BG);
        win_text(w, 4, 4, "RAM used (KiB):", C_HEAD);
        draw_num(w, 136, 4, si.used_kib, C_TEXT);
        win_text(w, 4, 16, "RAM total (KiB):", C_HEAD);
        draw_num(w, 136, 16, si.ram_kib, C_TEXT);
        win_text(w, 4, 28, "uptime (s):", C_HEAD);
        draw_num(w, 96, 28, si.uptime_s, C_TEXT);

        /* The RAM usage bar (the percentage from used/total) */
        uint32_t pct = si.ram_kib ? (si.used_kib * 160 / si.ram_kib) : 0;
        win_rect(w, 4, 48, 160, 12, C_RED);
        win_rect(w, 4, 48, (int)pct, 12, C_BLUE);

        sys_sleep(500);
    }
}
