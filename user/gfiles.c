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
 * the directory in the window's own title, "Edytuj" that starts the
 * editor on the chosen file, and "Usun" that asks first - the second
 * click is the answer, the same way the editor's second Alt+F4 is.
 *
 * The one thing that depends on the processor mode is reading a
 * directory: protected mode hands back the size in the return value,
 * long mode fills in a structure. Everything else - the list, the
 * clicking, the drawing - is one piece of code compiled twice.
 */

#include "uapi.h"

#define C_BG    8             /* the window's grey */
#define C_WHITE 4
#define C_BLUE  3
#define C_GREEN 2
#define C_RED   1

#define W0 320
#define H0 220
#define HEAD_H 12
#define ROW_Y0 18
#define ROW_H 10
#define MAXF 64               /* names kept; the window scrolls through them */
#define NAME_LEN 32
#define EDIT_W 56             /* the three buttons, kept at the right edge */
#define DEL_W 48
#define NAME_W 56

struct entry {
    char name[NAME_LEN];
    unsigned size;
    int is_dir;
};

static struct entry items[MAXF];
static int count;
static int top;               /* the first row shown */
static int chosen = -1;       /* which row, or -1 */
static int confirm;           /* a deletion has been asked about once */
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
static int del_x(void)  { return cw - DEL_W; }
static int edit_x(void) { return cw - DEL_W - EDIT_W - 4; }
static int name_x(void) { return cw - DEL_W - EDIT_W - NAME_W - 8; }

static int rows_shown(void)
{
    int n = (ch - ROW_Y0 - 12) / ROW_H;
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
       managers open on two directories are two windows called "Pliki"
       otherwise. */
    char t[80];
    int p = 0;
    const char *pre = "Pliki: ";
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
    confirm = 0;
    where_am_i();
}

/* --- what the buttons do ------------------------------------------ */

static void open_chosen(void)
{
    if (chosen < 0 || chosen >= count)
        return;
    if (items[chosen].is_dir) {
        if (sys_chdir(items[chosen].name) < 0) {
            ecopy(note, "nie da sie tam wejsc", sizeof(note));
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
    int picture = ln > 4 && nm[ln - 4] == '.' && nm[ln - 3] == 'p' &&
                  nm[ln - 2] == 's' && nm[ln - 1] == 'c';
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
    ecopy(note, picture ? "otwarte w podgladzie" : "otwarte w edytorze",
          sizeof(note));
}

/* Deleting asks first. A file manager that throws a file away on one
   click is a file manager one is afraid to use - and the second click
   is an answer, not a repetition. */
static void delete_chosen(void)
{
    if (chosen < 0 || chosen >= count) {
        ecopy(note, "nie wybrano pliku", sizeof(note));
        return;
    }
    if (items[chosen].is_dir) {
        ecopy(note, "to katalog - nie usuwam", sizeof(note));
        confirm = 0;
        return;
    }
    if (!confirm) {
        confirm = 1;
        int p = 0;
        const char *q = "Usunac '";
        for (int i = 0; q[i]; i++)
            note[p++] = q[i];
        for (int i = 0; items[chosen].name[i] && p < 30; i++)
            note[p++] = items[chosen].name[i];
        const char *r = "'? Kliknij jeszcze raz";
        for (int i = 0; r[i] && p < (int)sizeof(note) - 1; i++)
            note[p++] = r[i];
        note[p] = '\0';
        return;
    }
    if (sys_unlink(items[chosen].name) < 0)
        ecopy(note, "nie da sie usunac", sizeof(note));
    else
        ecopy(note, "usuniete", sizeof(note));
    confirm = 0;
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
        ecopy(note, "nie da sie - nazwa zajeta?", sizeof(note));
        return;
    }
    ecopy(note, "zmieniona nazwa", sizeof(note));
    chosen = -1;
    reread();
}

static void go_up(void)
{
    if (same(here, "/")) {
        ecopy(note, "to juz korzen", sizeof(note));
        return;
    }
    if (sys_chdir("..") < 0) {
        ecopy(note, "nie da sie wyjsc wyzej", sizeof(note));
        return;
    }
    chosen = -1;
    top = 0;
    note[0] = '\0';
    reread();
}

/* --- drawing ------------------------------------------------------- */

static void draw(void)
{
    int win = winid;
    int shown = rows_shown();

    win_rect(win, 0, 0, cw, ch, C_BG);

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
    win_text(win, 4, 2, head, C_GREEN);

    win_rect(win, name_x(), 0, NAME_W, HEAD_H, asking ? C_GREEN : C_BLUE);
    win_text(win, name_x() + 4, 2, "Nazwa", C_WHITE);
    win_rect(win, edit_x(), 0, EDIT_W, HEAD_H, C_BLUE);
    win_text(win, edit_x() + 4, 2, "Otworz", C_WHITE);
    win_rect(win, del_x(), 0, DEL_W, HEAD_H, confirm ? C_RED : C_BLUE);
    win_text(win, del_x() + 8, 2, "Usun", C_WHITE);
    win_rect(win, 0, ROW_Y0 - 3, cw, 1, C_WHITE);

    for (int i = 0; i < shown; i++) {
        int idx = top + i;
        if (idx >= count)
            break;
        int y = ROW_Y0 + i * ROW_H;
        int picked = idx == chosen;
        if (picked)
            win_rect(win, 0, y - 1, cw, ROW_H, C_BLUE);
        win_text(win, 6, y, items[idx].name,
                 items[idx].is_dir ? C_GREEN : C_WHITE);
        if (items[idx].is_dir) {
            /* Right-aligned, like the sizes it stands in place of: two
               columns that end in different places read as two
               columns. */
            win_text(win, cw - 7 * 8 - 6, y, "katalog",
                     picked ? C_WHITE : C_GREEN);
        } else {
            char sz[16];
            int n = num(sz, items[idx].size);
            sz[n++] = ' ';
            sz[n++] = 'B';
            sz[n] = '\0';
            win_text(win, cw - 8 * n - 6, y, sz, C_WHITE);
        }
    }

    /* Which part of the list is on the screen - a list that scrolls
       without saying so is a list that seems to have lost things. It
       goes at the BOTTOM: the top row belongs to the buttons now. */
    if (count > shown) {
        char more[24];
        int n = 0;
        more[n++] = '[';
        n += num(more + n, (unsigned)(top + 1));
        more[n++] = '-';
        n += num(more + n, (unsigned)(top + shown > count ? count
                                                          : top + shown));
        more[n++] = '/';
        n += num(more + n, (unsigned)count);
        more[n++] = ']';
        more[n] = '\0';
        win_text(win, cw - 8 * n - 4, ch - 10, more, C_WHITE);
    }

    if (asking) {
        char line[64];
        int p = 0;
        const char *q = "Nowa nazwa: ";
        for (int i = 0; q[i]; i++)
            line[p++] = q[i];
        for (int i = 0; i < alen && p < (int)sizeof(line) - 2; i++)
            line[p++] = answer[i];
        line[p++] = '_';
        line[p] = '\0';
        win_text(win, 4, ch - 10, line, C_GREEN);
    } else if (note[0]) {
        win_text(win, 4, ch - 10, note, confirm ? C_RED : C_WHITE);
    } else {
        win_text(win, 4, ch - 10,
                 "PgUp/PgDn przewija, '..' wyzej", C_GREEN);
    }
}

void _start(void)
{
    int win = (int)win_create(W0, H0, "Pliki");
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
            if (ev.type == WEV_CLOSE)
                sys_exit(0);
            if (ev.type == WEV_SIZE) {
                cw = ev.x;
                ch = ev.y;
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
                            ecopy(note, "nazwa bez zmian", sizeof(note));
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
                   on those as well means one click on "Otworz"
                   starting the editor twice, which is exactly what it
                   did. */
                if (ev.y < HEAD_H && ev.x >= del_x()) {
                    delete_chosen();
                } else if (ev.y < HEAD_H && ev.x >= name_x() &&
                           ev.x < edit_x()) {
                    if (chosen < 0)
                        ecopy(note, "nie wybrano pliku", sizeof(note));
                    else {
                        asking = 1;
                        alen = 0;
                        confirm = 0;
                    }
                } else if (ev.y < HEAD_H && ev.x >= edit_x()) {
                    open_chosen();
                } else if (ev.y >= ROW_Y0 - 1) {
                    int r = (ev.y - ROW_Y0 + 1) / ROW_H + top;
                    if (r >= 0 && r < count) {
                        if (r != chosen)
                            confirm = 0;    /* another file, another question */
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
            int keep = confirm;
            char kept[48];
            ecopy(kept, note, sizeof(kept));
            reread();
            confirm = keep;
            ecopy(note, kept, sizeof(note));
            if (was != count)
                again = 1;
        }
        if (again)
            draw();
        sys_sleep(100);
    }
}
