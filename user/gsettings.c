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

#include "udraw.h"

/* The size the window is created with. From then on it is whatever the
   person pulled it to, and the desktop says so with WEV_SIZE. */
/* Room for the settings there are, the two lines of description under
   them and the line that says what happened - and room for one more
   setting before this has to be thought about again. */
#define W0  360
#define H0  310

#define ROW_Y   30            /* the first row */
#define ROW_H   18
#define VAL_X   124           /* where a setting's value starts */
#define NOTE_DY 12            /* the last line's baseline, from the bottom */
#define PASS_DY 54            /* the password's field, from the bottom */
#define MAXCFG   12    /* room for the settings there are, and a few more */

static struct cfg_entry cfg[MAXCFG];
static int ncfg;
static int chosen;            /* the selected row */
static char edit[CFG_VAL_MAX];
static int elen;
static int saved;             /* 1 = written down since the last change */
/* The kernel refused the write because whoever is at the machine is
   not an admin. So we ask - here, in the window, the same way the
   terminal asks - and try once more. The password is never kept: it
   goes to the kernel and the buffer is cleared behind it. */
static int asking;
static char pass[CFG_VAL_MAX];
static int plen;
static int cw = W0, ch = H0;  /* the content area, as it is now */

/* Which settings are cycled rather than typed, and how far they go.
   The kernel knows the names; here we only know which of them a person
   picks from a list instead of writing out. */
static int same(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static int cycle_limit(const char *name)
{
    if (same(name, "background"))
        return 7;                     /* the aurora and six colours */
    if (same(name, "color"))
        return 16;                    /* sixteen text colours */
    if (same(name, "dock"))
        return 3;                     /* small, medium, large */
    return 0;                         /* everything else is typed */
}

/* The theme is not a number but one of two words, so clicking its row
   steps between those two rather than counting */
static const char *themes[] = { "dark", "light" };
#define THEME_N 2

static int is_theme(const char *name) { return same(name, "theme"); }

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
        int32_t rc = sys_cfg_set(cfg[chosen].name, edit);
        if (rc == 0) {
            saved = 1;
            asking = 0;
            load();
            take_value();
            return;
        }
        if (rc == -2) {
            /* Not allowed - which is not the same as 'it did not
               work', and the answer to it is a password rather than a
               different value */
            asking = 1;
            plen = 0;
            pass[0] = 0;
            saved = 0;
            return;
        }
    }
    saved = -1;                        /* refused - the name or the value */
}

/* The password typed in the window: if it is right, the same write is
   tried again - and the right is given back at once, because it was
   asked for one setting and not for the rest of the session. */
static void try_password(void)
{
    pass[plen] = 0;
    if (!plen) {                       /* nothing typed - never mind */
        asking = 0;
        return;
    }
    if (sys_auth(pass) != 0) {
        for (int i = 0; i < plen; i++)
            pass[i] = 0;
        plen = 0;
        saved = -1;
        asking = 0;
        return;
    }
    for (int i = 0; i < plen; i++)
        pass[i] = 0;
    plen = 0;
    asking = 0;
    save_all();
    sys_drop();
}

void _start(void)
{
    int w = (int)win_create(W0, H0, "Settings");
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
                        if (is_theme(cfg[row].name)) {
                            int k = 0;
                            for (int i = 0; i < THEME_N; i++)
                                if (same(edit, themes[i]))
                                    k = i;
                            const char *next = themes[(k + 1) % THEME_N];
                            elen = 0;
                            while (next[elen]) {
                                edit[elen] = next[elen];
                                elen++;
                            }
                            edit[elen] = 0;
                            saved = 0;
                            continue;
                        }
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
                if (asking) {
                    /* Everything typed now is the password, and none of
                       it goes anywhere near the value being edited */
                    if (c == '\n' || c == '\r')
                        try_password();
                    else if (c == '\b') {
                        if (plen > 0)
                            pass[--plen] = 0;
                    } else if (c >= 0x20 && c < 0x7F &&
                               plen < (int)sizeof(pass) - 1) {
                        pass[plen++] = c;
                    }
                    continue;
                }
                if (c == 0x1A) {           /* Ctrl+Insert: copy */
                    edit[elen] = 0;
                    if (elen)
                        sys_clip_set(edit, (uint32_t)elen);
                    continue;
                }
                if (c == 0x1B) {           /* Shift+Insert: paste */
                    char got[CLIP_MAX + 1];
                    int n = (int)sys_clip_get(got, sizeof(got));
                    for (int i = 0; i < n &&
                                    elen < (int)sizeof(edit) - 1; i++) {
                        if (got[i] < 0x20 || got[i] > 0x7E)
                            continue;
                        edit[elen++] = got[i];
                    }
                    edit[elen] = 0;
                    saved = 0;
                    continue;
                }
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

        /* --- drawing ---
         *
         * In the desktop's own colours, asked for every time: a switch
         * of the theme is followed on the next redraw. */
        struct ud_look l;
        ud_look(&l);
        ud_fill(w, 0, 0, cw, ch, l.bg);
        ud_text(w, 12, ud_baseline(6, 20, 14), "System settings", l.text,
                UD_BOLD, 14);

        for (int i = 0; i < ncfg; i++) {
            int y = ROW_Y + i * ROW_H;
            int base = ud_baseline(y, ROW_H, UD_PX);
            /* The selected row is marked, so that clicking and typing
               have a visible target - the accent, with its text on it */
            if (i == chosen)
                ud_round(w, 6, y, cw - 12, ROW_H, 5, l.accent);
            else if (i & 1)
                ud_round(w, 6, y, cw - 12, ROW_H, 5, l.row);
            uint32_t ink = i == chosen ? l.on_accent : l.text;
            ud_text(w, 14, base, cfg[i].name, ink, UD_SANS, UD_PX);
            int end = ud_text(w, VAL_X, base, i == chosen ? edit : cfg[i].value,
                              i == chosen ? l.on_accent : l.dim, UD_SANS,
                              UD_PX);
            /* Where the next typed character goes */
            if (i == chosen && !asking)
                ud_fill(w, end + 1, y + 3, 1, ROW_H - 6, l.on_accent);
        }

        /* What the selected setting is for, and how to change it */
        int y = ROW_Y + ncfg * ROW_H + 8;
        ud_fill(w, 12, y - 4, cw - 24, 1, l.line);
        if (chosen >= 0 && chosen < ncfg) {
            /* The description may be two lines - a line is drawn at a
               time, so we cut it at the newline ourselves */
            char line[CFG_HELP_MAX];
            const char *h = cfg[chosen].help;
            while (*h) {
                int n = 0;
                while (h[n] && h[n] != '\n' && n < (int)sizeof(line) - 1)
                    n++;
                for (int k = 0; k < n; k++)
                    line[k] = h[k];
                line[n] = 0;
                ud_text(w, 12, y + 11, line, l.dim, UD_SANS, 11);
                y += 14;
                h += n;
                if (*h == '\n')
                    h++;
            }
            ud_text(w, 12, y + 13,
                    (cycle_limit(cfg[chosen].name) ||
                     is_theme(cfg[chosen].name))
                        ? "A click on the line changes it, Enter saves"
                        : "Type on the keyboard, Enter saves",
                    l.accent, UD_SANS, 11);
        }

        /* The bottom of the window: the password when it is asked for,
           otherwise what became of the last change - always in the
           same place, so it is not looked for */
        if (asking) {
            /* Dots, not what was typed: the person needs to see that
               the keys are arriving, and nothing more than that */
            int bx = 12, by = ch - PASS_DY, bw = cw - 24;
            ud_round(w, bx, by, bw, 22, 6, l.field);
            ud_ring(w, bx, by, bw, 22, 6, l.accent);
            int x = ud_text(w, bx + 8, ud_baseline(by, 22, UD_PX),
                            "Admin password:", l.dim, UD_SANS, UD_PX) + 8;
            for (int i = 0; i < plen && x < bx + bw - 10; i++, x += 9)
                ud_disc(w, x, by + 8, 6, l.text);
            ud_text(w, 12, ch - NOTE_DY, "Changing settings needs admin; "
                    "Enter confirms", l.dim, UD_SANS, 11);
        } else if (saved > 0) {
            ud_text(w, 12, ch - NOTE_DY, "Saved to the disk", l.good, UD_BOLD,
                    UD_PX);
        } else if (saved < 0) {
            ud_text(w, 12, ch - NOTE_DY,
                    "NOT saved (wrong password or a bad value)", l.danger,
                    UD_BOLD, UD_PX);
        }

        sys_sleep(80);
    }
}
