/* gedit - a text editor in a desktop window (ring 3)
 *
 * The system could write files - with 'write' from the shell, with
 * 'echo' and a '>' - but it could not CHANGE one. Everything that
 * makes a file was one line typed at a prompt, and a line typed at a
 * prompt cannot go back to what it said a moment ago. That is the
 * difference between a machine that stores text and a machine one can
 * work on.
 *
 * So: a page of white with black text on it, a caret that goes where
 * the arrows and the mouse say, Enter that splits a line and Backspace
 * that joins two, Ctrl+S to save and Ctrl+O to open. The clipboard is
 * the same one the rest of the desktop uses (Ctrl+Insert copies the
 * line the caret is on, Shift+Insert puts text in at the caret), so
 * text moves between the editor, the terminal and everything else
 * without either of them knowing about the other.
 *
 * The status line at the bottom is the whole of the interface: what
 * the file is called, whether it has been changed since it was saved,
 * where the caret is, and - when the editor needs a file name - the
 * question, typed into the same line. A dialogue box would be a second
 * window to move, focus and close, for one line of text.
 *
 * Nothing here depends on the processor mode: it is one piece of code
 * compiled for both kernels.
 */

#include "uapi.h"
#include "ugui.h"

#define C_BLACK 0
#define C_BLUE  3
#define C_WHITE 4

#define C_PAGE  C_WHITE       /* the page */
#define C_TEXT  C_BLACK       /* what is written on it */
#define C_BAR   C_BLUE        /* the status line */
#define C_CARET C_BLUE

#define W0 420                /* the size the window opens with */
#define H0 260

#define MAXL 200              /* lines a file may have */
#define MAXC 96               /* and characters in a line */
#define ROW_H 10
#define PAD 4
#define STATUS_H 10
#define NAME_MAX 31

static char text[MAXL][MAXC + 1];
static int  llen[MAXL];
static int  nlines = 1;

static int  cy, cx;           /* the caret: which line, which column */
static int  top, hoff;        /* the first line and column shown */
/* Whether the page follows the caret.
 *
 * It always did, and for keys that is right: type anywhere in a long
 * file and the place you are typing has to be on the screen. The wheel
 * is the other thing - looking somewhere else without going there - so
 * it lets go, and the next key takes hold again. */
static int  follow_caret = 1;
static int  dirty;            /* changed since the last save */
/* Whether the person has already been told there is something to
   lose. Asking twice about the same thing is a program that does not
   believe the answer. */
static int  warned;

static char fname[NAME_MAX + 1];
static char note[48];         /* the last thing that happened */

static int  cw = W0, ch = H0; /* the content area, as the desktop says */

/* Asking for a file name: the status line becomes the question and
   what is typed goes into 'answer' until Enter. 0 = editing. */
#define ASK_NONE 0
#define ASK_SAVE 1
#define ASK_OPEN 2
#define ASK_FIND 3
static int  asking;
static char answer[NAME_MAX + 1];
static int  alen;
/* What was looked for last time. Ctrl+F and Enter on an empty line
   means "the same thing again", which is what a person expects after
   the first match is not the one they wanted. */
static char last_find[NAME_MAX + 1];

static int rows(void) { return (ch - PAD * 2 - STATUS_H) / ROW_H; }

static void ecopy(char *d, const char *s, int max)
{
    int i = 0;
    while (s[i] && i < max) {
        d[i] = s[i];
        i++;
    }
    d[i] = '\0';
}

/* --- what is on the disk, for the panel that opens a file -----------
 *
 * Reading a directory is the one thing that looks different in the two
 * kernels: in long mode the answer is a structure, in the older one a
 * name and a length in the return value. Everything above it - the
 * list, the panel, the opening - is the same code.
 */
#define FILES_MAX 64
static char names[FILES_MAX][NAME_MAX + 1];
static int nfiles;
static struct ug editor_ui;
static struct ug_pick picker;
static char picked[NAME_MAX + 1];

static const char *file_row(void *ctx, int i)
{
    (void)ctx;
    return (i >= 0 && i < nfiles) ? names[i] : "";
}

static void reread_files(void)
{
    nfiles = 0;
    for (int i = 0; i < 128 && nfiles < FILES_MAX; i++) {
#ifdef __x86_64__
        struct dirent64 e;
        if (sys_readdir(i, &e) != 0)
            break;
        if (e.is_dir)
            continue;
        ecopy(names[nfiles], e.name, NAME_MAX);
#else
        char name[FS_NAME_MAX + 1];
        int32_t r = sys_readdir(i, name);
        if (r == -1)
            break;
        if (r == -2)
            continue;                    /* a directory, not a file */
        ecopy(names[nfiles], name, NAME_MAX);
#endif
        nfiles++;
    }
}
static int cols(void) { return (cw - PAD * 2) / 8; }


static void num(char *d, int v)      /* a small unsigned number */
{
    char tmp[8];
    int n = 0;
    if (v < 0)
        v = 0;
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v && n < 7);
    for (int i = 0; i < n; i++)
        d[i] = tmp[n - 1 - i];
    d[n] = '\0';
}

/* --- the buffer ------------------------------------------------- */

/* How far back one can go; the log itself is described below, where
   the steps are written. */
#define UNDO_N 120
struct step {
    unsigned char kind;
    short line, col;
    char ch;
};
static struct step log[UNDO_N];
static int log_n;

static void clear_all(void)
{
    for (int i = 0; i < MAXL; i++)
        llen[i] = 0, text[i][0] = '\0';
    nlines = 1;
    cy = cx = top = hoff = 0;
    dirty = 0;
    warned = 0;
    log_n = 0;              /* another file, another history */
}

/* Taking it back.
 *
 * An editor without an undo is an editor one types in carefully, and
 * typing carefully is slower than typing and fixing. What is kept is
 * not the file but the CHANGES to it: a character put in, a character
 * taken out, a line split, two lines joined. Each of the four has an
 * obvious opposite, and doing the opposite in reverse order is the
 * whole of it - a hundred and twenty steps back cost two kilobytes,
 * where two hundred copies of the file would cost four megabytes.
 *
 * The oldest step is dropped when the log fills. A person who has
 * typed a hundred and twenty things since the mistake is not looking
 * for that mistake any more. */
#define U_INS   1     /* a character was put in at (line,col) */
#define U_DEL   2     /* a character was taken out of (line,col) */
#define U_SPLIT 3     /* the line was split at (line,col) */
#define U_JOIN  4     /* line+1 was joined onto line at col */

static void record(int kind, int line, int col, char ch)
{
    if (log_n == UNDO_N) {         /* drop the oldest */
        for (int i = 1; i < UNDO_N; i++)
            log[i - 1] = log[i];
        log_n--;
    }
    log[log_n].kind = (unsigned char)kind;
    log[log_n].line = (short)line;
    log[log_n].col = (short)col;
    log[log_n].ch = ch;
    log_n++;
}

/* The two operations on lines, written once: the editing keys use
   them, and so does the undo, in the other direction. */
static void split_at(int line, int col)
{
    if (nlines >= MAXL || line < 0 || line >= nlines)
        return;
    for (int i = nlines; i > line + 1; i--) {
        ecopy(text[i], text[i - 1], MAXC);
        llen[i] = llen[i - 1];
    }
    nlines++;
    int rest = llen[line] - col;
    if (rest < 0)
        rest = 0;
    for (int i = 0; i < rest; i++)
        text[line + 1][i] = text[line][col + i];
    text[line + 1][rest] = '\0';
    llen[line + 1] = rest;
    text[line][col] = '\0';
    llen[line] = col;
}

/* Joins line+1 onto line; returns where the seam is. */
static int join_at(int line)
{
    if (line < 0 || line + 1 >= nlines)
        return -1;
    int at = llen[line];
    for (int i = 0; i < llen[line + 1] && at + i < MAXC; i++)
        text[line][at + i] = text[line + 1][i];
    int total = at + llen[line + 1];
    llen[line] = total > MAXC ? MAXC : total;
    text[line][llen[line]] = '\0';
    for (int i = line + 1; i < nlines - 1; i++) {
        ecopy(text[i], text[i + 1], MAXC);
        llen[i] = llen[i + 1];
    }
    nlines--;
    if (nlines < 1)
        nlines = 1;
    return at;
}

static void insert_char(char c)
{
    if (llen[cy] >= MAXC)
        return;
    for (int i = llen[cy]; i > cx; i--)
        text[cy][i] = text[cy][i - 1];
    text[cy][cx] = c;
    llen[cy]++;
    text[cy][llen[cy]] = '\0';
    record(U_INS, cy, cx, c);
    cx++;
    dirty = 1;
}

/* Enter: what is to the right of the caret goes down with it. */
static void split_line(void)
{
    if (nlines >= MAXL)
        return;
    split_at(cy, cx);
    record(U_SPLIT, cy, cx, 0);
    cy++;
    cx = 0;
    dirty = 1;
}

/* Backspace at the start of a line joins it to the one above - which
   is the only way back from an Enter pressed by mistake. */
static void backspace(void)
{
    if (cx > 0) {
        char gone = text[cy][cx - 1];
        for (int i = cx - 1; i < llen[cy]; i++)
            text[cy][i] = text[cy][i + 1];
        llen[cy]--;
        cx--;
        record(U_DEL, cy, cx, gone);
        dirty = 1;
        return;
    }
    if (cy == 0)
        return;
    int up = cy - 1;
    int at = join_at(up);
    if (at < 0)
        return;
    record(U_JOIN, up, at, 0);
    cy = up;
    cx = at;
    dirty = 1;
}

/* One step back: the opposite of the last thing done. */
static void undo_one(void)
{
    if (!log_n) {
        ecopy(note, "there is nothing to undo", sizeof(note) - 1);
        return;
    }
    struct step st = log[--log_n];
    switch (st.kind) {
    case U_INS:                     /* it was put in - take it out */
        cy = st.line;
        cx = st.col;
        if (cy < nlines && cx < llen[cy]) {
            for (int i = cx; i < llen[cy]; i++)
                text[cy][i] = text[cy][i + 1];
            llen[cy]--;
        }
        break;
    case U_DEL:                     /* it was taken out - put it back */
        cy = st.line;
        cx = st.col;
        if (cy < nlines && llen[cy] < MAXC) {
            for (int i = llen[cy]; i > cx; i--)
                text[cy][i] = text[cy][i - 1];
            text[cy][cx] = st.ch;
            llen[cy]++;
            text[cy][llen[cy]] = '\0';
            cx++;
        }
        break;
    case U_SPLIT:                   /* it was split - join it back */
        join_at(st.line);
        cy = st.line;
        cx = st.col;
        break;
    case U_JOIN:                    /* it was joined - split it back */
        split_at(st.line, st.col);
        cy = st.line + 1;
        cx = 0;
        break;
    default:
        break;
    }
    dirty = 1;
    ecopy(note, "undone", sizeof(note) - 1);
}

static void insert_text(const char *s, int n)
{
    for (int i = 0; i < n; i++) {
        if (s[i] == '\n')
            split_line();
        else if (s[i] >= 0x20 && s[i] < 0x7F)
            insert_char(s[i]);
    }
}

/* --- the file ---------------------------------------------------- */

/* What the window is called: the program, and the file it holds. Three
   editors open on three files are three windows called "Editor"
   otherwise - and the taskbar shows the same name under the pointer,
   so this is the difference between finding a window and hunting for
   it. */
static int winid = -1;

static void retitle(void)
{
    char t[NAME_MAX + 12];
    int p = 0;
    const char *pre = "Editor";
    for (int i = 0; pre[i]; i++)
        t[p++] = pre[i];
    if (fname[0]) {
        t[p++] = ':';
        t[p++] = ' ';
        for (int i = 0; fname[i] && p < (int)sizeof(t) - 1; i++)
            t[p++] = fname[i];
    }
    t[p] = '\0';
    if (winid >= 0)
        win_title(winid, t);
}

static void save_to(const char *name)
{
    int fd = (int)sys_open(name, O_WRITE | O_CREATE | O_TRUNC);
    if (fd < 0) {
        ecopy(note, "it cannot be written", sizeof(note) - 1);
        return;
    }
    for (int i = 0; i < nlines; i++) {
        if (llen[i])
            sys_write(fd, text[i], (uint32_t)llen[i]);
        sys_write(fd, "\n", 1);
    }
    sys_close(fd);
    ecopy(fname, name, NAME_MAX);
    dirty = 0;
    warned = 0;
    ecopy(note, "saved", sizeof(note) - 1);
    retitle();
}

static void load_from(const char *name)
{
    int fd = (int)sys_open(name, O_READ);
    if (fd < 0) {
        ecopy(note, "there is no such file", sizeof(note) - 1);
        return;
    }
    clear_all();
    char chunk[256];
    int n;
    while ((n = (int)sys_read(fd, chunk, sizeof(chunk))) > 0) {
        for (int i = 0; i < n; i++) {
            char c = chunk[i];
            if (c == '\n') {
                if (nlines < MAXL) {
                    nlines++;
                    cy = nlines - 1;
                    cx = 0;
                }
            } else if (c >= 0x20 && c < 0x7F && llen[cy] < MAXC) {
                text[cy][llen[cy]++] = c;
                text[cy][llen[cy]] = '\0';
                cx = llen[cy];
            }
        }
    }
    sys_close(fd);
    /* A file that ends with a newline has no line after it - the last
       one is where the text stops, not where the file does. */
    if (nlines > 1 && llen[nlines - 1] == 0)
        nlines--;
    cy = cx = top = hoff = 0;
    dirty = 0;
    ecopy(fname, name, NAME_MAX);
    ecopy(note, "loaded", sizeof(note) - 1);
    retitle();
}

/* Looks for the text from just after the caret, and goes round the end
   of the file back to where it started: a search that stops at the
   bottom is a search one has to think about, and thinking about the
   search is not what the search is for. */
static int find_from(const char *what, int line, int col)
{
    int n = 0;
    while (what[n])
        n++;
    if (!n)
        return 0;

    for (int step = 0; step <= nlines; step++) {
        int l = (line + step) % nlines;
        int start = step ? 0 : col;
        for (int c = start; c + n <= llen[l]; c++) {
            int k = 0;
            while (k < n && text[l][c + k] == what[k])
                k++;
            if (k == n) {
                cy = l;
                cx = c;
                return 1;
            }
        }
    }
    return 0;
}

static void find_answer(const char *what)
{
    if (!what[0]) {
        ecopy(note, "there is nothing to look for", sizeof(note) - 1);
        return;
    }
    ecopy(last_find, what, NAME_MAX);
    if (find_from(what, cy, cx + 1)) {
        char n[8];
        num(n, cy + 1);
        int p = 0;
        const char *pre = "found on line ";
        for (int i = 0; pre[i]; i++)
            note[p++] = pre[i];
        for (int i = 0; n[i]; i++)
            note[p++] = n[i];
        note[p] = '\0';
    } else {
        ecopy(note, "not found", sizeof(note) - 1);
    }
}

/* --- drawing ------------------------------------------------------ */

static void scroll_to_caret(void)
{
    if (!follow_caret)
        return;
    int r = rows(), c = cols();
    if (cy < top)
        top = cy;
    if (cy >= top + r)
        top = cy - r + 1;
    if (top < 0)
        top = 0;
    if (cx < hoff)
        hoff = cx;
    if (cx >= hoff + c)
        hoff = cx - c + 1;
    if (hoff < 0)
        hoff = 0;
}

static void draw(int win)
{
    int r = rows(), c = cols();
    scroll_to_caret();

    win_rect(win, 0, 0, cw, ch, C_PAGE);

    char shown[MAXC + 1];
    for (int i = 0; i < r; i++) {
        int l = top + i;
        if (l >= nlines)
            break;
        int n = llen[l] - hoff;
        if (n < 0)
            n = 0;
        if (n > c)
            n = c;
        for (int k = 0; k < n; k++)
            shown[k] = text[l][hoff + k];
        shown[n] = '\0';
        if (n)
            win_text(win, PAD, PAD + i * ROW_H, shown, C_TEXT);
    }

    /* The caret: a hair BETWEEN two characters, where the next one
       will go. A block would cover the letter it stands on, and a bar
       inside the cell would eat its first column - both of them hide
       the very thing a person is looking at while they type. */
    int caret_x = PAD + (cx - hoff) * 8 - 1;
    if (caret_x < 0)
        caret_x = 0;
    win_rect(win, caret_x, PAD + (cy - top) * ROW_H, 1, 8, C_CARET);

    /* The status line: what the file is, whether it has been changed,
       where the caret is - or the question, when one is being asked. */
    win_rect(win, 0, ch - STATUS_H, cw, STATUS_H, C_BAR);
    char line[80];
    int p = 0;
    if (asking) {
        const char *q = asking == ASK_SAVE ? "Save as: "
                      : asking == ASK_OPEN ? "Open: "
                                           : "Find: ";
        for (int i = 0; q[i]; i++)
            line[p++] = q[i];
        for (int i = 0; i < alen; i++)
            line[p++] = answer[i];
        line[p++] = '_';
    } else {
        const char *nm = fname[0] ? fname : "(no name)";
        for (int i = 0; nm[i] && p < 40; i++)
            line[p++] = nm[i];
        if (dirty)
            line[p++] = '*';
        line[p++] = ' ';
        line[p++] = 'L';
        char n[8];
        num(n, cy + 1);
        for (int i = 0; n[i]; i++)
            line[p++] = n[i];
        line[p++] = ' ';
        line[p++] = 'C';
        num(n, cx + 1);
        for (int i = 0; n[i]; i++)
            line[p++] = n[i];
        if (note[0]) {
            line[p++] = ' ';
            line[p++] = '-';
            line[p++] = ' ';
            for (int i = 0; note[i] && p < 70; i++)
                line[p++] = note[i];
        }
    }
    line[p] = '\0';
    win_text(win, PAD, ch - STATUS_H + 1, line, C_WHITE);

    /* Last, over everything: the panel that opens a file. A list one
       can scroll the text under is not a question. */
    editor_ui.win = win;
    editor_ui.w = cw;
    editor_ui.h = ch;
    ug_pick_draw(&editor_ui, &picker);
}

/* --- keys --------------------------------------------------------- */

static void ask_key(char c)
{
    if (c == '\n') {
        int which = asking;
        asking = ASK_NONE;
        answer[alen] = '\0';
        if (which == ASK_FIND) {
            /* Nothing typed means "the same as last time" - which is
               how a search is repeated everywhere else. */
            find_answer(alen ? answer : last_find);
        } else if (alen) {
            if (which == ASK_SAVE)
                save_to(answer);
            else
                load_from(answer);
        }
        alen = 0;
        return;
    }
    if (c == '\b') {
        if (alen)
            alen--;
        return;
    }
    if (c >= 0x20 && c < 0x7F && alen < NAME_MAX)
        answer[alen++] = c;
}

static void edit_key(char c)
{
    note[0] = '\0';               /* anything typed answers the last note */
    switch (c) {
    case KEY_SAVE:
        if (fname[0])
            save_to(fname);       /* a file that has a name just gets saved */
        else {
            asking = ASK_SAVE;
            alen = 0;
        }
        return;
    case KEY_OPEN:
        /* A list of what is there, instead of a name typed from
           memory. Typing still works - the line under the list is the
           same line the question used to be. */
        reread_files();
        ug_pick_open(&picker, "Open a file:", file_row, (void *)0, nfiles,
                     picked, (int)sizeof(picked));
        return;
    case KEY_FIND:
        asking = ASK_FIND;
        alen = 0;
        return;
    case KEY_UNDO:
        undo_one();
        return;
    case KEY_UP:
        if (cy > 0)
            cy--;
        if (cx > llen[cy])
            cx = llen[cy];
        return;
    case KEY_DOWN:
        if (cy < nlines - 1)
            cy++;
        if (cx > llen[cy])
            cx = llen[cy];
        return;
    case KEY_LEFT:
        if (cx > 0)
            cx--;
        else if (cy > 0)
            cx = llen[--cy];
        return;
    case KEY_RIGHT:
        if (cx < llen[cy])
            cx++;
        else if (cy < nlines - 1) {
            cy++;
            cx = 0;
        }
        return;
    case KEY_HOME:
        cx = 0;
        return;
    case KEY_END:
        cx = llen[cy];
        return;
    case KEY_PGUP:
        cy -= rows();
        if (cy < 0)
            cy = 0;
        if (cx > llen[cy])
            cx = llen[cy];
        return;
    case KEY_PGDN:
        cy += rows();
        if (cy > nlines - 1)
            cy = nlines - 1;
        if (cx > llen[cy])
            cx = llen[cy];
        return;
    case 0x1A: {                  /* Ctrl+Insert - the line the caret is on */
        sys_clip_set(text[cy], (uint32_t)llen[cy]);
        ecopy(note, "the line was copied", sizeof(note) - 1);
        return;
    }
    case 0x1B: {                  /* Shift+Insert */
        char got[512];
        int n = (int)sys_clip_get(got, sizeof(got));
        if (n > 0)
            insert_text(got, n);
        return;
    }
    case '\n':
        split_line();
        return;
    case '\b':
        backspace();
        return;
    default:
        if (c >= 0x20 && c < 0x7F)
            insert_char(c);
        return;
    }
}

void _start(int argc, char **argv)
{
    int win = (int)win_create(W0, H0, "Editor");
    if (win < 0)
        sys_exit(1);
    winid = win;

    clear_all();
    ecopy(note, "Ctrl+O opens, Ctrl+S saves, Ctrl+F finds, "
                "Ctrl+Z undoes", sizeof(note) - 1);
    /* Started with a name - 'gedit notatka' - it opens that file at
       once. Making a person type the name a second time, in the
       program they have just told which file they mean, is the sort of
       thing that makes an editor a chore. */
    if (argc > 1 && argv && argv[1] && argv[1][0])
        load_from(argv[1]);
    draw(win);

    for (;;) {
        struct win_event ev;
        int drew = 0;
        while (win_poll(win, &ev) == 1) {
            if (picker.on) {
                editor_ui.win = win;
                editor_ui.w = cw;
                editor_ui.h = ch;
                int what = ug_pick_event(&editor_ui, &picker, &ev);
                if (what == UG_CLOSED)
                    sys_exit(0);
                if (what == UG_PRESSED && picker.answer && picked[0])
                    load_from(picked);
                drew = 0;
                draw(win);
                continue;
            }
            if (ev.type == WEV_CLOSE) {
                /* A file with changes in it is not something to lose
                   because a window was closed. The first request is
                   declined and turned into a question; the second one
                   is an answer to that question. */
                if (dirty && !warned) {
                    win_keep(win);
                    warned = 1;
                    ecopy(note, "Unsaved changes! Ctrl+S will save, "
                                "closing again will not",
                          sizeof(note) - 1);
                    drew = 1;
                    continue;
                }
                sys_exit(0);
            }
            if (ev.type == WEV_SIZE) {
                cw = ev.x;
                ch = ev.y;
            } else if (ev.type == WEV_SCROLL) {
                /* The page moves and the caret does not: looking
                   somewhere else in a file is not the same as putting
                   the caret there, and a person who scrolls and then
                   types expects to be typing where they were. */
                follow_caret = 0;
                top -= ev.val * 3;
                if (top > nlines - 1)
                    top = nlines - 1;
                if (top < 0)
                    top = 0;
            } else if (ev.type == WEV_KEY) {
                char c = (char)ev.val;
                follow_caret = 1;     /* a key means "where I am" again */
                if (asking)
                    ask_key(c);
                else
                    edit_key(c);
            } else if (ev.type == WEV_CLICK) {
                follow_caret = 1;     /* putting the caret somewhere */
                /* The caret goes where the pointer is - the nearest
                   place there is text, not the empty page past it. */
                int l = top + (ev.y - PAD) / ROW_H;
                if (l < 0)
                    l = 0;
                if (l > nlines - 1)
                    l = nlines - 1;
                cy = l;
                int col = hoff + (ev.x - PAD) / 8;
                if (col < 0)
                    col = 0;
                if (col > llen[cy])
                    col = llen[cy];
                cx = col;
            }
            drew = 1;
        }
        if (drew)
            draw(win);
        sys_sleep(40);
    }
}
