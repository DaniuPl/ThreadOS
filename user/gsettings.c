/* gsettings - system settings in a desktop window (ring 3)
 *
 * What the machine remembers about itself, shown the way a person
 * would want to see it: a row per setting, with its name, its value
 * and a line saying what it is for.
 *
 * The program holds none of it. It asks the kernel for the i-th
 * setting and asks the kernel to change one - because there is one
 * place that decides what a setting is, and it is not here. What the
 * program does have is the part a window is for: which row is
 * selected, what has been typed, and whether it has been written down
 * yet.
 *
 * Two ways of changing a value, because the settings are of two kinds.
 * A number out of a small set - the desktop background, the colour of
 * the prompt - is cycled by clicking its row: there is nothing to type
 * when there are six possibilities. Text - the machine's name, the
 * address of a name server - is typed, because it cannot be guessed.
 *
 * Enter writes everything to disk. And the proof that it worked is not
 * the line that appears here but the next boot.
 */

#include "usys.h"

#define C_BG     8    /* the window's grey background */
#define C_RED    1
#define C_GREEN  2
#define C_BLUE   3
#define C_WHITE  4
#define C_BLACK  0

/* The size the window is created with. From then on it is whatever the
   person pulled it to, and the desktop says so with WEV_SIZE. */
#define W0  330
#define H0  160

#define ROW_Y   22            /* the first row */
#define ROW_H   14
#define MAXCFG   8

static struct cfg_entry cfg[MAXCFG];
static int ncfg;
static int chosen;            /* the selected row */
static char edit[CFG_VAL_MAX];
static int elen;
static int saved;             /* 1 = written down since the last change */
static int cw = W0, ch = H0;  /* the content area, as it is now */

/* Which settings are cycled rather than typed, and how far they go.
   The kernel knows the names; here we only know which of them a person
   picks from a list instead of writing out. */
static int cycle_limit(const char *name)
{
    if (name[0] == 't' && name[1] == 'l' && name[2] == 'o' && !name[3])
        return 6;                     /* tlo: six backgrounds */
    if (name[0] == 'k' && name[1] == 'o' && name[2] == 'l')
        return 16;                    /* kolor: sixteen text colours */
    return 0;                         /* everything else is typed */
}

static void num_to_text(uint32_t v, char *out)
{
    char tmp[12];
    int n = 0, j = 0;
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        out[j++] = tmp[--n];
    out[j] = 0;
}

static uint32_t text_to_num(const char *s)
{
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (uint32_t)(*s++ - '0');
    return v;
}

static void load(void)
{
    ncfg = 0;
    while (ncfg < MAXCFG && sys_cfg_get((uint32_t)ncfg, &cfg[ncfg]) == 0)
        ncfg++;
}

/* Takes the selected row's value into the edit buffer - so that typing
   starts from what is there rather than from nothing */
static void take_value(void)
{
    elen = 0;
    if (chosen < 0 || chosen >= ncfg)
        return;
    while (cfg[chosen].value[elen] && elen < (int)sizeof(edit) - 1) {
        edit[elen] = cfg[chosen].value[elen];
        elen++;
    }
    edit[elen] = 0;
}

static void save_all(void)
{
    /* Only the selected one has been touched, but writing them all
       costs nothing and there is then no question of which of them the
       file holds */
    if (chosen >= 0 && chosen < ncfg) {
        edit[elen] = 0;
        if (sys_cfg_set(cfg[chosen].name, edit) == 0) {
            saved = 1;
            load();
            take_value();
            return;
        }
    }
    saved = -1;                        /* refused - the name or the value */
}

void _start(void)
{
    int w = win_create(W0, H0, "Ustawienia");
    if (w < 0)
        sys_exit(1);

    load();
    take_value();

    for (;;) {
        struct win_event ev;
        while (win_poll(w, &ev) == 1) {
            if (ev.type == WEV_CLICK) {
                int row = (ev.y - ROW_Y) / ROW_H;
                if (row >= 0 && row < ncfg) {
                    if (row == chosen) {
                        /* A second click on the same row: for a setting
                           picked from a list, that is the way to pick */
                        int limit = cycle_limit(cfg[row].name);
                        if (limit) {
                            uint32_t v = (text_to_num(edit) + 1) %
                                         (uint32_t)limit;
                            num_to_text(v, edit);
                            elen = 0;
                            while (edit[elen])
                                elen++;
                            saved = 0;
                        }
                    } else {
                        chosen = row;
                        take_value();
                        saved = 0;
                    }
                }
            } else if (ev.type == WEV_CLOSE) {
                /* Asked to end - so we end. There is nothing to save:
                   Enter writes to disk, and what was not written was
                   not meant to be. */
                sys_exit(0);
            } else if (ev.type == WEV_SIZE) {
                cw = ev.x;
                ch = ev.y;
            } else if (ev.type == WEV_KEY) {
                char c = (char)ev.val;
                if (c == '\n' || c == '\r') {
                    save_all();
                } else if (c == '\b') {
                    if (elen > 0) {
                        elen--;
                        saved = 0;
                    }
                } else if (c >= 0x20 && c < 0x7F &&
                           elen < (int)sizeof(edit) - 1) {
                    edit[elen++] = c;
                    saved = 0;
                }
            }
        }
        edit[elen] = 0;

        /* --- drawing --- */
        win_rect(w, 0, 0, cw, ch, C_BG);
        win_text(w, 6, 6, "Ustawienia systemu:", C_BLUE);

        for (int i = 0; i < ncfg; i++) {
            int y = ROW_Y + i * ROW_H;
            /* The selected row is marked, so that clicking and typing
               have a visible target */
            if (i == chosen)
                win_rect(w, 2, y - 2, cw - 4, ROW_H - 1, C_BLUE);
            /* Black on grey reads; white on grey does not. The
               selected row has a blue field under it, so there white
               is the one that reads. */
            win_text(w, 6, y, cfg[i].name, i == chosen ? C_WHITE : C_BLACK);
            win_text(w, 100, y, i == chosen ? edit : cfg[i].value,
                     i == chosen ? C_GREEN : C_BLACK);
        }

        /* What the selected setting is for, and how to change it */
        int y = ROW_Y + ncfg * ROW_H + 6;
        if (chosen >= 0 && chosen < ncfg) {
            /* The description may be two lines - win_text draws one, so
               we cut it at the newline ourselves */
            char line[CFG_HELP_MAX];
            const char *h = cfg[chosen].help;
            while (*h) {
                int n = 0;
                while (h[n] && h[n] != '\n' && n < (int)sizeof(line) - 1)
                    n++;
                for (int k = 0; k < n; k++)
                    line[k] = h[k];
                line[n] = 0;
                win_text(w, 6, y, line, C_BLACK);
                y += 11;
                h += n;
                if (*h == '\n')
                    h++;
            }
            win_text(w, 6, y + 3,
                     cycle_limit(cfg[chosen].name)
                         ? "klik w wiersz zmienia, Enter zapisuje"
                         : "pisz na klawiaturze, Enter zapisuje",
                     C_BLUE);
            y += 3;
        }

        if (saved > 0)
            win_text(w, 6, y + 15, "zapisane na dysku", C_GREEN);
        else if (saved < 0)
            win_text(w, 6, y + 15, "NIE zapisane", C_RED);

        sys_sleep(80);
    }
}
