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
static int  dirty;            /* changed since the last save */

static char fname[NAME_MAX + 1];
static char note[48];         /* the last thing that happened */

static int  cw = W0, ch = H0; /* the content area, as the desktop says */

/* Asking for a file name: the status line becomes the question and
   what is typed goes into 'answer' until Enter. 0 = editing. */
#define ASK_NONE 0
#define ASK_SAVE 1
#define ASK_OPEN 2
static int  asking;
static char answer[NAME_MAX + 1];
static int  alen;

static int rows(void) { return (ch - PAD * 2 - STATUS_H) / ROW_H; }
static int cols(void) { return (cw - PAD * 2) / 8; }

static void ecopy(char *d, const char *s, int max)
{
    int i = 0;
    while (s[i] && i < max) {
        d[i] = s[i];
        i++;
    }
    d[i] = '\0';
}

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

static void clear_all(void)
{
    for (int i = 0; i < MAXL; i++)
        llen[i] = 0, text[i][0] = '\0';
    nlines = 1;
    cy = cx = top = hoff = 0;
    dirty = 0;
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
    cx++;
    dirty = 1;
}

/* Enter: what is to the right of the caret goes down with it. */
static void split_line(void)
{
    if (nlines >= MAXL)
        return;
    for (int i = nlines; i > cy + 1; i--) {
        ecopy(text[i], text[i - 1], MAXC);
        llen[i] = llen[i - 1];
    }
    nlines++;
    int rest = llen[cy] - cx;
    for (int i = 0; i < rest; i++)
        text[cy + 1][i] = text[cy][cx + i];
    text[cy + 1][rest] = '\0';
    llen[cy + 1] = rest;
    text[cy][cx] = '\0';
    llen[cy] = cx;
    cy++;
    cx = 0;
    dirty = 1;
}

/* Backspace at the start of a line joins it to the one above - which
   is the only way back from an Enter pressed by mistake. */
static void backspace(void)
{
    if (cx > 0) {
        for (int i = cx - 1; i < llen[cy]; i++)
            text[cy][i] = text[cy][i + 1];
        llen[cy]--;
        cx--;
        dirty = 1;
        return;
    }
    if (cy == 0)
        return;
    int up = cy - 1, at = llen[up];
    for (int i = 0; i < llen[cy] && at + i < MAXC; i++)
        text[up][at + i] = text[cy][i];
    llen[up] = at + llen[cy] > MAXC ? MAXC : at + llen[cy];
    text[up][llen[up]] = '\0';
    for (int i = cy; i < nlines - 1; i++) {
        ecopy(text[i], text[i + 1], MAXC);
        llen[i] = llen[i + 1];
    }
    nlines--;
    if (nlines < 1)
        nlines = 1;
    cy = up;
    cx = at;
    dirty = 1;
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

static void save_to(const char *name)
{
    int fd = (int)sys_open(name, O_WRITE | O_CREATE | O_TRUNC);
    if (fd < 0) {
        ecopy(note, "nie da sie zapisac", sizeof(note) - 1);
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
    ecopy(note, "zapisano", sizeof(note) - 1);
}

static void load_from(const char *name)
{
    int fd = (int)sys_open(name, O_READ);
    if (fd < 0) {
        ecopy(note, "nie ma takiego pliku", sizeof(note) - 1);
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
    ecopy(note, "wczytano", sizeof(note) - 1);
}

/* --- drawing ------------------------------------------------------ */

static void scroll_to_caret(void)
{
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
        const char *q = asking == ASK_SAVE ? "Zapisz jako: " : "Otworz: ";
        for (int i = 0; q[i]; i++)
            line[p++] = q[i];
        for (int i = 0; i < alen; i++)
            line[p++] = answer[i];
        line[p++] = '_';
    } else {
        const char *nm = fname[0] ? fname : "(bez nazwy)";
        for (int i = 0; nm[i] && p < 40; i++)
            line[p++] = nm[i];
        if (dirty)
            line[p++] = '*';
        line[p++] = ' ';
        line[p++] = 'W';
        char n[8];
        num(n, cy + 1);
        for (int i = 0; n[i]; i++)
            line[p++] = n[i];
        line[p++] = ' ';
        line[p++] = 'K';
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
}

/* --- keys --------------------------------------------------------- */

static void ask_key(char c)
{
    if (c == '\n') {
        int which = asking;
        asking = ASK_NONE;
        if (alen) {
            answer[alen] = '\0';
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
        asking = ASK_OPEN;
        alen = 0;
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
        ecopy(note, "skopiowano wiersz", sizeof(note) - 1);
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
    int win = (int)win_create(W0, H0, "Edytor");
    if (win < 0)
        sys_exit(1);

    clear_all();
    ecopy(note, "Ctrl+O otwiera, Ctrl+S zapisuje", sizeof(note) - 1);
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
            if (ev.type == WEV_CLOSE)
                sys_exit(0);
            if (ev.type == WEV_SIZE) {
                cw = ev.x;
                ch = ev.y;
            } else if (ev.type == WEV_KEY) {
                char c = (char)ev.val;
                if (asking)
                    ask_key(c);
                else
                    edit_key(c);
            } else if (ev.type == WEV_PRESS || ev.type == WEV_CLICK) {
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
