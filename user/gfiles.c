/* gfiles - a file manager in a window (ring 3)
 *
 * What was here before listed eight names in a window the size of a
 * postage stamp and showed the first hundred bytes of one of them.
 * That proved the path "disk -> click -> read -> drawing"; it was not
 * a file manager. A file manager is where a person GOES to find a
 * file: it says how big things are, it walks into directories and back
 * out of them, it hands a file to the program that opens it, and it
 * throws one away when asked twice.
 *
 * So: a list with sizes, directories marked and walkable, the name of
 * the directory in the window's own title, "Edit" that starts the
 * editor on the chosen file, and "Remove" that asks first - the second
 * click is the answer, the same way the editor's second Alt+F4 is.
 *
 * The one thing that depends on the processor mode is reading a
 * directory: protected mode hands back the size in the return value,
 * long mode fills in a structure. Everything else - the list, the
 * clicking, the drawing - is one piece of code compiled twice.
 */

#include "uapi.h"
#include "ugui.h"

#define W0 380
#define H0 270
#define HEAD_H 32             /* the heading and the three buttons */
#define BTN_H 22
#define ROW_Y0 38             /* the first row: under the heading, and a gap */
#define NAME_X 36             /* a name, right of its picture */
#define ROW_H 20
#define FOOT_H 24             /* the status line along the bottom */
#define MAXF 64               /* names kept; the window scrolls through them */
#define NAME_LEN 32
#define EDIT_W 56             /* the three buttons, kept at the right edge */
#define DEL_W 70
#define NAME_W 70

struct entry {
    char name[NAME_LEN];
    unsigned size;
    int is_dir;
};

static struct entry items[MAXF];
static int count;
static int top;               /* the first row shown */
static int chosen = -1;       /* which row, or -1 */
/* The question before a deletion. It used to be asked by making the
   second click on the same button mean something different from the
   first - which works, and which nobody can guess. Now it is a panel
   over the window with two buttons on it, and the words on them say
   what each does. */
static struct ug ui;
static struct ug_modal ask;
static char ask_line[64];
/* Renaming asks for the new name in the status line, the way the
   editor asks for a file name: a window to move, focus and close, for
   one word of text, is a window nobody wants. */
static int asking;
static char answer[NAME_LEN];
static int alen;
static char note[48];
static char here[64];         /* the directory we are looking at */

static int cw = W0, ch = H0;
static int winid = -1;

/* The buttons hold the right-hand edge, whatever the window has been
   pulled to - nailed to a number they end up in the middle of a
   maximised window. */
static int del_x(void)  { return cw - 10 - DEL_W; }
static int edit_x(void) { return del_x() - 6 - EDIT_W; }
static int name_x(void) { return edit_x() - 6 - NAME_W; }

static int rows_shown(void)
{
    int n = (ch - ROW_Y0 - FOOT_H - 2) / ROW_H;
    return n < 1 ? 1 : n;
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

static int same(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static int num(char *d, unsigned v)
{
    char tmp[12];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v && n < 11);
    for (int i = 0; i < n; i++)
        d[i] = tmp[n - 1 - i];
    d[n] = '\0';
    return n;
}

/* --- the one thing that depends on the kernel underneath ---------- */

#ifdef __x86_64__
static int read_entry(int index, struct entry *out)
{
    struct dirent64 e;
    if (sys_readdir(index, &e) != 0)
        return -1;
    ecopy(out->name, e.name, NAME_LEN);
    out->size = e.size;
    out->is_dir = e.is_dir;
    return 0;
}
#else
static int read_entry(int index, struct entry *out)
{
    char name[FS_NAME_MAX + 1];
    int32_t r = sys_readdir(index, name);
    if (r == -1)
        return -1;
    ecopy(out->name, name, NAME_LEN);
    out->is_dir = (r == -2);
    out->size = out->is_dir ? 0 : (unsigned)r;
    return 0;
}
#endif

static void where_am_i(void)
{
#ifdef __x86_64__
    sys_getcwd(here, (uint64_t)sizeof(here));
#else
    sys_getcwd(here);
#endif
    if (!here[0])
        ecopy(here, "/", sizeof(here));

    /* The window says which directory it is showing - two file
       managers open on two directories are two windows called "Files"
       otherwise. */
    char t[80];
    int p = 0;
    const char *pre = "Files: ";
    for (int i = 0; pre[i]; i++)
        t[p++] = pre[i];
    for (int i = 0; here[i] && p < (int)sizeof(t) - 1; i++)
        t[p++] = here[i];
    t[p] = '\0';
    if (winid >= 0)
        win_title(winid, t);
}

static void reread(void)
{
    count = 0;
    for (int i = 0; i < 128 && count < MAXF; i++) {
        if (read_entry(i, &items[count]) != 0)
            break;
        count++;
    }
    if (top > count - 1)
        top = 0;
    if (chosen >= count)
        chosen = -1;
    where_am_i();
}

/* --- what the buttons do ------------------------------------------ */

static void open_chosen(void)
{
    if (chosen < 0 || chosen >= count)
        return;
    if (items[chosen].is_dir) {
        if (sys_chdir(items[chosen].name) < 0) {
            ecopy(note, "cannot go in there", sizeof(note));
            return;
        }
        chosen = -1;
        top = 0;
        note[0] = '\0';
        reread();
        return;
    }
    /* A file goes to the program that opens it: a picture to the
       viewer, everything else to the editor. The name is what tells
       them apart - looking inside the file to decide would mean
       reading every file in the directory to draw a list. */
    const char *nm = items[chosen].name;
    int ln = 0;
    while (nm[ln])
        ln++;
    /* The endings the viewer knows: the machine's own pictures, and
       the formats that come from outside it. */
    int picture = 0;
    if (ln > 4 && nm[ln - 4] == '.') {
        const char *e = nm + ln - 3;
        picture = (e[0] == 'p' && e[1] == 's' && e[2] == 'c') ||
                  (e[0] == 'p' && e[1] == 'n' && e[2] == 'g') ||
                  (e[0] == 'b' && e[1] == 'm' && e[2] == 'p') ||
                  (e[0] == 'j' && e[1] == 'p' && e[2] == 'g');
    }
    if (!picture && ln > 5 && nm[ln - 5] == '.') {
        const char *e = nm + ln - 4;
        picture = e[0] == 'j' && e[1] == 'p' && e[2] == 'e' && e[3] == 'g';
    }
    const char *prog = picture ? "gimg" : "gedit";

    char cmd[NAME_LEN + 8];
    int n = 0;
    const char *pre = picture ? "gimg " : "gedit ";
    for (int i = 0; pre[i]; i++)
        cmd[n++] = pre[i];
    ecopy(cmd + n, items[chosen].name, (int)sizeof(cmd) - n);
#ifdef __x86_64__
    int64_t pid = sys_fork();
    if (pid == 0) {
        char *args[3];
        args[0] = (char *)prog;
        args[1] = items[chosen].name;
        args[2] = 0;
        sys_exec(prog, args);
        sys_exit(127);
    }
#else
    (void)prog;
    sys_spawn(cmd, 0, 0);
#endif
    ecopy(note, picture ? "opened in the viewer" : "opened in the editor",
          sizeof(note));
}

/* Deleting asks first. A file manager that throws a file away on one
   click is a file manager one is afraid to use - and the second click
   is an answer, not a repetition. */
static void delete_chosen(void)
{
    if (chosen < 0 || chosen >= count) {
        ecopy(note, "no file picked", sizeof(note));
        return;
    }
    if (items[chosen].is_dir) {
        ecopy(note, "a directory - not removing it", sizeof(note));
        return;
    }

    int p = 0;
    const char *q = "Remove '";
    for (int i = 0; q[i]; i++)
        ask_line[p++] = q[i];
    for (int i = 0; items[chosen].name[i] && p < 40; i++)
        ask_line[p++] = items[chosen].name[i];
    ask_line[p++] = '\'';
    ask_line[p++] = '?';
    ask_line[p] = '\0';

    ug_ask(&ask, ask_line, "This cannot be undone.", "Remove", "Keep");
}

/* The answer. Only "Remove" removes anything - and that is the whole
   point of asking. */
static void delete_answered(void)
{
    if (!ask.answer) {
        ecopy(note, "kept", sizeof(note));
        return;
    }
    if (chosen < 0 || chosen >= count)
        return;
    if (sys_unlink(items[chosen].name) < 0)
        ecopy(note, "it cannot be removed", sizeof(note));
    else
        ecopy(note, "removed", sizeof(note));
    chosen = -1;
    reread();
}

/* The new name, once it has been typed. The blocks do not move - only
   the entry that names them - so this is instant whatever the file's
   size, which is the whole reason the filesystem grew a rename. */
static void rename_to(const char *what)
{
    if (chosen < 0 || chosen >= count || !what[0])
        return;
    if (sys_rename(items[chosen].name, what) < 0) {
        ecopy(note, "cannot - is the name taken?", sizeof(note));
        return;
    }
    ecopy(note, "renamed", sizeof(note));
    chosen = -1;
    reread();
}

static void go_up(void)
{
    if (same(here, "/")) {
        ecopy(note, "this is the root already", sizeof(note));
        return;
    }
    if (sys_chdir("..") < 0) {
        ecopy(note, "cannot go higher", sizeof(note));
        return;
    }
    chosen = -1;
    top = 0;
    note[0] = '\0';
    reread();
}

/* --- drawing ------------------------------------------------------- */

/* What a row is, drawn small at its left: a folder, or a sheet */
static void draw_kind(int win, const struct ud_look *l, int x, int y,
                      int is_dir, int picked)
{
    if (is_dir) {
        uint32_t c = picked ? l->on_accent : 0x5AB4F5;
        ud_round(win, x, y + 4, 7, 4, 2, c);
        ud_round(win, x, y + 6, 16, 11, 3, c);
    } else {
        ud_round(win, x + 2, y + 3, 12, 14, 2, picked ? l->on_accent : l->field);
        ud_ring(win, x + 2, y + 3, 12, 14, 2, picked ? l->on_accent : l->dim);
    }
}

static void draw(void)
{
    int win = winid;
    int shown = rows_shown();
    struct ud_look l;
    ud_look(&l);

    ud_fill(win, 0, 0, cw, ch, l.bg);

    /* The heading says where we are and how much is here */
    char head[80];
    int p = 0;
    for (int i = 0; here[i] && p < 20; i++)
        head[p++] = here[i];
    head[p++] = ' ';
    head[p++] = '(';
    p += num(head + p, (unsigned)count);
    head[p++] = ')';
    head[p] = '\0';
    ud_text(win, 12, ud_baseline(0, HEAD_H, 13), head, l.text, UD_BOLD, 13);

    int by = (HEAD_H - BTN_H) / 2;
    ug_draw_button(win, &l, name_x(), by, NAME_W, BTN_H, "Rename",
                   asking ? l.danger : l.row, asking ? l.on_accent : l.text);
    ug_draw_button(win, &l, edit_x(), by, EDIT_W, BTN_H, "Open", l.accent,
                   l.on_accent);
    ug_draw_button(win, &l, del_x(), by, DEL_W, BTN_H, "Remove",
                   ask.on ? l.danger : l.row, ask.on ? l.on_accent : l.danger);
    ud_fill(win, 0, HEAD_H, cw, 1, l.line);

    for (int i = 0; i < shown; i++) {
        int idx = top + i;
        if (idx >= count)
            break;
        int y = ROW_Y0 + i * ROW_H;
        int picked = idx == chosen;
        if (picked)
            ud_round(win, 6, y, cw - 12, ROW_H, 6, l.accent);
        uint32_t ink = picked ? l.on_accent : l.text;
        uint32_t quiet = picked ? l.on_accent : l.dim;
        int base = ud_baseline(y, ROW_H, UD_PX);
        draw_kind(win, &l, 12, y, items[idx].is_dir, picked);
        ud_text(win, NAME_X, base, items[idx].name, ink,
                items[idx].is_dir ? UD_BOLD : UD_SANS, UD_PX);
        /* Right-aligned, like the sizes it stands in place of: two
           columns that end in different places read as two columns. */
        if (items[idx].is_dir) {
            ud_text_right(win, cw - 14, base, "folder", quiet, UD_SANS, UD_PX);
        } else {
            char sz[16];
            int n = num(sz, items[idx].size);
            sz[n++] = ' ';
            sz[n++] = 'B';
            sz[n] = '\0';
            ud_text_right(win, cw - 14, base, sz, quiet, UD_SANS, UD_PX);
        }
    }

    /* The status line along the bottom */
    int fy = ch - FOOT_H;
    ud_fill(win, 0, fy, cw, 1, l.line);
    int fb = ud_baseline(fy, FOOT_H, 11);

    /* Which part of the list is on the screen - a list that scrolls
       without saying so is a list that seems to have lost things. */
    if (count > shown) {
        char more[24];
        int n = 0;
        n += num(more + n, (unsigned)(top + 1));
        more[n++] = '-';
        n += num(more + n, (unsigned)(top + shown > count ? count
                                                          : top + shown));
        more[n++] = ' ';
        more[n++] = 'o';
        more[n++] = 'f';
        more[n++] = ' ';
        n += num(more + n, (unsigned)count);
        more[n] = '\0';
        ud_text_right(win, cw - 12, fb, more, l.dim, UD_SANS, 11);
    }

    if (asking) {
        char line[64];
        int p2 = 0;
        const char *q = "New name: ";
        for (int i = 0; q[i]; i++)
            line[p2++] = q[i];
        for (int i = 0; i < alen && p2 < (int)sizeof(line) - 2; i++)
            line[p2++] = answer[i];
        line[p2++] = '_';
        line[p2] = '\0';
        ud_text(win, 12, fb, line, l.accent, UD_BOLD, 11);
    } else if (note[0]) {
        ud_text(win, 12, fb, note, l.text, UD_SANS, 11);
    } else {
        ud_text(win, 12, fb, "PgUp/PgDn scrolls, '..' goes up", l.dim,
                UD_SANS, 11);
    }

    /* Last, and therefore over everything: a question one can scroll
       away from is not a question. */
    ui.win = win;
    ui.w = cw;
    ui.h = ch;
    ug_modal_draw(&ui, &ask);
}

void _start(void)
{
    int win = (int)win_create(W0, H0, "Files");
    if (win < 0)
        sys_exit(1);
    winid = win;

    reread();
    draw();

    int ticks = 0;
    for (;;) {
        struct win_event ev;
        int again = 0;
        while (win_poll(win, &ev) == 1) {
            if (ask.on) {
                /* While the question is up it takes everything: that is
                   what makes it a question rather than a suggestion. */
                ui.win = win;
                ui.w = cw;
                ui.h = ch;
                int what = ug_modal_event(&ui, &ask, &ev);
                if (what == UG_CLOSED)
                    sys_exit(0);
                if (what == UG_PRESSED)
                    delete_answered();
                again = 1;
                continue;
            }
            if (ev.type == WEV_CLOSE)
                sys_exit(0);
            if (ev.type == WEV_SIZE) {
                cw = ev.x;
                ch = ev.y;
            } else if (ev.type == WEV_SCROLL) {
                /* Three rows a notch, and the choice is left where it
                   was: scrolling a list is looking at it, not moving
                   through it. */
                int shown = rows_shown();
                top -= ev.val * 3;
                if (top > count - shown)
                    top = count - shown;
                if (top < 0)
                    top = 0;
            } else if (ev.type == WEV_KEY) {
                char c = (char)ev.val;
                int shown = rows_shown();
                if (asking) {
                    if (c == '\n') {
                        asking = 0;
                        answer[alen] = '\0';
                        if (alen)
                            rename_to(answer);
                        else
                            ecopy(note, "the name is unchanged", sizeof(note));
                        alen = 0;
                    } else if (c == '\b') {
                        if (alen)
                            alen--;
                        else
                            asking = 0;      /* nothing typed = never mind */
                    } else if (c >= 0x20 && c < 0x7F &&
                               alen < NAME_LEN - 1) {
                        answer[alen++] = c;
                    }
                    again = 1;
                    continue;
                }
                if (c == KEY_PGDN && top + shown < count)
                    top += shown;
                else if (c == KEY_PGUP)
                    top = top > shown ? top - shown : 0;
                else if (c == KEY_DOWN && chosen + 1 < count)
                    chosen++;
                else if (c == KEY_UP && chosen > 0)
                    chosen--;
                else if (c == '\n')
                    open_chosen();
                else if (c == '\b')
                    go_up();
                if (chosen >= 0) {          /* keep the choice in sight */
                    if (chosen < top)
                        top = chosen;
                    if (chosen >= top + shown)
                        top = chosen - shown + 1;
                }
            } else if (ev.type == WEV_CLICK) {
                /* A click, and only a click. The compositor also sends
                   the press and the release that make one up - acting
                   on those as well means one click on "Open"
                   starting the editor twice, which is exactly what it
                   did. */
                if (ev.y < HEAD_H && ev.x >= del_x()) {
                    delete_chosen();
                } else if (ev.y < HEAD_H && ev.x >= name_x() &&
                           ev.x < edit_x()) {
                    if (chosen < 0)
                        ecopy(note, "no file picked", sizeof(note));
                    else {
                        asking = 1;
                        alen = 0;
                    }
                } else if (ev.y < HEAD_H && ev.x >= edit_x()) {
                    open_chosen();
                } else if (ev.y >= ROW_Y0) {
                    int r = (ev.y - ROW_Y0) / ROW_H + top;
                    if (r >= 0 && r < count) {
                        chosen = r;
                        note[0] = '\0';
                    }
                }
            }
            again = 1;
        }

        /* The disk is not ours alone - a terminal in the next window
           can write a file while this one is open. Twice a second is
           often enough to notice and rare enough to cost nothing. */
        if (++ticks >= 5 && !asking) {
            /* While a name is being typed the list stands still: a
               refresh under a question would rub out the question. */
            ticks = 0;
            int was = count;
            char kept[48];
            ecopy(kept, note, sizeof(kept));
            reread();
            ecopy(note, kept, sizeof(note));
            if (was != count)
                again = 1;
        }
        if (again)
            draw();
        sys_sleep(100);
    }
}
