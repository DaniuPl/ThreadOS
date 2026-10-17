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

#include "udraw.h"

#define W0 340
#define H0 298
#define PAD 12
#define HEAD_H 34             /* the heading and the button */
#define COLS_Y 36             /* the names of the columns */
#define COLS_H 16
#define ROW_Y0 54             /* the first task */
#define ROW_H 16
#define FOOT_H 20             /* a line for what was said */
#define MAXT 14
#define KILL_W 84             /* "End task", kept at the right */
#define KILL_Y 7
#define KILL_H 22

/* Where each column starts */
#define COL_PID   PAD
#define COL_NAME  52
#define COL_STATE 176
#define COL_KIND  256

static struct psinfo list[MAXT];
static int count;
static int chosen = -1;       /* the pid picked, not the row: rows move */
static char note[40];
static int note_ok;           /* what was said is good news */

static int cw = W0, ch = H0;

/* The button holds the right-hand edge, whatever width the window has
   been pulled to. Nailed to a number, it sat in the middle of a
   maximised window looking like a mistake. */
static int kill_x(void)
{
    return cw - PAD - KILL_W;
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
    case 0:  return "ready";
    case 1:  return "running";
    case 2:  return "spi";
    case 3:  return "waiting";
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
    note_ok = 0;
    if (chosen < 0) {
        ecopy(note, "no task picked", sizeof(note));
        return;
    }
    if (chosen == (int)sys_getpid()) {
        ecopy(note, "I do not end myself", sizeof(note));
        return;
    }
    for (int i = 0; i < count; i++) {
        if (list[i].pid != chosen)
            continue;
        if (!list[i].ring3) {
            ecopy(note, "a kernel thread, not a program", sizeof(note));
            return;
        }
        sys_kill(chosen, SIGKILL);
        ecopy(note, "ended", sizeof(note));
        note_ok = 1;
        chosen = -1;
        return;
    }
    ecopy(note, "that task is gone already", sizeof(note));
    chosen = -1;
}

static void draw(int win)
{
    struct ud_look l;
    ud_look(&l);
    ud_fill(win, 0, 0, cw, ch, l.bg);

    char line[32];
    int p = num(line, count);
    ecopy(line + p, " tasks", (int)sizeof(line) - p);
    int end = ud_text(win, PAD, ud_baseline(0, HEAD_H, 14), "Tasks", l.text,
                      UD_BOLD, 14);
    ud_text(win, end + 8, ud_baseline(0, HEAD_H, 14), line, l.dim, UD_SANS,
            11);

    /* The button, in the top right corner where there is room next to
       the heading - quiet until there is something to end */
    int ready = chosen >= 0;
    ud_round(win, kill_x(), KILL_Y, KILL_W, KILL_H, 6,
             ready ? l.danger : l.row);
    ud_text_mid(win, kill_x(), KILL_Y, KILL_W, KILL_H, "End task",
                ready ? l.on_accent : l.dim, UD_BOLD, UD_PX);

    static const char *const head[4] = { "PID", "Name", "State", "Kind" };
    static const int col[4] = { COL_PID, COL_NAME, COL_STATE, COL_KIND };
    for (int k = 0; k < 4; k++)
        ud_text(win, col[k], ud_baseline(COLS_Y, COLS_H, 11), head[k], l.dim,
                UD_SANS, 11);
    ud_fill(win, PAD, ROW_Y0 - 2, cw - 2 * PAD, 1, l.line);

    for (int i = 0; i < count; i++) {
        int y = ROW_Y0 + i * ROW_H;
        if (y + ROW_H > ch - FOOT_H)
            break;
        int picked = list[i].pid == chosen;
        if (picked)
            ud_round(win, PAD / 2, y, cw - PAD, ROW_H, 6, l.accent);
        else if (i & 1)
            ud_round(win, PAD / 2, y, cw - PAD, ROW_H, 6, l.row);

        uint32_t ink = picked ? l.on_accent : l.text;
        uint32_t quiet = picked ? l.on_accent : l.dim;
        int base = ud_baseline(y, ROW_H, UD_PX);
        char pid[8];
        num(pid, list[i].pid);
        ud_text(win, COL_PID, base, pid, quiet, UD_SANS, UD_PX);
        ud_text(win, COL_NAME, base, list[i].name, ink,
                UD_SANS, UD_PX);
        ud_text(win, COL_STATE, base, state_name(list[i].state), ink, UD_SANS,
                UD_PX);
        /* A process has memory of its own; a kernel thread does not -
           and only the first of the two is something to end */
        ud_text(win, COL_KIND, base, list[i].ring3 ? "program" : "kernel",
                list[i].ring3 ? ink : quiet, UD_SANS, UD_PX);
    }

    if (note[0])
        ud_text(win, PAD, ud_baseline(ch - FOOT_H, FOOT_H, 11), note,
                note_ok ? l.good : l.danger, UD_SANS, 11);
}

void _start(void)
{
    int win = (int)win_create(W0, H0, "Tasks");
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
                if (ev.y >= KILL_Y && ev.y < KILL_Y + KILL_H &&
                    ev.x >= kill_x() && ev.x < kill_x() + KILL_W) {
                    end_chosen();
                    refresh();
                } else if (ev.y >= ROW_Y0) {
                    int row = (ev.y - ROW_Y0) / ROW_H;
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
