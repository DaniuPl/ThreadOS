/* gtasks - what is running, and a way to end it (ring 3)
 *
 * The shell has printed 'ps' since the beginning, but printing is not
 * something a program can read, and a list one has to type a command
 * for is a list nobody looks at. A machine with a dozen windows on it
 * needs one place that says what is running - and, when a program has
 * stopped answering, a way to end it that is not "restart the
 * machine".
 *
 * So: a row per task, refreshed twice a second, a click to choose one
 * and a button to end it. What may be ended are PROCESSES - things
 * with memory of their own. The kernel's own threads are on the list
 * because seeing them is the whole point of a list, but this program
 * will not offer to kill the scheduler's idle task; and it will not
 * end itself, because a task manager that vanishes when clicked is a
 * joke rather than a tool.
 *
 * Nothing here depends on the processor mode: one source, both
 * kernels. Only the kernel underneath differs, and it answers the same
 * two calls - one row of the list, and a signal to a pid.
 */

#include "uapi.h"

/* Dark text on a light surface, light text on a dark one. The window
   is the system's light grey, so its text is BLACK; white belongs on
   the accent blue and nowhere else. */
#define C_BG    8             /* the window's light grey */
#define C_TEXT  0             /* black - text on the window */
#define C_DIM  15             /* dark grey - the same, said quietly */
#define C_HEAD  3             /* the accent: headings, buttons, the picked row */
#define C_WHITE 4             /* only ever on something dark */
#define C_LINE 11             /* a line between one thing and the next */
#define C_BLUE  3
#define C_RED   1

#define W0 300
#define H0 190
#define ROW_H 10
#define ROW_Y0 26
#define HEAD_H 12
#define MAXT 14
#define KILL_W 64             /* the "Zakoncz" button, kept at the right */

static struct psinfo list[MAXT];
static int count;
static int chosen = -1;       /* the pid picked, not the row: rows move */
static char note[40];

static int cw = W0, ch = H0;

/* The button holds the right-hand edge, whatever width the window has
   been pulled to. Nailed to a number, it sat in the middle of a
   maximised window looking like a mistake. */
static int kill_x(void)
{
    return cw - KILL_W;
}

static void ecopy(char *d, const char *s, int max)
{
    int i = 0;
    while (s[i] && i < max - 1) {
        d[i] = s[i];
        i++;
    }
    d[i] = '\0';
}

static int num(char *d, int v)
{
    char tmp[8];
    int n = 0;
    if (v < 0) {
        d[0] = '-';
        d[1] = '\0';
        return 1;
    }
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v && n < 7);
    for (int i = 0; i < n; i++)
        d[i] = tmp[n - 1 - i];
    d[n] = '\0';
    return n;
}

static const char *state_name(int st)
{
    switch (st) {
    case 0:  return "gotowy";
    case 1:  return "dziala";
    case 2:  return "spi";
    case 3:  return "czeka";
    default: return "zombie";
    }
}

static void refresh(void)
{
    count = 0;
    for (int i = 0; i < MAXT; i++) {
        if (sys_ps(i, &list[count]) != 0)
            break;
        count++;
    }
}

/* Ends the chosen one. What may be ended is a process - a thing with
   memory of its own; a kernel thread shares the kernel's, and ending
   it would take a piece of the machine with it. */
static void end_chosen(void)
{
    if (chosen < 0) {
        ecopy(note, "nie wybrano zadania", sizeof(note));
        return;
    }
    if (chosen == (int)sys_getpid()) {
        ecopy(note, "siebie nie koncze", sizeof(note));
        return;
    }
    for (int i = 0; i < count; i++) {
        if (list[i].pid != chosen)
            continue;
        if (!list[i].ring3) {
            ecopy(note, "to watek jadra, nie program", sizeof(note));
            return;
        }
        sys_kill(chosen, SIGKILL);
        ecopy(note, "zakonczone", sizeof(note));
        chosen = -1;
        return;
    }
    ecopy(note, "tego zadania juz nie ma", sizeof(note));
    chosen = -1;
}

static void draw(int win)
{
    win_rect(win, 0, 0, cw, ch, C_BG);

    char line[64];
    int p = num(line, count);
    ecopy(line + p, " zadan", (int)sizeof(line) - p);
    win_text(win, 4, 2, "Zadania:", C_HEAD);
    win_text(win, 80, 2, line, C_TEXT);

    /* The button, in the top right corner where there is room next to
       the heading */
    win_rect(win, kill_x(), 0, KILL_W, HEAD_H, C_BLUE);
    win_text(win, kill_x() + 4, 2, "Zakoncz", C_WHITE);

    win_rect(win, 0, ROW_Y0 - 4, cw, 1, C_LINE);

    for (int i = 0; i < count; i++) {
        int y = ROW_Y0 + i * ROW_H;
        if (y + ROW_H > ch - 12)
            break;
        int picked = list[i].pid == chosen;
        if (picked)
            win_rect(win, 0, y - 1, cw, ROW_H, C_BLUE);

        char row[64];
        int n = num(row, list[i].pid);
        while (n < 4)
            row[n++] = ' ';
        row[n] = '\0';
        win_text(win, 4, y, row, picked ? C_WHITE : C_HEAD);
        win_text(win, 44, y, list[i].name, picked ? C_WHITE : C_TEXT);
        win_text(win, 160, y, state_name(list[i].state),
                 picked ? C_WHITE : C_TEXT);
        /* A process has memory of its own; a kernel thread does not -
           and only the first of the two is something to end */
        win_text(win, 232, y, list[i].ring3 ? "program" : "jadro",
                 picked ? C_WHITE : (list[i].ring3 ? C_TEXT : C_DIM));
    }

    if (note[0])
        win_text(win, 4, ch - 10, note, C_RED);
}

void _start(void)
{
    int win = (int)win_create(W0, H0, "Zadania");
    if (win < 0)
        sys_exit(1);

    refresh();
    draw(win);

    int ticks = 0;
    for (;;) {
        struct win_event ev;
        int again = 0;
        while (win_poll(win, &ev) == 1) {
            if (ev.type == WEV_CLOSE)
                sys_exit(0);
            if (ev.type == WEV_SIZE) {
                cw = ev.x;
                ch = ev.y;
            } else if (ev.type == WEV_CLICK) {
                /* The press and the release of the same click arrive
                   too; acting on all three would end the chosen task
                   three times over. */
                if (ev.y < HEAD_H && ev.x >= kill_x()) {
                    end_chosen();
                    refresh();
                } else if (ev.y >= ROW_Y0 - 1) {
                    int row = (ev.y - ROW_Y0 + 1) / ROW_H;
                    if (row >= 0 && row < count) {
                        chosen = list[row].pid;
                        note[0] = '\0';
                    }
                }
            }
            again = 1;
        }

        /* Twice a second the list is read again: a task manager that
           shows what was true a minute ago is a picture, not a list. */
        if (++ticks >= 5) {
            ticks = 0;
            refresh();
            again = 1;
        }
        if (again)
            draw(win);
        sys_sleep(100);
    }
}
