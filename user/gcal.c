/* gcal - a calendar (ring 3)
 *
 * A month at a time, the week starting on Monday as it does here, today
 * on the accent - from the machine's own clock (the RTC), so it is the
 * day the bar says it is. The arrows, or Left and Right, go a month
 * back or forward; a click on the month's name, or Home, comes back to
 * today. A click on a day picks it and the line at the bottom says
 * which day of the week it falls on - which is, most of the time, what
 * a calendar is opened to find out.
 *
 * One program for both kernels (uapi.h).
 */

#include "udraw.h"
#include "ulocale.h"

#define W0 300
#define H0 336
#define PAD 14

#define HEAD_BASE 34          /* the month's name */
#define ARROW     28          /* the two arrows, round, at the right */
#define ARROW_Y   14
#define DAYS_Y    52          /* the names of the days */
#define GRID_Y    72          /* the first week */
#define CELL_W    38
#define CELL_H    36
#define FOOT_BASE 322         /* the line at the bottom */

static const char *const months_en[12] = {
    "January", "February", "March", "April", "May", "June", "July",
    "August", "September", "October", "November", "December"
};
static const char *const months_pl[12] = {
    "styczeń", "luty", "marzec", "kwiecień", "maj", "czerwiec", "lipiec",
    "sierpień", "wrzesień", "październik", "listopad", "grudzień"
};
static const char *const months_pl_genitive[12] = {
    "stycznia", "lutego", "marca", "kwietnia", "maja", "czerwca", "lipca",
    "sierpnia", "września", "października", "listopada", "grudnia"
};
static const char *const wdays_en[7] = {
    "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday",
    "Sunday"
};
static const char *const wdays_pl[7] = {
    "poniedziałek", "wtorek", "środa", "czwartek", "piątek", "sobota",
    "niedziela"
};
static const char *const wshort_en[7] = { "Mo", "Tu", "We", "Th", "Fr", "Sa",
                                          "Su" };
static const char *const wshort_pl[7] = { "Pn", "Wt", "Śr", "Cz", "Pt", "Sb",
                                          "Nd" };

static int year, month;       /* the month shown, month 1..12 */
static int ty, tm, td;        /* today */
static int picked;            /* a day of the month shown, or 0 */

static int leap(int y)
{
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

static int month_days(int y, int m)
{
    static const int d[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30,
                               31 };
    return m == 2 && leap(y) ? 29 : d[m - 1];
}

/* The day of the week, 0 = Monday (Sakamoto's method, moved a day) */
static int weekday(int y, int m, int d)
{
    static const int t[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 3)
        y -= 1;
    int sun0 = (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
    return (sun0 + 6) % 7;
}

static int put(char *d, const char *s)
{
    int n = 0;
    while (s[n]) {
        d[n] = s[n];
        n++;
    }
    d[n] = '\0';
    return n;
}

static int put_num(char *d, int v)
{
    char t[8];
    int n = 0, k = 0;
    do {
        t[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        d[k++] = t[--n];
    d[k] = '\0';
    return k;
}

static int cell_x(int col) { return PAD + col * CELL_W; }
static int cell_y(int row) { return GRID_Y + row * CELL_H; }

static void today(void)
{
    struct utime t;
    sys_time(&t);
    ty = t.year;
    tm = t.month;
    td = t.day;
    year = ty;
    month = tm;
    picked = 0;
}

static void step(int by)
{
    month += by;
    while (month > 12) {
        month -= 12;
        year++;
    }
    while (month < 1) {
        month += 12;
        year--;
    }
    picked = 0;
}

static void draw(int win, int cw, int ch)
{
    int polish = ulocale_language_polish();
    ulocale_window_title(win, "Calendar", "Kalendarz");
    const char *const *months = polish ? months_pl : months_en;
    const char *const *wdays = polish ? wdays_pl : wdays_en;
    const char *const *wshort = polish ? wshort_pl : wshort_en;
    struct ud_look l;
    ud_look(&l);
    ud_fill(win, 0, 0, cw, ch, l.bg);

    char head[32];
    int n = put(head, months[month - 1]);
    head[n++] = ' ';
    put_num(head + n, year);
    ud_text(win, PAD, HEAD_BASE, head, l.text, UD_BOLD, 17);

    /* The two arrows, as chevrons on round buttons */
    for (int k = 0; k < 2; k++) {
        int x = cw - PAD - 2 * ARROW - 6 + k * (ARROW + 6);
        ud_disc(win, x, ARROW_Y, ARROW, l.row);
        int mx = x + ARROW / 2, my = ARROW_Y + ARROW / 2;
        int dx = k == 0 ? 3 : -3;
        ud_line(win, mx + dx, my - 6, mx - dx, my, 28, l.text);
        ud_line(win, mx - dx, my, mx + dx, my + 6, 28, l.text);
    }

    for (int c = 0; c < 7; c++)
        ud_text_mid(win, cell_x(c), DAYS_Y, CELL_W, 16, wshort[c],
                    c == 6 ? l.danger : l.dim, UD_SANS, 11);
    ud_fill(win, PAD, GRID_Y - 3, 7 * CELL_W, 1, l.line);

    int first = weekday(year, month, 1);
    int days = month_days(year, month);
    for (int d = 1; d <= days; d++) {
        int slot = first + d - 1;
        int x = cell_x(slot % 7), y = cell_y(slot / 7);
        int is_today = year == ty && month == tm && d == td;
        int disc = 30;
        int dx = x + (CELL_W - disc) / 2, dy = y + (CELL_H - disc) / 2;
        if (is_today)
            ud_disc(win, dx, dy, disc, l.accent);
        else if (d == picked)
            ud_disc(win, dx, dy, disc, l.row);
        if (d == picked && is_today)
            ud_ring(win, dx - 2, dy - 2, disc + 4, disc + 4, disc / 2 + 2,
                    l.accent);
        char s[4];
        put_num(s, d);
        uint32_t ink = is_today ? l.on_accent
                     : slot % 7 == 6 ? l.danger : l.text;
        ud_text_mid(win, x, y, CELL_W, CELL_H, s, ink,
                    is_today ? UD_BOLD : UD_SANS, 13);
    }

    /* The picked day, said in words */
    if (picked) {
        char line[48];
        int k = put(line, wdays[weekday(year, month, picked)]);
        k += put(line + k, ", ");
        k += put_num(line + k, picked);
        line[k++] = ' ';
        k += put(line + k, polish ? months_pl_genitive[month - 1]
                                   : months[month - 1]);
        line[k++] = ' ';
        put_num(line + k, year);
        ud_text(win, PAD, FOOT_BASE, line, l.text, UD_SANS, UD_PX);
    } else {
        ud_text_l(win, PAD, FOOT_BASE, "A click on a day says what day it is",
                  l.dim, UD_SANS, 11);
    }
}

static void click(int x, int y, int cw)
{
    for (int k = 0; k < 2; k++) {
        int ax = cw - PAD - 2 * ARROW - 6 + k * (ARROW + 6);
        if (x >= ax && x < ax + ARROW && y >= ARROW_Y && y < ARROW_Y + ARROW) {
            step(k == 0 ? -1 : 1);
            return;
        }
    }
    if (y < GRID_Y - 8 && x < cw / 2) {      /* the month's name: today */
        today();
        return;
    }
    if (x < PAD || x >= PAD + 7 * CELL_W || y < GRID_Y)
        return;
    int slot = (y - GRID_Y) / CELL_H * 7 + (x - PAD) / CELL_W;
    int d = slot - weekday(year, month, 1) + 1;
    if (d >= 1 && d <= month_days(year, month))
        picked = d;
}

void _start(void)
{
    int win = (int)win_create(W0, H0, ulocale_language_polish()
                                        ? "Kalendarz" : "Calendar");
    if (win < 0)
        sys_exit(1);
    int cw = W0, ch = H0;
    today();
    draw(win, cw, ch);

    struct ud_look was;
    ud_look(&was);
    int language = ulocale_language_polish();
    int ticks = 0;
    for (;;) {
        int again = 0;
        int current_language = ulocale_language_polish();
        if (current_language != language) {
            language = current_language;
            again = 1;
        }
        struct win_event ev;
        while (win_poll(win, &ev) == 1) {
            if (ev.type == WEV_CLOSE)
                sys_exit(0);
            if (ev.type == WEV_SIZE) {
                cw = ev.x;
                ch = ev.y;
            } else if (ev.type == WEV_CLICK) {
                click(ev.x, ev.y, cw);
            } else if (ev.type == WEV_KEY) {
                char c = (char)ev.val;
                if (c == KEY_LEFT || c == KEY_PGUP)
                    step(-1);
                else if (c == KEY_RIGHT || c == KEY_PGDN)
                    step(1);
                else if (c == KEY_HOME)
                    today();
            }
            again = 1;
        }
        /* Past midnight today is another day */
        if (++ticks >= 50) {
            ticks = 0;
            struct utime t;
            sys_time(&t);
            if (t.day != td || t.month != tm || t.year != ty) {
                ty = t.year;
                tm = t.month;
                td = t.day;
                again = 1;
            }
        }
        struct ud_look now;
        ud_look(&now);
        if (now.dark != was.dark) {
            was = now;
            again = 1;
        }
        if (again)
            draw(win, cw, ch);
        sys_sleep(100);
    }
}
