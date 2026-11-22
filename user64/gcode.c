/* gcode - Code: a place to work on a project
 *
 * The editor is a page for one file. Writing a program is more than
 * that: files that belong together, a way to run what they make and to
 * see what it said, the line an error points at, a shell in the same
 * folder. A development environment puts all of it in one window
 * around the editor, and the JetBrains ones settled how that looks: a
 * toolbar with the project's name on the left and Run on the right, a
 * stripe of tool windows down the left side - the project's tree, the
 * outline of the file at the top; Run, Terminal and Problems at the
 * bottom, which open under the editor - tabs over the editor and a
 * status bar under all of it. Code is laid out the same way, so a
 * person who has used one of them finds things where they look.
 *
 * A project is a folder. What Code knows about it besides its files -
 * the command that runs it, the files that were open - is kept in the
 * folder's .code directory, so a project goes where its folder goes.
 * The projects opened lately are listed in ~/.code-projects, newest
 * first; the last one comes back when Code starts, and with none the
 * window welcomes, with a list to choose from and New Project, which
 * makes a folder in ~/Projects from a template.
 *
 * Keys are the ones those environments use, as far as this keyboard
 * goes: Ctrl+S saves, Ctrl+Z and Ctrl+Shift+Z undo and redo, Ctrl+/
 * comments a line out, Ctrl+D doubles it and Ctrl+Y deletes it, Shift
 * with the arrows selects, Ctrl+C, X and V go through the desktop's
 * clipboard, Ctrl+F finds, Ctrl+O (or Ctrl+Shift+N) goes to a file,
 * Ctrl+R runs and Alt with an arrow goes to the tab beside.
 *
 * Built against musl: files of any length and a tree of any size need
 * memory that grows and is given back.
 */

#include "uapi.h"
#include "udraw.h"
#include "ulocale.h"
#include "uterm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The keys compat64.h leaves out (kernel/include/keyboard.h) */
#define KEY_INTR  0x03                /* Ctrl+C */
#define KEY_EOF   0x04                /* Ctrl+D */
#define KEY_COPY  0x1A                /* Ctrl+Insert */
#define KEY_PASTE 0x1B                /* Shift+Insert - and Escape */

/* The layout, in pixels of the window's content. The tests read these
   (scripts/tests/t_gcode.py), so they stay plain numbers. */
/* The size it opens with: as large as the screen takes, of these - the
   last one fits the smallest screen the desktop runs on */
#define W0 720
#define H0 480
#define W1 960
#define H1 620
#define W2 1180
#define H2 740
#define BAR_H 40              /* the toolbar */
#define STRIPE_W 40           /* the tool windows' buttons, down the left */
#define STRIPE_STEP 36
#define STRIPE_ICON 28
#define SIDE_W 220            /* a tool window on the left */
#define HEAD_H 32             /* a tool window's title */
#define TREE_ROW_H 22
#define TREE_INDENT 16
#define PLUS_R 30             /* New File in the Project title, from its right */
#define TAB_H 34
#define FIND_H 32
#define GUTTER_W 52           /* the line numbers */
#define CODE_PAD 8            /* between them and the text */
#define CELL 8                /* a column of text */
#define ROW_H 18              /* a line of text */
#define TEXT_PX 12
#define TEXT_BASE 13          /* the baseline, from a line's top */
#define BOTTOM_H 160          /* a tool window under the editor */
#define STATUS_H 26
#define UI_PX 12
#define BTN 28                /* a toolbar button */
#define RUN_R 112             /* Run's left edge, from the right */
#define STOP_R 76
#define SEARCH_R 40
#define CFG_W 168             /* the run configuration, left of Run */
#define POP_W 520             /* a popup over the window */
#define POP_Y 56
#define POP_TITLE_H 28
#define POP_IN_H 36
#define POP_ROW_H 26
#define POP_ROWS 10
#define WEL_SIDE_W 220        /* the welcome screen's left side */
#define WEL_HEAD_H 64
#define WEL_ROW_H 52
#define WEL_BTN_W 120
#define WEL_BTN_H 30
#define TPL_TOP 64            /* New Project: the templates */
#define TPL_ROW_H 32
#define FORM_X 120            /* and the form, from the right side's edge */
#define FORM_Y 40
#define FORM_W 320
#define FIELD_H 28
#define TABW 4                /* a tab stops every four columns */

#define DOC_MAX 16
#define UNDO_N 64
#define ENT_MAX 4000
#define OUT_MAX 600           /* lines a Run or Terminal window keeps */
#define OUT_COLS 240
#define PROB_MAX 64
#define SYM_MAX 200
#define RECENT_MAX 12
#define PATH_MAX_ 512

/* --- colours ------------------------------------------------------------- */

/* The JetBrains New UI palettes, dark and light: the desktop's theme says
   which, and the colours are the environment's own, because an IDE that
   looked like every other window would not be recognised as one */
struct pal {
    uint32_t editor, panel, border, text, ui, dim, accent, on_accent;
    uint32_t sel, cur, hover, tree_sel, gutter, gutter_cur;
    uint32_t kw, str, com, num, pre, fn, run, stop, err, match, field;
};

static const struct pal dark_pal = {
    0x1E1F22, 0x2B2D30, 0x1E1F22, 0xBCBEC4, 0xDFE1E5, 0x6F737A, 0x3574F0,
    0xFFFFFF, 0x214283, 0x26282E, 0x393B40, 0x2E436E, 0x4B5059, 0xA1A3AB,
    0xCF8E6D, 0x6AAB73, 0x7A7E85, 0x2AACB8, 0xB3AE60, 0x56A8F5, 0x5FB865,
    0xDB5C5C, 0xF75464, 0x32593D, 0x1E1F22
};

static const struct pal light_pal = {
    0xFFFFFF, 0xF7F8FA, 0xEBECF0, 0x080808, 0x000000, 0x818594, 0x3574F0,
    0xFFFFFF, 0xA6D2FF, 0xF5F8FE, 0xDFE1E5, 0xD5E1FF, 0xAEB3C2, 0x767A8A,
    0x0033B3, 0x067D17, 0x8C8C8C, 0x1750EB, 0x9E880D, 0x00627A, 0x208A3C,
    0xE55765, 0xE55765, 0xC8E6C9, 0xFFFFFF
};

static struct pal P;

/* --- small helpers --------------------------------------------------------- */

static void scopy(char *d, const char *s, size_t cap)
{
    size_t i = 0;
    if (!cap)
        return;
    for (; s && s[i] && i + 1 < cap; i++)
        d[i] = s[i];
    d[i] = 0;
}

static int lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

static int is_id(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '$' ||
           (c & 0x80);
}

static const char *base_name(const char *p)
{
    const char *b = strrchr(p, '/');
    return b ? b + 1 : p;
}

static const char *ext_of(const char *p)
{
    const char *b = base_name(p);
    const char *d = strrchr(b, '.');
    return d ? d + 1 : "";
}

static int write_all(int fd, const void *p, uint32_t n)
{
    const uint8_t *b = p;
    while (n) {
        int64_t w = sys_write(fd, b, n);
        if (w <= 0)
            return -1;
        b += w;
        n -= (uint32_t)w;
    }
    return 0;
}

/* A whole file into memory, zero-terminated; NULL if there is none */
static char *slurp(const char *path, int *len)
{
    int fd = (int)sys_open(path, O_READ);
    if (fd < 0)
        return NULL;
    int cap = 4096, n = 0;
    char *b = malloc((size_t)cap + 1);
    for (;;) {
        if (!b)
            break;
        if (cap - n < 2048) {
            char *g = realloc(b, (size_t)cap * 2 + 1);
            if (!g) {
                free(b);
                b = NULL;
                break;
            }
            b = g;
            cap *= 2;
        }
        int64_t r = sys_read(fd, b + n, (uint64_t)(cap - n));
        if (r <= 0)
            break;
        n += (int)r;
    }
    sys_close(fd);
    if (!b)
        return NULL;
    b[n] = 0;
    if (len)
        *len = n;
    return b;
}

static int spill(const char *path, const char *text, int len)
{
    int fd = (int)sys_open(path, O_WRITE | O_CREATE | O_TRUNC);
    if (fd < 0)
        return -1;
    int r = write_all(fd, text, (uint32_t)len);
    sys_close(fd);
    return r;
}

static uint64_t now_ms(void)
{
    return sys_ticks() * 10;           /* the timer ticks a hundred a second */
}

/* --- the state of the window --------------------------------------------- */

#define SCR_WELCOME 0
#define SCR_NEW     1
#define SCR_IDE     2

#define LEFT_NONE      0
#define LEFT_PROJECT   1
#define LEFT_STRUCTURE 2

#define BOT_NONE     0
#define BOT_RUN      1
#define BOT_TERMINAL 2
#define BOT_PROBLEMS 3

#define POP_NONE    0
#define POP_GOTO    1
#define POP_NEWFILE 2
#define POP_RUNCFG  3
#define POP_PROJECT 4
#define POP_OPEN    5

#define FOC_EDITOR   0
#define FOC_TERMINAL 1
#define FOC_FIND     2

static int win = -1;
static int cw = W0, ch = H0;
static int screen = SCR_WELCOME;
static int left_tool = LEFT_PROJECT;
static int bottom_tool = BOT_NONE;
static int popup = POP_NONE;
static int focus = FOC_EDITOR;

static char home[PATH_MAX_];
static char proj[PATH_MAX_];          /* the project's folder, absolute */
static char proj_name[64];
static char run_cmd[160];             /* how the project runs */

static char note[96];                 /* the last thing that happened */
static uint64_t note_at;

static void say(const char *s)
{
    scopy(note, s, sizeof(note));
    note_at = now_ms();
}

static void join(char *out, size_t cap, const char *a, const char *b)
{
    size_t n = 0;
    scopy(out, a, cap);
    n = strlen(out);
    if (n && out[n - 1] != '/' && n + 1 < cap)
        out[n++] = '/';
    scopy(out + n, b, cap - n);
}

/* --- documents ------------------------------------------------------------- */

#define L_TEXT 0
#define L_C    1
#define L_JS   2
#define L_PY   3
#define L_SH   4
#define L_MD   5
#define L_JSON 6
#define L_MAKE 7
#define L_CPP  8
#define L_RUST 9
#define L_JAVA 10

static const char *const lang_names[] = {
    "Plain text", "C", "JavaScript", "Python", "Shell Script", "Markdown",
    "JSON", "Makefile", "C++", "Rust", "Java"
};

/* The languages written the C way: // and block comments, braces */
static int c_like(int lang)
{
    return lang == L_C || lang == L_CPP || lang == L_JS || lang == L_RUST ||
           lang == L_JAVA;
}

struct snap {
    char *text;
    int len, cy, cx;
};

struct doc {
    char rel[256];                    /* where it is in the project */
    char **ln;
    int *len, *cap;
    int n, max;
    unsigned char *st;                /* the highlighter's state at each line */
    int st_ok;                        /* lines whose state is known */
    int cy, cx;                       /* the caret: line, byte */
    int sel, sy, sx;                  /* a selection from sy, sx to the caret */
    int pref;                         /* the column Up and Down keep to */
    int top, hoff;                    /* the first line and column shown */
    int dirty, lang;
    struct snap undo[UNDO_N], redo[UNDO_N];
    int undo_n, redo_n;
    int last_kind, run_len;           /* what the last change was */
};

static struct doc *docs[DOC_MAX];
static int doc_n, cur = -1;

static int lang_of(const char *rel)
{
    const char *e = ext_of(rel);
    const char *b = base_name(rel);
    if (!strcmp(e, "c") || !strcmp(e, "h"))
        return L_C;
    if (!strcmp(e, "cpp") || !strcmp(e, "cc") || !strcmp(e, "cxx") ||
        !strcmp(e, "hpp") || !strcmp(e, "hh"))
        return L_CPP;
    if (!strcmp(e, "rs"))
        return L_RUST;
    if (!strcmp(e, "java"))
        return L_JAVA;
    if (!strcmp(e, "js") || !strcmp(e, "mjs") || !strcmp(e, "ts"))
        return L_JS;
    if (!strcmp(e, "py"))
        return L_PY;
    if (!strcmp(e, "sh"))
        return L_SH;
    if (!strcmp(e, "md"))
        return L_MD;
    if (!strcmp(e, "json"))
        return L_JSON;
    if (!strcmp(b, "Makefile") || !strcmp(e, "mk"))
        return L_MAKE;
    return L_TEXT;
}

static int doc_grow(struct doc *d, int want)
{
    if (want <= d->max)
        return 0;
    int m = d->max ? d->max : 64;
    while (m < want)
        m *= 2;
    char **ln = realloc(d->ln, sizeof(char *) * (size_t)m);
    if (!ln)
        return -1;
    d->ln = ln;
    int *len = realloc(d->len, sizeof(int) * (size_t)m);
    if (!len)
        return -1;
    d->len = len;
    int *cap = realloc(d->cap, sizeof(int) * (size_t)m);
    if (!cap)
        return -1;
    d->cap = cap;
    unsigned char *st = realloc(d->st, (size_t)m);
    if (!st)
        return -1;
    d->st = st;
    d->max = m;
    return 0;
}

/* Room in line y for 'n' bytes */
static int line_room(struct doc *d, int y, int n)
{
    if (n + 1 <= d->cap[y])
        return 0;
    int c = d->cap[y] ? d->cap[y] : 16;
    while (c < n + 1)
        c *= 2;
    char *g = realloc(d->ln[y], (size_t)c);
    if (!g)
        return -1;
    d->ln[y] = g;
    d->cap[y] = c;
    return 0;
}

/* A new line at y, holding s[0..n) */
static int line_insert(struct doc *d, int y, const char *s, int n)
{
    if (doc_grow(d, d->n + 1) < 0)
        return -1;
    memmove(d->ln + y + 1, d->ln + y, sizeof(char *) * (size_t)(d->n - y));
    memmove(d->len + y + 1, d->len + y, sizeof(int) * (size_t)(d->n - y));
    memmove(d->cap + y + 1, d->cap + y, sizeof(int) * (size_t)(d->n - y));
    d->ln[y] = NULL;
    d->len[y] = 0;
    d->cap[y] = 0;
    d->n++;
    if (line_room(d, y, n) < 0)
        return -1;
    memcpy(d->ln[y], s, (size_t)n);
    d->len[y] = n;
    d->ln[y][n] = 0;
    return 0;
}

static void line_remove(struct doc *d, int y)
{
    free(d->ln[y]);
    memmove(d->ln + y, d->ln + y + 1, sizeof(char *) * (size_t)(d->n - y - 1));
    memmove(d->len + y, d->len + y + 1, sizeof(int) * (size_t)(d->n - y - 1));
    memmove(d->cap + y, d->cap + y + 1, sizeof(int) * (size_t)(d->n - y - 1));
    d->n--;
}

static void doc_clear(struct doc *d)
{
    for (int i = 0; i < d->n; i++)
        free(d->ln[i]);
    d->n = 0;
}

/* The text as one string, lines joined by newlines */
static char *doc_text(struct doc *d, int *out_len)
{
    int total = 0;
    for (int i = 0; i < d->n; i++)
        total += d->len[i] + 1;
    char *t = malloc((size_t)total + 1);
    if (!t)
        return NULL;
    int k = 0;
    for (int i = 0; i < d->n; i++) {
        memcpy(t + k, d->ln[i], (size_t)d->len[i]);
        k += d->len[i];
        if (i + 1 < d->n)
            t[k++] = '\n';
    }
    t[k] = 0;
    *out_len = k;
    return t;
}

static void doc_set_text(struct doc *d, const char *t, int n)
{
    doc_clear(d);
    int s = 0;
    for (int i = 0; i <= n; i++) {
        if (i == n || t[i] == '\n') {
            int e = i;
            if (e > s && t[e - 1] == '\r')
                e--;
            line_insert(d, d->n, t + s, e - s);
            s = i + 1;
        }
    }
    if (!d->n)
        line_insert(d, 0, "", 0);
    d->st_ok = 0;
}

static struct doc *doc_new(const char *rel)
{
    struct doc *d = calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    scopy(d->rel, rel, sizeof(d->rel));
    d->lang = lang_of(rel);
    d->last_kind = -1;
    return d;
}

static void doc_free(struct doc *d)
{
    doc_clear(d);
    free(d->ln);
    free(d->len);
    free(d->cap);
    free(d->st);
    for (int i = 0; i < d->undo_n; i++)
        free(d->undo[i].text);
    for (int i = 0; i < d->redo_n; i++)
        free(d->redo[i].text);
    free(d);
}

/* --- columns: tabs and UTF-8 ----------------------------------------------- */

/* The column byte x of line y starts at: a tab goes to the next stop and
   only the first byte of a UTF-8 character takes a column */
static int vcol(struct doc *d, int y, int x)
{
    int v = 0;
    const char *s = d->ln[y];
    for (int i = 0; i < x && i < d->len[y]; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\t')
            v = (v / TABW + 1) * TABW;
        else if ((c & 0xC0) != 0x80)
            v++;
    }
    return v;
}

/* The byte at column v of line y - the nearest one before it */
static int byte_at(struct doc *d, int y, int v)
{
    int c = 0;
    const char *s = d->ln[y];
    for (int i = 0; i < d->len[y]; i++) {
        unsigned char b = (unsigned char)s[i];
        if ((b & 0xC0) == 0x80)
            continue;
        int next = b == '\t' ? (c / TABW + 1) * TABW : c + 1;
        if (next > v)
            return (v - c) * 2 >= next - c && b != '\t' ? i + 1 : i;
        c = next;
    }
    return d->len[y];
}

static int next_char(struct doc *d, int y, int x)
{
    x++;
    while (x < d->len[y] && ((unsigned char)d->ln[y][x] & 0xC0) == 0x80)
        x++;
    return x;
}

static int prev_char(struct doc *d, int y, int x)
{
    x--;
    while (x > 0 && ((unsigned char)d->ln[y][x] & 0xC0) == 0x80)
        x--;
    return x;
}

/* --- undo ------------------------------------------------------------------ */

#define K_TYPE  1
#define K_ERASE 2
#define K_OTHER 3

static void snap_push(struct snap *stack, int *n, struct snap s)
{
    if (*n == UNDO_N) {
        free(stack[0].text);
        memmove(stack, stack + 1, sizeof(struct snap) * (UNDO_N - 1));
        (*n)--;
    }
    stack[(*n)++] = s;
}

static struct snap snap_take(struct doc *d)
{
    struct snap s;
    s.text = doc_text(d, &s.len);
    s.cy = d->cy;
    s.cx = d->cx;
    return s;
}

/* Before a change: typing in a row is one step to undo, as a person
   thinks of it, until something else is done or a line ends */
static void remember(struct doc *d, int kind)
{
    if (kind != K_OTHER && kind == d->last_kind && d->run_len < 40) {
        d->run_len++;
        return;
    }
    struct snap s = snap_take(d);
    if (!s.text)
        return;
    snap_push(d->undo, &d->undo_n, s);
    for (int i = 0; i < d->redo_n; i++)
        free(d->redo[i].text);
    d->redo_n = 0;
    d->last_kind = kind;
    d->run_len = 0;
}

static void caret_moved(struct doc *d)
{
    d->last_kind = -1;                 /* the next change starts a new step */
}

static void restore(struct doc *d, struct snap *from, int *from_n,
                    struct snap *to, int *to_n)
{
    if (!*from_n)
        return;
    struct snap now = snap_take(d);
    struct snap s = from[--(*from_n)];
    doc_set_text(d, s.text, s.len);
    d->cy = s.cy < d->n ? s.cy : d->n - 1;
    d->cx = s.cx <= d->len[d->cy] ? s.cx : d->len[d->cy];
    d->sel = 0;
    free(s.text);
    if (now.text)
        snap_push(to, to_n, now);
    d->dirty = 1;
    d->last_kind = -1;
}

/* --- changing the text ----------------------------------------------------- */

static void sel_bounds(struct doc *d, int *y0, int *x0, int *y1, int *x1)
{
    if (d->sy < d->cy || (d->sy == d->cy && d->sx <= d->cx)) {
        *y0 = d->sy; *x0 = d->sx; *y1 = d->cy; *x1 = d->cx;
    } else {
        *y0 = d->cy; *x0 = d->cx; *y1 = d->sy; *x1 = d->sx;
    }
}

static void touched(struct doc *d, int y)
{
    d->dirty = 1;
    if (d->st_ok > y)
        d->st_ok = y;
}

/* Text from y0, x0 to y1, x1 removed; the caret where it began */
static void erase(struct doc *d, int y0, int x0, int y1, int x1)
{
    if (y0 == y1) {
        memmove(d->ln[y0] + x0, d->ln[y0] + x1, (size_t)(d->len[y0] - x1 + 1));
        d->len[y0] -= x1 - x0;
    } else {
        int tail = d->len[y1] - x1;
        line_room(d, y0, x0 + tail);
        memcpy(d->ln[y0] + x0, d->ln[y1] + x1, (size_t)tail);
        d->len[y0] = x0 + tail;
        d->ln[y0][d->len[y0]] = 0;
        for (int y = y1; y > y0; y--)
            line_remove(d, y);
    }
    d->cy = y0;
    d->cx = x0;
    touched(d, y0);
}

static void erase_sel(struct doc *d)
{
    if (!d->sel)
        return;
    int y0, x0, y1, x1;
    sel_bounds(d, &y0, &x0, &y1, &x1);
    d->sel = 0;
    erase(d, y0, x0, y1, x1);
}

/* s[0..n) put in at the caret, newlines and all; the caret after it */
static void insert(struct doc *d, const char *s, int n)
{
    int y = d->cy, x = d->cx;
    int i = 0;
    touched(d, y);
    while (i <= n) {
        int j = i;
        while (j < n && s[j] != '\n')
            j++;
        int piece = j - i;
        if (line_room(d, y, d->len[y] + piece) < 0)
            return;
        memmove(d->ln[y] + x + piece, d->ln[y] + x,
                (size_t)(d->len[y] - x + 1));
        memcpy(d->ln[y] + x, s + i, (size_t)piece);
        d->len[y] += piece;
        x += piece;
        if (j == n)
            break;
        /* the rest of the line goes onto a new one */
        if (line_insert(d, y + 1, d->ln[y] + x, d->len[y] - x) < 0)
            return;
        d->len[y] = x;
        d->ln[y][x] = 0;
        y++;
        x = 0;
        i = j + 1;
    }
    d->cy = y;
    d->cx = x;
}

static char *sel_text(struct doc *d, int *n)
{
    int y0, x0, y1, x1;
    sel_bounds(d, &y0, &x0, &y1, &x1);
    int total = 0;
    for (int y = y0; y <= y1; y++)
        total += d->len[y] + 1;
    char *t = malloc((size_t)total + 1);
    if (!t)
        return NULL;
    int k = 0;
    for (int y = y0; y <= y1; y++) {
        int a = y == y0 ? x0 : 0;
        int b = y == y1 ? x1 : d->len[y];
        memcpy(t + k, d->ln[y] + a, (size_t)(b - a));
        k += b - a;
        if (y < y1)
            t[k++] = '\n';
    }
    t[k] = 0;
    *n = k;
    return t;
}

static int indent_of(struct doc *d, int y)
{
    int i = 0;
    while (i < d->len[y] && (d->ln[y][i] == ' ' || d->ln[y][i] == '\t'))
        i++;
    return i;
}

static const char *comment_of(int lang)
{
    switch (lang) {
    case L_C: case L_JS: case L_JSON: case L_CPP: case L_RUST: case L_JAVA:
        return "//";
    case L_PY: case L_SH: case L_MAKE:
        return "#";
    default:
        return NULL;
    }
}

/* --- colouring ------------------------------------------------------------- */

#define C_PLAIN 0
#define C_KW    1
#define C_STR   2
#define C_COM   3
#define C_NUM   4
#define C_PRE   5
#define C_FN    6

/* What a line can leave open for the next one */
#define S_NONE  0
#define S_BLOCK 1                     /* a C comment */
#define S_TRIPLE 2                    /* a Python string in three quotes */
#define S_TICK  3                     /* a JavaScript template string */

static const char *const kw_c[] = {
    "auto", "break", "case", "char", "const", "continue", "default", "do",
    "double", "else", "enum", "extern", "float", "for", "goto", "if",
    "inline", "int", "long", "register", "restrict", "return", "short",
    "signed", "sizeof", "static", "struct", "switch", "typedef", "union",
    "unsigned", "void", "volatile", "while", "bool", "true", "false", "NULL",
    "size_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t", "int8_t",
    "int16_t", "int32_t", "int64_t", 0
};
static const char *const kw_cpp[] = {
    "alignas", "alignof", "auto", "bool", "break", "case", "catch", "char",
    "class", "concept", "const", "consteval", "constexpr", "const_cast",
    "continue", "co_await", "co_return", "co_yield", "decltype", "default",
    "delete", "do", "double", "dynamic_cast", "else", "enum", "explicit",
    "export", "extern", "false", "final", "float", "for", "friend", "goto",
    "if", "inline", "int", "long", "mutable", "namespace", "new", "noexcept",
    "nullptr", "operator", "override", "private", "protected", "public",
    "reinterpret_cast", "requires", "return", "short", "signed", "sizeof",
    "static", "static_assert", "static_cast", "struct", "switch", "template",
    "this", "thread_local", "throw", "true", "try", "typedef", "typeid",
    "typename", "union", "unsigned", "using", "virtual", "void", "volatile",
    "while", "size_t", "std", "string", "vector", 0
};
static const char *const kw_rust[] = {
    "as", "async", "await", "break", "const", "continue", "crate", "dyn",
    "else", "enum", "extern", "false", "fn", "for", "if", "impl", "in",
    "let", "loop", "match", "mod", "move", "mut", "pub", "ref", "return",
    "self", "Self", "static", "struct", "super", "trait", "true", "type",
    "unsafe", "use", "where", "while", "i8", "i16", "i32", "i64", "i128",
    "u8", "u16", "u32", "u64", "u128", "usize", "isize", "f32", "f64",
    "bool", "char", "str", "String", "Vec", "Option", "Some", "None", "Ok",
    "Err", "Box", "Result", 0
};
static const char *const kw_java[] = {
    "abstract", "assert", "boolean", "break", "byte", "case", "catch",
    "char", "class", "continue", "default", "do", "double", "else", "enum",
    "extends", "final", "finally", "float", "for", "if", "implements",
    "import", "instanceof", "int", "interface", "long", "native", "new",
    "null", "package", "private", "protected", "public", "record", "return",
    "short", "static", "super", "switch", "synchronized", "this", "throw",
    "throws", "transient", "true", "false", "try", "var", "void",
    "volatile", "while", "String", 0
};
static const char *const kw_js[] = {
    "async", "await", "break", "case", "catch", "class", "const", "continue",
    "debugger", "default", "delete", "do", "else", "export", "extends",
    "false", "finally", "for", "function", "if", "import", "in",
    "instanceof", "let", "new", "null", "of", "return", "static", "super",
    "switch", "this", "throw", "true", "try", "typeof", "undefined", "var",
    "void", "while", "with", "yield", 0
};
static const char *const kw_py[] = {
    "and", "as", "assert", "async", "await", "break", "class", "continue",
    "def", "del", "elif", "else", "except", "False", "finally", "for",
    "from", "global", "if", "import", "in", "is", "lambda", "None",
    "nonlocal", "not", "or", "pass", "raise", "return", "True", "try",
    "while", "with", "yield", "self", 0
};
static const char *const kw_sh[] = {
    "if", "then", "else", "elif", "fi", "for", "while", "until", "do",
    "done", "case", "esac", "in", "function", "return", "export", "local",
    "exit", 0
};
static const char *const kw_json[] = { "true", "false", "null", 0 };

static int keyword(int lang, const char *s, int n)
{
    const char *const *k;
    switch (lang) {
    case L_C: k = kw_c; break;
    case L_CPP: k = kw_cpp; break;
    case L_RUST: k = kw_rust; break;
    case L_JAVA: k = kw_java; break;
    case L_JS: k = kw_js; break;
    case L_PY: k = kw_py; break;
    case L_SH: k = kw_sh; break;
    case L_JSON: k = kw_json; break;
    default: return 0;
    }
    for (; *k; k++)
        if ((int)strlen(*k) == n && !memcmp(*k, s, (size_t)n))
            return 1;
    return 0;
}

/* The class of each byte of a line, and what it leaves open */
static int colour_line(int lang, const char *s, int n, int state,
                       unsigned char *cls)
{
    const char *line_com = comment_of(lang);
    int lc = line_com ? (int)strlen(line_com) : 0;
    int i = 0;
    memset(cls, C_PLAIN, (size_t)n);
    if (lang == L_TEXT)
        return S_NONE;
    if (lang == L_MD) {
        if (n && s[0] == '#')
            memset(cls, C_KW, (size_t)n);
        for (int k = 0; k < n; k++) {
            if (s[k] == '`') {
                int e = k + 1;
                while (e < n && s[e] != '`')
                    e++;
                memset(cls + k, C_STR, (size_t)((e < n ? e + 1 : n) - k));
                k = e;
            }
        }
        return S_NONE;
    }
    if (lang == L_C || lang == L_CPP || lang == L_RUST) {
        int k = 0;
        while (k < n && (s[k] == ' ' || s[k] == '\t'))
            k++;
        if (state == S_NONE && k < n && s[k] == '#') {
            /* a directive, up to a comment; an include's name a string */
            int e = k + 1;
            while (e < n && !(s[e] == '/' && e + 1 < n &&
                              (s[e + 1] == '/' || s[e + 1] == '*')))
                e++;
            memset(cls + k, C_PRE, (size_t)(e - k));
            for (int q = k; q < e; q++)
                if (s[q] == '<' || s[q] == '"') {
                    memset(cls + q, C_STR, (size_t)(e - q));
                    break;
                }
            i = e;
        }
    }
    while (i < n) {
        if (state == S_BLOCK) {
            int e = i;
            while (e < n && !(s[e] == '*' && e + 1 < n && s[e + 1] == '/'))
                e++;
            if (e < n) {
                e += 2;
                state = S_NONE;
            }
            memset(cls + i, C_COM, (size_t)(e - i));
            i = e;
            continue;
        }
        if (state == S_TRIPLE || state == S_TICK) {
            int e = i;
            while (e < n) {
                if (s[e] == '\\') {
                    e += 2;
                    continue;
                }
                if (state == S_TICK && s[e] == '`') {
                    e++;
                    state = S_NONE;
                    break;
                }
                if (state == S_TRIPLE && e + 2 < n && s[e] == '"' &&
                    s[e + 1] == '"' && s[e + 2] == '"') {
                    e += 3;
                    state = S_NONE;
                    break;
                }
                e++;
            }
            if (e > n)
                e = n;
            memset(cls + i, C_STR, (size_t)(e - i));
            i = e;
            continue;
        }
        char c = s[i];
        if (lc && i + lc <= n && !memcmp(s + i, line_com, (size_t)lc) &&
            !(lang == L_SH && i > 0 && s[i - 1] == '$')) {
            memset(cls + i, C_COM, (size_t)(n - i));
            break;
        }
        if (c_like(lang) && c == '/' && i + 1 < n && s[i + 1] == '*') {
            cls[i] = cls[i + 1] = C_COM;
            i += 2;
            state = S_BLOCK;
            continue;
        }
        if (lang == L_PY && c == '"' && i + 2 < n && s[i + 1] == '"' &&
            s[i + 2] == '"') {
            memset(cls + i, C_STR, 3);
            i += 3;
            state = S_TRIPLE;
            continue;
        }
        if (lang == L_JS && c == '`') {
            cls[i++] = C_STR;
            state = S_TICK;
            continue;
        }
        /* a Rust lifetime, 'a, is not a character */
        if (lang == L_RUST && c == '\'' && i + 2 < n && is_id(s[i + 1]) &&
            s[i + 2] != '\'') {
            int e = i + 1;
            while (e < n && is_id(s[e]))
                e++;
            memset(cls + i, C_KW, (size_t)(e - i));
            i = e;
            continue;
        }
        /* a Java annotation */
        if (lang == L_JAVA && c == '@' && i + 1 < n && is_id(s[i + 1])) {
            int e = i + 1;
            while (e < n && is_id(s[e]))
                e++;
            memset(cls + i, C_PRE, (size_t)(e - i));
            i = e;
            continue;
        }
        if (c == '"' || c == '\'') {
            int e = i + 1;
            while (e < n && s[e] != c) {
                if (s[e] == '\\')
                    e++;
                e++;
            }
            if (e < n)
                e++;
            if (e > n)
                e = n;
            memset(cls + i, C_STR, (size_t)(e - i));
            i = e;
            continue;
        }
        if (c >= '0' && c <= '9' && (i == 0 || !is_id(s[i - 1]))) {
            int e = i;
            while (e < n && (is_id(s[e]) || s[e] == '.'))
                e++;
            memset(cls + i, C_NUM, (size_t)(e - i));
            i = e;
            continue;
        }
        if (is_id(c)) {
            int e = i;
            while (e < n && is_id(s[e]))
                e++;
            int k = e;
            while (k < n && s[k] == ' ')
                k++;
            if (keyword(lang, s + i, e - i))
                memset(cls + i, C_KW, (size_t)(e - i));
            else if (k < n && (s[k] == '(' ||
                               (lang == L_RUST && s[k] == '!')) && lang != L_JSON)
                memset(cls + i, C_FN, (size_t)(e - i));
            else if (lang == L_JSON && k < n && s[k] == ':')
                memset(cls + i, C_FN, (size_t)(e - i));
            i = e;
            continue;
        }
        i++;
    }
    if (state == S_TRIPLE || state == S_TICK || state == S_BLOCK)
        return state;
    return S_NONE;
}

/* The state every line starts in, worked out as far as line y */
static void states_upto(struct doc *d, int y)
{
    static unsigned char scratch[4096];
    if (d->st_ok < 1) {
        d->st[0] = S_NONE;
        d->st_ok = 1;
    }
    while (d->st_ok <= y && d->st_ok < d->n) {
        int p = d->st_ok - 1;
        unsigned char *cls = d->len[p] <= (int)sizeof(scratch) ? scratch :
                             malloc((size_t)d->len[p] + 1);
        if (!cls)
            return;
        d->st[d->st_ok] = (unsigned char)colour_line(d->lang, d->ln[p],
                                                     d->len[p], d->st[p], cls);
        if (cls != scratch)
            free(cls);
        d->st_ok++;
    }
}

/* --- the project: its files ------------------------------------------------ */

struct ent {
    char rel[200];
    int depth, is_dir, open;
};

static struct ent *ents;
static int ent_n;
static int tree_top;                  /* the first row of the tree shown */
static char tree_sel[200];            /* the row picked last */
static uint64_t scanned_at;

static int name_cmp(const char *a, const char *b)
{
    for (;; a++, b++) {
        int x = lower((unsigned char)*a), y = lower((unsigned char)*b);
        if (x != y || !x)
            return x - y;
    }
}

struct dentry {
    char name[64];
    int is_dir;
};

static int dentry_cmp(const void *a, const void *b)
{
    const struct dentry *x = a, *y = b;
    if (x->is_dir != y->is_dir)
        return y->is_dir - x->is_dir;  /* folders first */
    return name_cmp(x->name, y->name);
}

static int was_open(struct ent *old, int old_n, const char *rel)
{
    for (int i = 0; i < old_n; i++)
        if (old[i].is_dir && !strcmp(old[i].rel, rel))
            return old[i].open;
    return -1;
}

/* One folder's entries into ents, in tree order, and its folders' */
static void scan_dir(const char *rel, int depth, struct ent *old, int old_n)
{
    char abs[PATH_MAX_];
    if (rel[0])
        join(abs, sizeof(abs), proj, rel);
    else
        scopy(abs, proj, sizeof(abs));
    if (sys_chdir(abs) < 0)
        return;
    int cap = 64, n = 0;
    struct dentry *list = malloc(sizeof(*list) * (size_t)cap);
    struct dirent64 de;
    for (int i = 0; list && sys_readdir(i, &de) == 0; i++) {
        /* what starts with a dot is the tools' own: .code among them */
        if (de.name[0] == '.' || !de.name[0])
            continue;
        if (n == cap) {
            struct dentry *g = realloc(list, sizeof(*list) * (size_t)cap * 2);
            if (!g)
                break;
            list = g;
            cap *= 2;
        }
        scopy(list[n].name, de.name, sizeof(list[n].name));
        list[n].is_dir = de.is_dir;
        n++;
    }
    if (!list)
        return;
    qsort(list, (size_t)n, sizeof(*list), dentry_cmp);
    for (int i = 0; i < n && ent_n < ENT_MAX; i++) {
        struct ent *e = &ents[ent_n++];
        if (rel[0])
            join(e->rel, sizeof(e->rel), rel, list[i].name);
        else
            scopy(e->rel, list[i].name, sizeof(e->rel));
        e->depth = depth;
        e->is_dir = list[i].is_dir;
        int o = was_open(old, old_n, e->rel);
        e->open = o < 0 ? 0 : o;
        if (e->is_dir && depth < 8) {
            char sub[200];
            scopy(sub, e->rel, sizeof(sub));
            scan_dir(sub, depth + 1, old, old_n);
        }
    }
    free(list);
}

static void rescan(void)
{
    if (!proj[0])
        return;
    struct ent *old = ents;
    int old_n = ent_n;
    ents = malloc(sizeof(struct ent) * ENT_MAX);
    ent_n = 0;
    if (ents)
        scan_dir("", 0, old, old_n);
    free(old);
    sys_chdir(proj);
    scanned_at = now_ms();
}

/* Whether row i of ents shows: every folder above it open */
static int ent_shown(int i)
{
    int depth = ents[i].depth;
    for (int k = i - 1; k >= 0 && depth > 0; k--) {
        if (ents[k].depth < depth) {
            if (!ents[k].is_dir || !ents[k].open)
                return 0;
            depth = ents[k].depth;
        }
    }
    return 1;
}

static int ent_find(const char *rel)
{
    for (int i = 0; i < ent_n; i++)
        if (!strcmp(ents[i].rel, rel))
            return i;
    return -1;
}

/* Every folder on the way to rel opened, so that it shows */
static void reveal(const char *rel)
{
    char part[200];
    for (int i = 0; rel[i]; i++) {
        if (rel[i] != '/')
            continue;
        scopy(part, rel, (size_t)i + 1);
        int k = ent_find(part);
        if (k >= 0)
            ents[k].open = 1;
    }
}

/* --- the project's own file, and the projects opened lately --------------- */

static char recent[RECENT_MAX][PATH_MAX_];
static int recent_n;

static void recent_load(void)
{
    char path[PATH_MAX_];
    join(path, sizeof(path), home, ".code-projects");
    recent_n = 0;
    char *t = slurp(path, NULL);
    if (!t)
        return;
    char *p = t;
    while (*p && recent_n < RECENT_MAX) {
        char *e = strchr(p, '\n');
        if (e)
            *e = 0;
        if (*p == '/')
            scopy(recent[recent_n++], p, PATH_MAX_);
        if (!e)
            break;
        p = e + 1;
    }
    free(t);
}

static void recent_save(void)
{
    char path[PATH_MAX_];
    join(path, sizeof(path), home, ".code-projects");
    char *t = malloc((size_t)RECENT_MAX * (PATH_MAX_ + 1) + 1);
    if (!t)
        return;
    int k = 0;
    for (int i = 0; i < recent_n; i++) {
        int n = (int)strlen(recent[i]);
        memcpy(t + k, recent[i], (size_t)n);
        k += n;
        t[k++] = '\n';
    }
    spill(path, t, k);
    free(t);
}

static void recent_front(const char *p)
{
    int at = -1;
    for (int i = 0; i < recent_n; i++)
        if (!strcmp(recent[i], p))
            at = i;
    if (at < 0) {
        at = recent_n < RECENT_MAX ? recent_n++ : RECENT_MAX - 1;
    }
    for (int i = at; i > 0; i--)
        memcpy(recent[i], recent[i - 1], PATH_MAX_);
    scopy(recent[0], p, PATH_MAX_);
    recent_save();
}

/* .code/project: "name=", "run=", and the files that were open */
static void project_save(void)
{
    if (!proj[0])
        return;
    sys_mkdir(".code");
    char *t = malloc(512 + (size_t)DOC_MAX * 300);
    if (!t)
        return;
    int k = sprintf(t, "name=%s\nrun=%s\n", proj_name, run_cmd);
    for (int i = 0; i < doc_n; i++)
        k += sprintf(t + k, "open=%s\n", docs[i]->rel);
    if (cur >= 0)
        k += sprintf(t + k, "active=%s\n", docs[cur]->rel);
    spill(".code/project", t, k);
    free(t);
}

static void say_name(const char *what, const char *name)
{
    char b[300];
    snprintf(b, sizeof(b), "%s %s", what, name);
    say(b);
}

/* --- open files ------------------------------------------------------------ */

static void doc_save(struct doc *d)
{
    int n;
    char *t = doc_text(d, &n);
    if (!t)
        return;
    /* a file ends with a newline, as tools expect */
    char *g = realloc(t, (size_t)n + 2);
    if (g) {
        t = g;
        if (!n || t[n - 1] != '\n')
            t[n++] = '\n';
    }
    if (spill(d->rel, t, n) == 0) {
        d->dirty = 0;
        say_name("Saved", base_name(d->rel));
    } else {
        say_name("Could not save", base_name(d->rel));
    }
    free(t);
}

static void save_all(void)
{
    for (int i = 0; i < doc_n; i++)
        if (docs[i]->dirty)
            doc_save(docs[i]);
}

static void show_caret(void);

static int doc_open(const char *rel)
{
    for (int i = 0; i < doc_n; i++)
        if (!strcmp(docs[i]->rel, rel)) {
            cur = i;
            focus = FOC_EDITOR;
            scopy(tree_sel, rel, sizeof(tree_sel));
            return i;
        }
    int len = 0;
    char *t = slurp(rel, &len);
    if (!t)
        return -1;
    struct doc *d = doc_new(rel);
    if (!d) {
        free(t);
        return -1;
    }
    /* a trailing newline ends the last line rather than starting one */
    if (len && t[len - 1] == '\n')
        len--;
    doc_set_text(d, t, len);
    free(t);
    if (doc_n == DOC_MAX) {
        if (docs[0]->dirty)
            doc_save(docs[0]);
        doc_free(docs[0]);
        memmove(docs, docs + 1, sizeof(docs[0]) * (DOC_MAX - 1));
        doc_n--;
    }
    docs[doc_n] = d;
    cur = doc_n++;
    focus = FOC_EDITOR;
    scopy(tree_sel, rel, sizeof(tree_sel));
    project_save();
    return cur;
}

static void doc_close(int i)
{
    if (i < 0 || i >= doc_n)
        return;
    if (docs[i]->dirty)
        doc_save(docs[i]);
    doc_free(docs[i]);
    memmove(docs + i, docs + i + 1, sizeof(docs[0]) * (size_t)(doc_n - i - 1));
    doc_n--;
    if (cur >= doc_n)
        cur = doc_n - 1;
    else if (cur > i)
        cur--;
    project_save();
}

static void close_all(void)
{
    while (doc_n)
        doc_close(doc_n - 1);
    cur = -1;
}

/* --- templates and opening projects ---------------------------------------- */

struct tpl {
    const char *name, *about, *run;
    const char *files[3][2];
};

static const struct tpl templates[] = {
    { "Empty Project", "A folder with a README, for anything.", "",
      { { "README.md", "# %s\n\nA ThreadOS project.\n" } } },
    { "JavaScript", "A program for QuickJS, run with 'js main.js'.",
      "js main.js",
      { { "main.js",
          "// %s\n\nfunction greet(name) {\n"
          "    return \"Hello, \" + name + \"!\";\n}\n\n"
          "console.log(greet(\"ThreadOS\"));\n" },
        { "README.md",
          "# %s\n\nRun it with the green triangle, or Ctrl+R.\n" } } },
    { "C", "A C program: main.c and a header to grow from.", "",
      { { "src/main.c",
          "#include <stdio.h>\n#include \"greet.h\"\n\nint main(void)\n{\n"
          "    printf(\"%%s\\n\", greet());\n    return 0;\n}\n" },
        { "src/greet.h",
          "#ifndef GREET_H\n#define GREET_H\n\n"
          "static inline const char *greet(void)\n{\n"
          "    return \"Hello from %s\";\n}\n\n#endif\n" },
        { "README.md", "# %s\n\nA C project for ThreadOS.\n" } } },
    { "C++", "A C++ program: main.cpp and a header to grow from.", "",
      { { "src/main.cpp",
          "#include <iostream>\n#include \"greet.hpp\"\n\nint main()\n{\n"
          "    std::cout << greet(\"%s\") << std::endl;\n    return 0;\n}\n" },
        { "src/greet.hpp",
          "#pragma once\n\n#include <string>\n\n"
          "inline std::string greet(const std::string &who)\n{\n"
          "    return \"Hello from \" + who;\n}\n" },
        { "README.md", "# %s\n\nA C++ project for ThreadOS.\n" } } },
};
#define TPL_N 4

static void left_reset(void);

static int project_open(const char *path)
{
    if (sys_chdir(path) < 0) {
        say("That folder is not there");
        if (proj[0])
            sys_chdir(proj);
        return -1;
    }
    if (proj[0]) {
        /* the one open now is put away from its own folder */
        sys_chdir(proj);
        save_all();
        project_save();
        close_all();
        sys_chdir(path);
    }
    scopy(proj, path, sizeof(proj));
    sys_getcwd(proj, sizeof(proj));
    scopy(proj_name, base_name(proj), sizeof(proj_name));
    run_cmd[0] = 0;
    free(ents);
    ents = NULL;
    ent_n = 0;
    tree_top = 0;
    tree_sel[0] = 0;
    rescan();
    char active[256] = "";
    char *t = slurp(".code/project", NULL);
    if (t) {
        char *p = t;
        while (*p) {
            char *e = strchr(p, '\n');
            if (e)
                *e = 0;
            if (!strncmp(p, "name=", 5) && p[5])
                scopy(proj_name, p + 5, sizeof(proj_name));
            else if (!strncmp(p, "run=", 4))
                scopy(run_cmd, p + 4, sizeof(run_cmd));
            else if (!strncmp(p, "open=", 5))
                doc_open(p + 5);
            else if (!strncmp(p, "active=", 7))
                scopy(active, p + 7, sizeof(active));
            if (!e)
                break;
            p = e + 1;
        }
        free(t);
    }
    for (int i = 0; i < doc_n; i++)
        if (!strcmp(docs[i]->rel, active))
            cur = i;
    if (cur >= 0) {
        scopy(tree_sel, docs[cur]->rel, sizeof(tree_sel));
        reveal(docs[cur]->rel);
    }
    screen = SCR_IDE;
    popup = POP_NONE;
    focus = FOC_EDITOR;
    left_reset();
    recent_front(proj);
    project_save();
    return 0;
}

/* The folder made, the template's files written, the project opened */
static int project_create(const char *name, int tpl)
{
    char dir[PATH_MAX_], path[PATH_MAX_];
    if (!name[0] || strchr(name, '/')) {
        say("A project needs a name, without a slash");
        return -1;
    }
    join(dir, sizeof(dir), home, "Projects");
    sys_mkdir(dir);
    join(path, sizeof(path), dir, name);
    if (sys_chdir(path) == 0) {
        sys_chdir(proj[0] ? proj : home);
        say("There is a project of that name already");
        return -1;
    }
    if (sys_mkdir(path) < 0 || sys_chdir(path) < 0) {
        sys_chdir(proj[0] ? proj : home);
        say("Could not make the project's folder");
        return -1;
    }
    const struct tpl *t = &templates[tpl];
    for (int i = 0; i < 3 && t->files[i][0]; i++) {
        const char *rel = t->files[i][0];
        const char *slash = strrchr(rel, '/');
        if (slash) {
            char sub[64];
            scopy(sub, rel, (size_t)(slash - rel) + 1);
            sys_mkdir(sub);
        }
        char body[1024];
        int n = snprintf(body, sizeof(body), t->files[i][1], name);
        spill(rel, body, n);
    }
    sys_mkdir(".code");
    char meta[256];
    int n = snprintf(meta, sizeof(meta), "name=%s\nrun=%s\nopen=%s\nactive=%s\n",
                     name, t->run, t->files[0][0], t->files[0][0]);
    spill(".code/project", meta, n);
    return project_open(path);
}

static void project_close(void)
{
    save_all();
    project_save();
    close_all();
    proj[0] = 0;
    free(ents);
    ents = NULL;
    ent_n = 0;
    screen = SCR_WELCOME;
    popup = POP_NONE;
    sys_chdir(home);
}

/* --- Run and Terminal: programs and what they print ------------------------ */

/* What a tool window under the editor shows: lines, the newest last.
   The programs write into files in .code (there are no terminals to
   give them), read back as they grow. */
struct out {
    char line[OUT_MAX][OUT_COLS + 1];
    unsigned char kind[OUT_MAX];      /* 0 output, 1 errors, 2 Code's own */
    int n, len;                       /* lines; the last one's length */
    int scroll;                       /* lines up from the bottom */
};

struct proc {
    int pid, in_fd;
    uint32_t out_seen, err_seen;
    const char *out_path, *err_path;
    struct out o;
};

static struct proc run = { .pid = -1, .in_fd = -1, .out_path = ".code/run.out",
                           .err_path = ".code/run.err" };
static struct proc term = { .pid = -1, .in_fd = -1, .out_path = ".code/term.out",
                            .err_path = ".code/term.err" };
static char term_in[160];
static int term_len;

static void out_newline(struct out *o, int kind)
{
    if (o->n == OUT_MAX) {
        memmove(o->line, o->line[1], sizeof(o->line[0]) * (OUT_MAX - 1));
        memmove(o->kind, o->kind + 1, OUT_MAX - 1);
        o->n--;
    }
    o->line[o->n][0] = 0;
    o->kind[o->n] = (unsigned char)kind;
    o->n++;
    o->len = 0;
}

static void out_put(struct out *o, const char *s, int n, int kind)
{
    if (!o->n)
        out_newline(o, kind);
    for (int i = 0; i < n; i++) {
        char c = s[i];
        if (c == '\n') {
            out_newline(o, kind);
            continue;
        }
        if (c == '\r')
            continue;
        if (c == '\t')
            c = ' ';
        if (o->len < OUT_COLS) {
            o->line[o->n - 1][o->len++] = c;
            o->line[o->n - 1][o->len] = 0;
            if (kind)
                o->kind[o->n - 1] = (unsigned char)kind;
        }
    }
}

static void out_say(struct out *o, const char *s, int kind)
{
    if (o->n && o->len)
        out_newline(o, kind);
    out_put(o, s, (int)strlen(s), kind);
    out_newline(o, 0);
}

static void out_clear(struct out *o)
{
    o->n = 0;
    o->len = 0;
    o->scroll = 0;
}

static int proc_start(struct proc *p, const char *cmd)
{
    sys_mkdir(".code");
    int fd = -1;
    int pid = uterm_start_one(cmd, p->out_path, p->err_path, &fd);
    if (pid < 0)
        return -1;
    p->pid = pid;
    p->in_fd = fd;
    p->out_seen = p->err_seen = 0;
    return 0;
}

static int proc_read(struct proc *p)
{
    char buf[512];
    int got = 0;
    for (int k = 0; k < 8; k++) {
        int32_t n = uterm_read_new(p->out_path, &p->out_seen, buf, sizeof(buf));
        if (n <= 0)
            break;
        out_put(&p->o, buf, n, 0);
        got = 1;
    }
    for (int k = 0; k < 8; k++) {
        int32_t n = uterm_read_new(p->err_path, &p->err_seen, buf, sizeof(buf));
        if (n <= 0)
            break;
        out_put(&p->o, buf, n, 1);
        got = 1;
    }
    return got;
}

/* --- Problems: places the output points at --------------------------------- */

struct problem {
    char rel[200];
    int line;
    char msg[160];
};

static struct problem probs[PROB_MAX];
static int prob_n;

/* A project file by the name an error gives it - its path, or only its
   name when that is all a stack trace says */
static int project_file(const char *name, int n, char *rel_out)
{
    char want[200];
    if (n <= 0 || n >= (int)sizeof(want))
        return 0;
    memcpy(want, name, (size_t)n);
    want[n] = 0;
    if (!strncmp(want, "./", 2))
        memmove(want, want + 2, strlen(want + 2) + 1);
    for (int i = 0; i < ent_n; i++)
        if (!ents[i].is_dir && !strcmp(ents[i].rel, want)) {
            scopy(rel_out, ents[i].rel, 200);
            return 1;
        }
    for (int i = 0; i < ent_n; i++)
        if (!ents[i].is_dir && !strcmp(base_name(ents[i].rel), base_name(want))) {
            scopy(rel_out, ents[i].rel, 200);
            return 1;
        }
    return 0;
}

static void trim_copy(char *d, const char *s, size_t cap)
{
    while (*s == ' ' || *s == ':')
        s++;
    scopy(d, s, cap);
    size_t n = strlen(d);
    while (n && (d[n - 1] == ' ' || d[n - 1] == ')'))
        d[--n] = 0;
}

/* After a run: every "file:line" in what it printed that names a project
   file. A compiler says "main.c:12:5: error: ..." - the words after the
   place are the message; a stack trace says "at f (main.js:3)" under the
   error, so the message is the line above that is not one of those */
static void find_problems(void)
{
    prob_n = 0;
    const char *last_msg = "";
    for (int i = 0; i < run.o.n && prob_n < PROB_MAX; i++) {
        const char *s = run.o.line[i];
        if (run.o.kind[i] == 2)
            continue;
        const char *t = s;
        while (*t == ' ')
            t++;
        int is_trace = !strncmp(t, "at ", 3);
        int found = 0;
        for (const char *p = s; *p && !found; p++) {
            if (*p != ':' || p[1] < '0' || p[1] > '9')
                continue;
            const char *b = p;
            while (b > s && b[-1] != ' ' && b[-1] != '(' && b[-1] != '\'' &&
                   b[-1] != '"')
                b--;
            char rel[200];
            if (!project_file(b, (int)(p - b), rel))
                continue;
            struct problem *pr = &probs[prob_n++];
            scopy(pr->rel, rel, sizeof(pr->rel));
            pr->line = atoi(p + 1);
            const char *after = p + 1;
            while (*after >= '0' && *after <= '9')
                after++;
            if (*after == ':' && after[1] >= '0' && after[1] <= '9') {
                after++;
                while (*after >= '0' && *after <= '9')
                    after++;
            }
            if (is_trace || !after[0] || !strcmp(after, ")"))
                trim_copy(pr->msg, last_msg, sizeof(pr->msg));
            else
                trim_copy(pr->msg, after, sizeof(pr->msg));
            found = 1;
        }
        if (!is_trace && !found && *t)
            last_msg = s;
    }
}

static int probs_in(const char *rel, int line)
{
    for (int i = 0; i < prob_n; i++)
        if (probs[i].line == line && !strcmp(probs[i].rel, rel))
            return 1;
    return 0;
}

static void open_bottom(int which);
static void popup_open(int which);

static void run_finished(int code)
{
    char b[80];
    proc_read(&run);
    snprintf(b, sizeof(b), "Process finished with exit code %d", code);
    out_say(&run.o, b, 2);
    printf("[code] finished, exit code %d\n", code);
    fflush(stdout);
    if (run.in_fd >= 0)
        sys_close(run.in_fd);
    run.in_fd = -1;
    run.pid = -1;
    find_problems();
    if (prob_n) {
        snprintf(b, sizeof(b), "%d problem%s found", prob_n,
                 prob_n == 1 ? "" : "s");
        say(b);
    }
}

/* A run configuration may be steps joined by &&, the way a shell joins
   them: build, then run what was built - each only if the one before
   it ended well */
#define STEP_MAX 4
static char steps[STEP_MAX][160];
static int step_n, step_i;

static void split_steps(void)
{
    step_n = 0;
    const char *p = run_cmd;
    while (*p && step_n < STEP_MAX) {
        const char *e = strstr(p, "&&");
        int n = e ? (int)(e - p) : (int)strlen(p);
        while (n && p[n - 1] == ' ')
            n--;
        while (n && *p == ' ') {
            p++;
            n--;
        }
        if (n) {
            scopy(steps[step_n], p, (size_t)n + 1);
            step_n++;
        }
        if (!e)
            break;
        p = e + 2;
    }
}

static int step_start(void)
{
    out_say(&run.o, steps[step_i], 2);
    /* and on the serial port, where a test watches a build go by */
    printf("[code] step %d of %d: %s\n", step_i + 1, step_n, steps[step_i]);
    fflush(stdout);
    if (proc_start(&run, steps[step_i]) < 0) {
        out_say(&run.o, "Could not start it", 1);
        run.pid = -1;
        return -1;
    }
    return 0;
}

static void run_start(void)
{
    if (run.pid >= 0) {
        say("It is running already");
        return;
    }
    split_steps();
    if (!step_n) {
        say("Nothing to run: set the command in the run configuration");
        popup_open(POP_RUNCFG);
        return;
    }
    save_all();
    out_clear(&run.o);
    prob_n = 0;
    step_i = 0;
    step_start();
    open_bottom(BOT_RUN);
}

static void run_stop(void)
{
    if (run.pid < 0)
        return;
    sys_kill(run.pid, SIGKILL);
}

static void term_start(void)
{
    out_clear(&term.o);
    if (proc_start(&term, "ush64") < 0) {
        out_say(&term.o, "Could not start the shell", 1);
        term.pid = -1;
    }
}

static void poll_procs(int *redraw)
{
    int code = 0;
    if (run.pid >= 0) {
        *redraw |= proc_read(&run);
        int r = sys_trywait(run.pid, &code);
        if (r != -3) {
            if (r == 0 && code == 0 && step_i + 1 < step_n) {
                proc_read(&run);
                if (run.in_fd >= 0)
                    sys_close(run.in_fd);
                run.in_fd = -1;
                run.pid = -1;
                step_i++;
                if (step_start() < 0)
                    run_finished(-1);
            } else {
                run_finished(r == 0 ? code : -1);
            }
            *redraw = 1;
            rescan();
        }
    }
    if (term.pid >= 0) {
        *redraw |= proc_read(&term);
        int r = sys_trywait(term.pid, &code);
        if (r != -3) {
            proc_read(&term);
            char b[64];
            snprintf(b, sizeof(b), "The shell has ended (code %d)", code);
            out_say(&term.o, b, 2);
            if (term.in_fd >= 0)
                sys_close(term.in_fd);
            term.in_fd = -1;
            term.pid = -1;
            *redraw = 1;
        }
    }
}

static void term_send(void)
{
    if (term.pid < 0)
        term_start();
    out_put(&term.o, term_in, term_len, 2);
    out_put(&term.o, "\n", 1, 0);
    term_in[term_len++] = '\n';
    uterm_write_input(term.in_fd, term_in, (uint32_t)term_len);
    term_len = 0;
    term.o.scroll = 0;
}

/* --- Structure: the file's functions and classes --------------------------- */

struct sym {
    char name[64];
    int line, kind;                   /* 0 function, 1 type, 2 heading */
};

static struct sym syms[SYM_MAX];
static int sym_n;

static int starts(const char *s, const char *w)
{
    size_t n = strlen(w);
    return !strncmp(s, w, n) && !is_id(s[n]);
}

static void add_sym(const char *s, int n, int line, int kind)
{
    if (sym_n == SYM_MAX || n <= 0)
        return;
    if (n > 63)
        n = 63;
    memcpy(syms[sym_n].name, s, (size_t)n);
    syms[sym_n].name[n] = 0;
    syms[sym_n].line = line;
    syms[sym_n].kind = kind;
    sym_n++;
}

static int id_len(const char *s)
{
    int n = 0;
    while (is_id(s[n]))
        n++;
    return n;
}

static void find_symbols(struct doc *d)
{
    sym_n = 0;
    if (!d)
        return;
    states_upto(d, d->n - 1);
    for (int y = 0; y < d->n; y++) {
        const char *s = d->ln[y];
        int ind = indent_of(d, y);
        const char *t = s + ind;
        if (d->st[y] != S_NONE)
            continue;
        if (d->lang == L_RUST) {
            if (starts(t, "pub"))
                t += 4;
            if (starts(t, "async"))
                t += 6;
            if (starts(t, "fn")) {
                t += 3;
                add_sym(t, id_len(t), y, 0);
            } else if (starts(t, "struct") || starts(t, "enum") ||
                       starts(t, "trait") || starts(t, "impl")) {
                t += strcspn(t, " ") + 1;
                add_sym(t, id_len(t), y, 1);
            }
            continue;
        }
        if (d->lang == L_JAVA || (d->lang == L_CPP && ind > 0)) {
            const char *k = t;
            while (starts(k, "public") || starts(k, "private") ||
                   starts(k, "protected") || starts(k, "static") ||
                   starts(k, "final") || starts(k, "abstract") ||
                   starts(k, "virtual"))
                k += strcspn(k, " ") + 1;
            if (starts(k, "class") || starts(k, "interface") ||
                starts(k, "enum") || starts(k, "record") ||
                starts(k, "struct")) {
                k += strcspn(k, " ") + 1;
                add_sym(k, id_len(k), y, 1);
                continue;
            }
            const char *par = strchr(t, '(');
            if (!par || strchr(t, ';') || strchr(t, '=') || starts(t, "if") ||
                starts(t, "while") || starts(t, "for") || starts(t, "switch") ||
                starts(t, "return") || starts(t, "catch") || starts(t, "new") ||
                starts(t, "else"))
                continue;
            const char *e = par;
            while (e > t && e[-1] == ' ')
                e--;
            const char *b = e;
            while (b > t && is_id(b[-1]))
                b--;
            if (b < e && b > t)
                add_sym(b, (int)(e - b), y, 0);
            continue;
        }
        if ((d->lang == L_C || d->lang == L_CPP) && ind == 0 && is_id(s[0])) {
            if (d->lang == L_CPP && !strchr(s, ';') &&
                (starts(s, "class") || starts(s, "namespace"))) {
                const char *n = s + strcspn(s, " ") + 1;
                add_sym(n, id_len(n), y, 1);
                continue;
            }
            if ((starts(s, "struct") || starts(s, "union") ||
                 starts(s, "enum")) && strchr(s, '{') && !strchr(s, '(')) {
                const char *n = s + strcspn(s, " ") + 1;
                add_sym(n, id_len(n), y, 1);
                continue;
            }
            const char *par = strchr(s, '(');
            if (!par || strchr(s, ';') || strchr(s, '=') ||
                starts(s, "if") || starts(s, "while") || starts(s, "for") ||
                starts(s, "return") || starts(s, "switch") || starts(s, "else"))
                continue;
            const char *e = par;
            while (e > s && e[-1] == ' ')
                e--;
            const char *b = e;
            while (b > s && is_id(b[-1]))
                b--;
            if (b < e && b > s)
                add_sym(b, (int)(e - b), y, 0);
        } else if (d->lang == L_JS) {
            if (starts(t, "export"))
                t += 7;
            if (starts(t, "async"))
                t += 6;
            if (starts(t, "function")) {
                t += 8;
                while (*t == ' ' || *t == '*')
                    t++;
                add_sym(t, id_len(t), y, 0);
            } else if (starts(t, "class")) {
                t += 6;
                add_sym(t, id_len(t), y, 1);
            } else if (ind == 0 && (starts(t, "const") || starts(t, "let") ||
                                    starts(t, "var"))) {
                const char *n = t + strcspn(t, " ") + 1;
                int len = id_len(n);
                const char *r = n + len;
                if (strstr(r, "=>") || strstr(r, "function"))
                    add_sym(n, len, y, 0);
            }
        } else if (d->lang == L_PY) {
            if (starts(t, "def")) {
                t += 4;
                add_sym(t, id_len(t), y, 0);
            } else if (starts(t, "class")) {
                t += 6;
                add_sym(t, id_len(t), y, 1);
            }
        } else if (d->lang == L_MD && s[0] == '#') {
            while (*t == '#' || *t == ' ')
                t++;
            add_sym(t, (int)strlen(t), y, 2);
        }
    }
}

/* --- Go to File ---------------------------------------------------------- */

static char ask[160];                 /* what is typed into a popup */
static int ask_len;
static int pick;                      /* the row chosen in a popup's list */
static int matches[POP_ROWS * 4];
static int match_n;

/* How well a file answers what was typed: its name starting with it
   best, then containing it, then the letters in order; 0 not at all */
static int score(const char *rel, const char *q)
{
    if (!q[0])
        return 1;
    const char *b = base_name(rel);
    size_t n = strlen(q);
    int pre = 1;
    for (size_t i = 0; i < n; i++)
        if (lower((unsigned char)b[i]) != lower((unsigned char)q[i]) || !b[i]) {
            pre = 0;
            break;
        }
    if (pre)
        return 4;
    for (const char *s = b; *s; s++) {
        size_t i = 0;
        while (i < n && s[i] &&
               lower((unsigned char)s[i]) == lower((unsigned char)q[i]))
            i++;
        if (i == n)
            return 3;
    }
    const char *s = rel;
    for (size_t i = 0; i < n; i++) {
        while (*s && lower((unsigned char)*s) != lower((unsigned char)q[i]))
            s++;
        if (!*s)
            return 0;
        s++;
    }
    return 2;
}

static void find_matches(void)
{
    int cap = (int)(sizeof(matches) / sizeof(matches[0]));
    match_n = 0;
    for (int best = 4; best >= 1; best--)
        for (int i = 0; i < ent_n && match_n < cap; i++)
            if (!ents[i].is_dir && score(ents[i].rel, ask) == best)
                matches[match_n++] = i;
    if (pick >= match_n)
        pick = match_n ? match_n - 1 : 0;
}

/* --- the left and bottom tool windows -------------------------------------- */

static void left_reset(void)
{
    if (left_tool == LEFT_STRUCTURE)
        find_symbols(cur >= 0 ? docs[cur] : NULL);
}

static void open_bottom(int which)
{
    bottom_tool = which;
    if (which == BOT_TERMINAL) {
        focus = FOC_TERMINAL;
        if (term.pid < 0)
            term_start();
    } else if (focus == FOC_TERMINAL) {
        focus = FOC_EDITOR;
    }
}

static void toggle_bottom(int which)
{
    if (bottom_tool == which) {
        bottom_tool = BOT_NONE;
        if (focus == FOC_TERMINAL)
            focus = FOC_EDITOR;
    } else {
        open_bottom(which);
    }
}

static void toggle_left(int which)
{
    left_tool = left_tool == which ? LEFT_NONE : which;
    left_reset();
}

/* --- where things are ------------------------------------------------------ */

#define DLG_BTN_W 100

static int find_open;
static char find_text[64];
static int find_len;
static int tpl_pick = 1;              /* New Project's template */
static int tab_x[DOC_MAX + 1];        /* where each tab begins, as drawn */
static int dragging;

static int ed_x(void)
{
    return STRIPE_W + (left_tool != LEFT_NONE ? SIDE_W + 1 : 0);
}

static int bot_y(void)
{
    return ch - STATUS_H - (bottom_tool != BOT_NONE ? BOTTOM_H : 0);
}

static int code_y(void)
{
    return BAR_H + TAB_H + (find_open ? FIND_H : 0);
}

static int code_x(void)
{
    return ed_x() + GUTTER_W + CODE_PAD;
}

static int code_rows(void)
{
    int r = (bot_y() - code_y()) / ROW_H;
    return r > 1 ? r : 1;
}

static int code_cols(void)
{
    int c = (cw - code_x() - 8) / CELL;
    return c > 1 ? c : 1;
}

static int pop_x(void)
{
    return (cw - POP_W) / 2;
}

static void show_caret(void)
{
    if (cur < 0)
        return;
    struct doc *d = docs[cur];
    int rows = code_rows(), cols = code_cols();
    if (d->cy < d->top)
        d->top = d->cy;
    if (d->cy >= d->top + rows)
        d->top = d->cy - rows + 1;
    int v = vcol(d, d->cy, d->cx);
    if (v < d->hoff)
        d->hoff = v;
    if (v >= d->hoff + cols)
        d->hoff = v - cols + 1;
}

/* --- drawing helpers ------------------------------------------------------- */

static uint32_t colour_of(int cls)
{
    switch (cls) {
    case C_KW: return P.kw;
    case C_STR: return P.str;
    case C_COM: return P.com;
    case C_NUM: return P.num;
    case C_PRE: return P.pre;
    case C_FN: return P.fn;
    default: return P.text;
    }
}

static uint32_t hue_of(const char *s)
{
    static const uint32_t hues[] = { 0x3574F0, 0x5FB865, 0xE08855, 0xB073E0,
                                     0xD9534F, 0x2AACB8, 0xC9A33A };
    unsigned h = 0;
    for (; *s; s++)
        h = h * 31 + (unsigned char)*s;
    return hues[h % 7];
}

/* A project's badge: its initials on a colour of its own */
static void badge(int x, int y, int d, const char *name)
{
    char ini[3] = { 0, 0, 0 };
    ini[0] = (char)(name[0] >= 'a' && name[0] <= 'z' ? name[0] - 32 : name[0]);
    const char *sp = strpbrk(name, " -_");
    if (sp && sp[1])
        ini[1] = (char)(sp[1] >= 'a' && sp[1] <= 'z' ? sp[1] - 32 : sp[1]);
    else if (name[0] && name[1])
        ini[1] = (char)(name[1] >= 'a' && name[1] <= 'z' ? name[1] - 32
                                                          : name[1]);
    ud_round(win, x, y, d, d, d / 4, hue_of(name));
    ud_text_mid(win, x, y, d, d, ini, 0xFFFFFF, UD_BOLD, d * 11 / 24);
}

static void folder_icon(int x, int y, uint32_t c)
{
    ud_round(win, x, y + 2, 7, 4, 1, c);
    ud_round(win, x, y + 4, 15, 10, 2, c);
}

/* A file's icon: its kind as a letter on a colour, the way the IDEs
   tell a C file from a header at a glance */
static void file_icon(int x, int y, const char *rel)
{
    int lang = lang_of(rel);
    const char *e = ext_of(rel);
    const char *t = "";
    uint32_t bg = P.dim, fg = 0xFFFFFF;
    switch (lang) {
    case L_C:
        t = !strcmp(e, "h") || !strcmp(e, "hpp") ? "H" : "C";
        bg = t[0] == 'H' ? 0xA571E6 : 0x548AF7;
        break;
    case L_CPP:
        t = !strcmp(e, "hpp") || !strcmp(e, "hh") ? "H+" : "C+";
        bg = t[0] == 'H' ? 0xA571E6 : 0x3D6FD6;
        break;
    case L_RUST: t = "Rs"; bg = 0xDEA584; fg = 0x1E1F22; break;
    case L_JAVA: t = "J"; bg = 0xE76F00; break;
    case L_JS: t = "JS"; bg = 0xF2C55C; fg = 0x1E1F22; break;
    case L_PY: t = "Py"; bg = 0x3D7EBF; break;
    case L_SH: t = "$"; bg = 0x5FB865; break;
    case L_MD: t = "M"; bg = 0x6F737A; break;
    case L_JSON: t = "{}"; bg = 0xE08855; break;
    case L_MAKE: t = "M"; bg = 0xC75450; break;
    default:
        ud_round(win, x + 2, y, 12, 15, 2, P.dim);
        ud_fill(win, x + 4, y + 4, 8, 1, P.panel);
        ud_fill(win, x + 4, y + 7, 8, 1, P.panel);
        ud_fill(win, x + 4, y + 10, 6, 1, P.panel);
        return;
    }
    ud_round(win, x, y, 16, 16, 4, bg);
    ud_text_mid(win, x, y, 16, 16, t, fg, UD_BOLD, strlen(t) > 1 ? 8 : 10);
}

static void chevron(int x, int y, int down, uint32_t c)
{
    if (down) {
        ud_line(win, x, y, x + 4, y + 4, 22, c);
        ud_line(win, x + 4, y + 4, x + 8, y, 22, c);
    } else {
        ud_line(win, x, y, x + 4, y + 4, 22, c);
        ud_line(win, x, y + 8, x + 4, y + 4, 22, c);
    }
}

/* A triangle pointing right, filled a column at a time */
static void play_icon(int x, int y, int h, uint32_t c)
{
    int w = h * 7 / 8;
    for (int i = 0; i < w; i++) {
        int half = (h / 2) * (w - i) / w;
        ud_fill(win, x + i, y + h / 2 - half, 1, half * 2 + 1, c);
    }
}

static void field(int x, int y, int w, const char *text, int caret, int focused)
{
    ud_round(win, x, y, w, FIELD_H, 5, focused ? P.accent : P.border);
    ud_round(win, x + 2, y + 2, w - 4, FIELD_H - 4, 4, P.field);
    int base = ud_baseline(y, FIELD_H, UI_PX);
    ud_clip(win, x + 2, y + 2, w - 4, FIELD_H - 4);
    int e = ud_text(win, x + 10, base, text, P.ui, UD_SANS, UI_PX);
    if (caret)
        ud_fill(win, e + 1, y + 7, 1, FIELD_H - 14, P.ui);
    ud_unclip(win);
}

static void button(int x, int y, int w, int h, const char *label, int primary)
{
    if (primary) {
        ud_round(win, x, y, w, h, 6, P.accent);
        ud_text_mid_l(win, x, y, w, h, label, P.on_accent, UD_SANS, UI_PX);
    } else {
        ud_round(win, x, y, w, h, 6, P.border);
        ud_round(win, x + 1, y + 1, w - 2, h - 2, 5, P.panel);
        ud_text_mid_l(win, x, y, w, h, label, P.ui, UD_SANS, UI_PX);
    }
}

static void home_short(char *out, size_t cap, const char *path)
{
    size_t n = strlen(home);
    if (n > 1 && !strncmp(path, home, n) && (path[n] == '/' || !path[n]))
        snprintf(out, cap, "~%s", path + n);
    else
        scopy(out, path, cap);
}

/* --- the welcome screen ----------------------------------------------------- */

static void app_icon(int x, int y, int d)
{
    ud_grad(win, x, y, d, d, d / 4, 0x3574F0, 0xB073E0);
    /* < and >, in white */
    for (int side = 0; side < 2; side++) {
        int tip = side ? x + d * 26 / 36 : x + d * 10 / 36;
        int arm = side ? x + d * 21 / 36 : x + d * 15 / 36;
        ud_line(win, tip, y + d / 2, arm, y + d * 12 / 36, 28, 0xFFFFFF);
        ud_line(win, tip, y + d / 2, arm, y + d * 24 / 36, 28, 0xFFFFFF);
    }
}

static void draw_welcome(void)
{
    ud_fill(win, 0, 0, WEL_SIDE_W, ch, P.panel);
    ud_fill(win, WEL_SIDE_W, 0, 1, ch, P.border);
    ud_fill(win, WEL_SIDE_W + 1, 0, cw - WEL_SIDE_W - 1, ch, P.editor);
    app_icon(20, 20, 36);
    ud_text(win, 66, 36, "Code", P.ui, UD_BOLD, 14);
    ud_text(win, 66, 52, "2026.11", P.dim, UD_SANS, 11);
    ud_round(win, 10, 80, WEL_SIDE_W - 20, 30, 6, P.tree_sel);
    ud_text_l(win, 22, ud_baseline(80, 30, UI_PX), "Projects", P.ui, UD_SANS,
              UI_PX);
    int by = (WEL_HEAD_H - WEL_BTN_H) / 2;
    button(cw - 20 - 2 * WEL_BTN_W - 10, by, WEL_BTN_W, WEL_BTN_H,
           "New Project", 1);
    button(cw - 20 - WEL_BTN_W, by, WEL_BTN_W, WEL_BTN_H, "Open", 0);
    if (!recent_n) {
        int mid = WEL_SIDE_W + (cw - WEL_SIDE_W) / 2;
        const char *a = "Welcome to Code";
        int w = ud_width_l(a, UD_BOLD, 18);
        ud_text_l(win, mid - w / 2, ch / 2 - 10, a, P.ui, UD_BOLD, 18);
        const char *b = "Create a new project to start from scratch.";
        w = ud_width_l(b, UD_SANS, UI_PX);
        ud_text_l(win, mid - w / 2, ch / 2 + 16, b, P.dim, UD_SANS, UI_PX);
        return;
    }
    for (int i = 0; i < recent_n; i++) {
        int y = WEL_HEAD_H + i * WEL_ROW_H;
        if (y + WEL_ROW_H > ch)
            break;
        int x = WEL_SIDE_W + 16;
        badge(x, y + 10, 32, base_name(recent[i]));
        ud_text(win, x + 46, ud_baseline(y + 8, 20, UI_PX), base_name(recent[i]),
                P.ui, UD_BOLD, UI_PX);
        char shortp[PATH_MAX_];
        home_short(shortp, sizeof(shortp), recent[i]);
        ud_text(win, x + 46, y + 40, shortp, P.dim, UD_SANS, 11);
    }
}

static void draw_new(void)
{
    ud_fill(win, 0, 0, WEL_SIDE_W, ch, P.panel);
    ud_fill(win, WEL_SIDE_W, 0, 1, ch, P.border);
    ud_fill(win, WEL_SIDE_W + 1, 0, cw - WEL_SIDE_W - 1, ch, P.editor);
    ud_text_l(win, 20, 38, "New Project", P.ui, UD_BOLD, 14);
    for (int i = 0; i < TPL_N; i++) {
        int y = TPL_TOP + i * TPL_ROW_H;
        if (i == tpl_pick)
            ud_round(win, 10, y, WEL_SIDE_W - 20, TPL_ROW_H, 6, P.tree_sel);
        const char *icon = i == 1 ? "main.js" : i == 2 ? "main.c" :
                           i == 3 ? "main.cpp" : "README.md";
        file_icon(22, y + (TPL_ROW_H - 16) / 2, icon);
        ud_text_l(win, 48, ud_baseline(y, TPL_ROW_H, UI_PX), templates[i].name,
                  P.ui, UD_SANS, UI_PX);
    }
    int x0 = WEL_SIDE_W + 1;
    ud_text_l(win, x0 + 24, ud_baseline(FORM_Y, FIELD_H, UI_PX), "Name:", P.ui,
              UD_SANS, UI_PX);
    field(x0 + FORM_X, FORM_Y, FORM_W, ask, 1, 1);
    ud_text_l(win, x0 + 24, ud_baseline(FORM_Y + 44, FIELD_H, UI_PX),
              "Location:", P.ui, UD_SANS, UI_PX);
    char loc[PATH_MAX_];
    snprintf(loc, sizeof(loc), "~/Projects/%s", ask);
    ud_text(win, x0 + FORM_X + 2, ud_baseline(FORM_Y + 44, FIELD_H, UI_PX),
            loc, P.dim, UD_SANS, UI_PX);
    ud_text_l(win, x0 + 24, FORM_Y + 110, templates[tpl_pick].about, P.dim,
              UD_SANS, UI_PX);
    if (note[0] && now_ms() - note_at < 6000)
        ud_text_l(win, x0 + 24, FORM_Y + 140, note, P.err, UD_SANS, UI_PX);
    int by = ch - 20 - WEL_BTN_H;
    button(cw - 20 - DLG_BTN_W, by, DLG_BTN_W, WEL_BTN_H, "Create", 1);
    button(cw - 30 - 2 * DLG_BTN_W, by, DLG_BTN_W, WEL_BTN_H, "Cancel", 0);
}

/* --- the IDE --------------------------------------------------------------- */

static void draw_toolbar(void)
{
    ud_fill(win, 0, 0, cw, BAR_H, P.panel);
    ud_fill(win, 0, BAR_H - 1, cw, 1, P.border);
    badge(10, (BAR_H - 24) / 2, 24, proj_name);
    int e = ud_text(win, 42, ud_baseline(0, BAR_H, UI_PX), proj_name, P.ui,
                    UD_BOLD, UI_PX);
    chevron(e + 8, BAR_H / 2 - 2, 1, P.dim);
    /* the run configuration, Run, Stop, and Go to File */
    int cx = cw - RUN_R - 8 - CFG_W, by = (BAR_H - BTN) / 2;
    ud_round(win, cx, by, CFG_W, BTN, 6, P.hover);
    ud_clip(win, cx, by, CFG_W - 22, BTN);
    if (run_cmd[0]) {
        play_icon(cx + 10, by + 9, 10, P.run);
        ud_text(win, cx + 26, ud_baseline(by, BTN, UI_PX), run_cmd, P.ui,
                UD_SANS, UI_PX);
    } else {
        ud_text_l(win, cx + 10, ud_baseline(by, BTN, UI_PX),
                  "Add Configuration...", P.dim, UD_SANS, UI_PX);
    }
    ud_unclip(win);
    chevron(cx + CFG_W - 16, BAR_H / 2 - 2, 1, P.dim);
    play_icon(cw - RUN_R + 9, by + 7, 14, run.pid >= 0 ? P.dim : P.run);
    ud_round(win, cw - STOP_R + 8, by + 8, 12, 12, 2,
             run.pid >= 0 ? P.stop : P.hover);
    int sx = cw - SEARCH_R + 6;
    ud_ring(win, sx, by + 6, 12, 12, 6, P.ui);
    ud_line(win, sx + 10, by + 16, sx + 15, by + 21, 28, P.ui);
}

static void stripe_button(int y, int on, int kind)
{
    int x = (STRIPE_W - STRIPE_ICON) / 2;
    if (on)
        ud_round(win, x, y, STRIPE_ICON, STRIPE_ICON, 6, P.tree_sel);
    uint32_t c = on ? P.ui : P.dim;
    int mx = x + STRIPE_ICON / 2, my = y + STRIPE_ICON / 2;
    switch (kind) {
    case 0:                              /* Project: a folder */
        ud_ring(win, mx - 8, my - 6, 16, 12, 2, c);
        ud_fill(win, mx - 8, my - 7, 6, 2, c);
        break;
    case 1:                              /* Structure: an outline */
        for (int i = 0; i < 3; i++) {
            ud_fill(win, mx - 7 + (i ? 4 : 0), my - 6 + i * 5, 2, 2, c);
            ud_fill(win, mx - 3 + (i ? 4 : 0), my - 6 + i * 5,
                    10 - (i ? 4 : 0), 2, c);
        }
        break;
    case 2:                              /* Run */
        play_icon(mx - 5, my - 7, 14, on ? P.run : c);
        break;
    case 3:                              /* Terminal: >_ */
        ud_ring(win, mx - 9, my - 7, 18, 14, 3, c);
        ud_line(win, mx - 5, my - 3, mx - 2, my, 20, c);
        ud_line(win, mx - 5, my + 3, mx - 2, my, 20, c);
        ud_fill(win, mx, my + 3, 5, 1, c);
        break;
    default:                             /* Problems: ! in a circle */
        ud_ring(win, mx - 8, my - 8, 16, 16, 8, prob_n ? P.err : c);
        ud_fill(win, mx - 1, my - 5, 2, 6, prob_n ? P.err : c);
        ud_fill(win, mx - 1, my + 3, 2, 2, prob_n ? P.err : c);
        break;
    }
}

static int stripe_top_y(int i)
{
    return BAR_H + 6 + i * STRIPE_STEP;
}

static int stripe_bottom_y(int j)
{
    return ch - STATUS_H - 6 - (3 - j) * STRIPE_STEP +
           (STRIPE_STEP - STRIPE_ICON);
}

static void draw_stripe(void)
{
    ud_fill(win, 0, BAR_H, STRIPE_W, ch - BAR_H - STATUS_H, P.panel);
    ud_fill(win, STRIPE_W - 1, BAR_H, 1, ch - BAR_H - STATUS_H, P.border);
    stripe_button(stripe_top_y(0), left_tool == LEFT_PROJECT, 0);
    stripe_button(stripe_top_y(1), left_tool == LEFT_STRUCTURE, 1);
    stripe_button(stripe_bottom_y(0), bottom_tool == BOT_RUN, 2);
    stripe_button(stripe_bottom_y(1), bottom_tool == BOT_TERMINAL, 3);
    stripe_button(stripe_bottom_y(2), bottom_tool == BOT_PROBLEMS, 4);
}

static int tree_rows(void)
{
    return (ch - STATUS_H - BAR_H - HEAD_H) / TREE_ROW_H;
}

/* The rows of the tree as shown: indices into ents */
static int shown_rows(int *out, int max)
{
    int n = 0, skip = tree_top;
    for (int i = 0; i < ent_n && n < max; i++) {
        if (!ent_shown(i))
            continue;
        if (skip) {
            skip--;
            continue;
        }
        out[n++] = i;
    }
    return n;
}

static void draw_left(void)
{
    int x0 = STRIPE_W, y0 = BAR_H;
    ud_fill(win, x0, y0, SIDE_W, ch - y0 - STATUS_H, P.panel);
    ud_fill(win, x0 + SIDE_W, y0, 1, ch - y0 - STATUS_H, P.border);
    const char *title = left_tool == LEFT_PROJECT ? "Project" : "Structure";
    ud_text_l(win, x0 + 12, ud_baseline(y0, HEAD_H, UI_PX), title, P.ui,
              UD_BOLD, UI_PX);
    if (left_tool == LEFT_PROJECT) {
        /* New File: a plus */
        int px = x0 + SIDE_W - PLUS_R, py = y0 + (HEAD_H - 22) / 2;
        ud_fill(win, px + 5, py + 10, 12, 2, P.ui);
        ud_fill(win, px + 10, py + 5, 2, 12, P.ui);
        int rows[256];
        int n = shown_rows(rows, tree_rows() < 256 ? tree_rows() : 256);
        ud_clip(win, x0, y0 + HEAD_H, SIDE_W, ch - y0 - HEAD_H - STATUS_H);
        for (int r = 0; r < n; r++) {
            struct ent *e = &ents[rows[r]];
            int y = y0 + HEAD_H + r * TREE_ROW_H;
            if (!strcmp(e->rel, tree_sel))
                ud_fill(win, x0, y, SIDE_W, TREE_ROW_H, P.tree_sel);
            int x = x0 + 10 + e->depth * TREE_INDENT;
            if (e->is_dir) {
                chevron(x + (e->open ? 0 : 2), y + (e->open ? 9 : 7), e->open,
                        P.dim);
                folder_icon(x + 14, y + 3, P.dim);
            } else {
                file_icon(x + 14, y + 3, e->rel);
            }
            ud_text(win, x + 36, ud_baseline(y, TREE_ROW_H, UI_PX),
                    base_name(e->rel), P.ui, UD_SANS, UI_PX);
        }
        ud_unclip(win);
        return;
    }
    if (cur < 0) {
        ud_text_l(win, x0 + 12, y0 + HEAD_H + 20, "No file is open", P.dim,
                  UD_SANS, UI_PX);
        return;
    }
    ud_clip(win, x0, y0 + HEAD_H, SIDE_W, ch - y0 - HEAD_H - STATUS_H);
    for (int i = 0; i < sym_n && i < tree_rows(); i++) {
        int y = y0 + HEAD_H + i * TREE_ROW_H;
        uint32_t c = syms[i].kind == 0 ? 0xE08855 : syms[i].kind == 1 ?
                     0x548AF7 : P.dim;
        ud_disc(win, x0 + 12, y + 4, 14, c);
        ud_text_mid(win, x0 + 12, y + 4, 14, 14,
                    syms[i].kind == 0 ? "f" : syms[i].kind == 1 ? "c" : "#",
                    0xFFFFFF, UD_BOLD, 9);
        int e = ud_text(win, x0 + 34, ud_baseline(y, TREE_ROW_H, UI_PX),
                        syms[i].name, P.ui, UD_SANS, UI_PX);
        char ln[16];
        snprintf(ln, sizeof(ln), ":%d", syms[i].line + 1);
        ud_text(win, e + 6, ud_baseline(y, TREE_ROW_H, UI_PX), ln, P.dim,
                UD_SANS, 11);
    }
    ud_unclip(win);
}

static void draw_tabs(void)
{
    int x = ed_x(), y = BAR_H;
    ud_fill(win, x, y, cw - x, TAB_H, P.panel);
    ud_fill(win, x, y + TAB_H - 1, cw - x, 1, P.border);
    for (int i = 0; i < doc_n; i++) {
        const char *name = base_name(docs[i]->rel);
        int w = 12 + 16 + 6 + ud_width(name, UD_SANS, UI_PX) + 10 + 12 + 8;
        tab_x[i] = x;
        if (i == cur) {
            ud_fill(win, x, y, w, TAB_H - 1, P.editor);
            ud_fill(win, x, y + TAB_H - 3, w, 2, P.accent);
        }
        file_icon(x + 12, y + (TAB_H - 16) / 2, docs[i]->rel);
        ud_text(win, x + 34, ud_baseline(y, TAB_H, UI_PX), name,
                i == cur ? P.ui : P.dim, UD_SANS, UI_PX);
        int cx = x + w - 20, cy = y + TAB_H / 2;
        if (docs[i]->dirty) {
            ud_disc(win, cx + 2, cy - 4, 8, P.ui);
        } else {
            ud_line(win, cx + 2, cy - 4, cx + 9, cy + 3, 18, P.dim);
            ud_line(win, cx + 9, cy - 4, cx + 2, cy + 3, 18, P.dim);
        }
        x += w;
        if (x > cw)
            break;
    }
    tab_x[doc_n] = x;
}

/* Where text t occurs in line y, from byte 'from'; -1 if not */
static int find_in(struct doc *d, int y, int from, const char *t, int n)
{
    if (!n)
        return -1;
    for (int i = from; i + n <= d->len[y]; i++) {
        int k = 0;
        while (k < n && lower((unsigned char)d->ln[y][i + k]) ==
                        lower((unsigned char)t[k]))
            k++;
        if (k == n)
            return i;
    }
    return -1;
}

static void find_count(int *at, int *total)
{
    *at = *total = 0;
    if (cur < 0 || !find_len)
        return;
    struct doc *d = docs[cur];
    for (int y = 0; y < d->n; y++)
        for (int i = find_in(d, y, 0, find_text, find_len); i >= 0;
             i = find_in(d, y, i + 1, find_text, find_len)) {
            (*total)++;
            if (y < d->cy || (y == d->cy && i <= d->cx))
                *at = *total;
        }
}

static void draw_findbar(void)
{
    int x = ed_x(), y = BAR_H + TAB_H;
    ud_fill(win, x, y, cw - x, FIND_H, P.panel);
    ud_fill(win, x, y + FIND_H - 1, cw - x, 1, P.border);
    int fy = y + (FIND_H - FIELD_H) / 2;
    field(x + 8, fy, 260, find_text, focus == FOC_FIND, focus == FOC_FIND);
    int at, total;
    find_count(&at, &total);
    char b[48];
    if (!find_len)
        b[0] = 0;
    else if (!total)
        scopy(b, "0 results", sizeof(b));
    else
        snprintf(b, sizeof(b), "%d/%d", at, total);
    ud_text(win, x + 280, ud_baseline(y, FIND_H, UI_PX), b,
            find_len && !total ? P.err : P.dim, UD_SANS, UI_PX);
}

/* The bracket that matches the one at y, x, searched up to a few hundred
   lines away; 0 if there is none */
static int bracket_match(struct doc *d, int y, int x, int *my, int *mx)
{
    static const char open_b[] = "([{", close_b[] = ")]}";
    if (x < 0 || x >= d->len[y])
        return 0;
    char c = d->ln[y][x];
    const char *o = strchr(open_b, c), *cl = strchr(close_b, c);
    if (!c || (!o && !cl))
        return 0;
    char want = o ? close_b[o - open_b] : open_b[cl - close_b];
    int dir = o ? 1 : -1, depth = 0;
    for (int yy = y, n = 0; yy >= 0 && yy < d->n && n < 400; yy += dir, n++) {
        int i = yy == y ? x : dir > 0 ? 0 : d->len[yy] - 1;
        for (; i >= 0 && i < d->len[yy]; i += dir) {
            char k = d->ln[yy][i];
            if (k == c)
                depth++;
            else if (k == want && --depth == 0) {
                *my = yy;
                *mx = i;
                return 1;
            }
        }
    }
    return 0;
}

static void draw_line_text(struct doc *d, int y, int top, unsigned char *cls)
{
    char run_s[OUT_COLS * 4 + 8];
    int run_n = 0, run_v = 0, run_c = -1;
    int hoff = d->hoff, cols = code_cols(), x0 = code_x();
    int v = 0;
    for (int i = 0; i <= d->len[y]; i++) {
        unsigned char b = i < d->len[y] ? (unsigned char)d->ln[y][i] : 0;
        int cont = (b & 0xC0) == 0x80;
        int flush = i == d->len[y] || b == '\t' ||
                    (!cont && (cls[i] != run_c || v >= hoff + cols ||
                               run_n > (int)sizeof(run_s) - 8));
        if (flush && run_n) {
            run_s[run_n] = 0;
            ud_cells(win, x0 + (run_v - hoff) * CELL, top + TEXT_BASE, CELL,
                     run_s, colour_of(run_c), UD_MONO, TEXT_PX);
            run_n = 0;
        }
        if (i == d->len[y] || v >= hoff + cols)
            break;
        if (b == '\t') {
            v = (v / TABW + 1) * TABW;
            run_c = -1;
            continue;
        }
        if (cont) {
            if (run_n)
                run_s[run_n++] = (char)b;
            continue;
        }
        if (v >= hoff) {
            if (!run_n) {
                run_v = v;
                run_c = cls[i];
            }
            run_s[run_n++] = (char)b;
        }
        v++;
    }
}

static void draw_editor(void)
{
    int ex = ed_x(), top0 = BAR_H + TAB_H;
    ud_fill(win, ex, top0, cw - ex, bot_y() - top0, P.editor);
    if (cur < 0) {
        const char *a = "No files are open";
        int w = ud_width_l(a, UD_SANS, 14);
        int mid = ex + (cw - ex) / 2;
        ud_text_l(win, mid - w / 2, top0 + (bot_y() - top0) / 2 - 20, a,
                  P.dim, UD_SANS, 14);
        const char *b = "Go to File: Ctrl+O    Run: Ctrl+R";
        w = ud_width_l(b, UD_SANS, UI_PX);
        ud_text_l(win, mid - w / 2, top0 + (bot_y() - top0) / 2 + 6, b,
                  P.dim, UD_SANS, UI_PX);
        return;
    }
    struct doc *d = docs[cur];
    int rows = code_rows(), x0 = code_x(), y0 = code_y();
    states_upto(d, d->top + rows);
    int y0s = 0, x0s = 0, y1s = -1, x1s = 0;
    if (d->sel)
        sel_bounds(d, &y0s, &x0s, &y1s, &x1s);
    int by1 = -1, bx1 = 0, by2 = -1, bx2 = 0;
    int bx = d->cx, bmy, bmx;
    if (bracket_match(d, d->cy, bx, &bmy, &bmx) ||
        (bx > 0 && bracket_match(d, d->cy, --bx, &bmy, &bmx))) {
        by1 = d->cy; bx1 = bx; by2 = bmy; bx2 = bmx;
    }
    ud_clip(win, ex, y0, cw - ex, bot_y() - y0);
    for (int r = 0; r < rows; r++) {
        int y = d->top + r;
        if (y >= d->n)
            break;
        int top = y0 + r * ROW_H;
        if (y == d->cy && !d->sel)
            ud_fill(win, ex, top, cw - ex, ROW_H, P.cur);
        if (d->sel && y >= y0s && y <= y1s) {
            int a = y == y0s ? vcol(d, y, x0s) : 0;
            int b = y == y1s ? vcol(d, y, x1s) : vcol(d, y, d->len[y]) + 1;
            a -= d->hoff;
            b -= d->hoff;
            if (a < 0)
                a = 0;
            if (b > a)
                ud_fill(win, x0 + a * CELL, top, (b - a) * CELL, ROW_H, P.sel);
        }
        if (find_open && find_len)
            for (int i = find_in(d, y, 0, find_text, find_len); i >= 0;
                 i = find_in(d, y, i + 1, find_text, find_len)) {
                int a = vcol(d, y, i) - d->hoff;
                int b = vcol(d, y, i + find_len) - d->hoff;
                if (b > 0)
                    ud_fill(win, x0 + (a > 0 ? a : 0) * CELL, top,
                            (b - (a > 0 ? a : 0)) * CELL, ROW_H, P.match);
            }
        if (y == by1 || y == by2) {
            int bxx = y == by1 ? bx1 : bx2;
            int a = vcol(d, y, bxx) - d->hoff;
            if (a >= 0)
                ud_fill(win, x0 + a * CELL, top + 1, CELL, ROW_H - 2, P.hover);
            if (y == by1 && y == by2) {
                a = vcol(d, y, bx2) - d->hoff;
                if (a >= 0)
                    ud_fill(win, x0 + a * CELL, top + 1, CELL, ROW_H - 2,
                            P.hover);
            }
        }
        char num[12];
        snprintf(num, sizeof(num), "%d", y + 1);
        ud_text_right(win, ex + GUTTER_W - 6, top + TEXT_BASE, num,
                      y == d->cy ? P.gutter_cur : P.gutter, UD_MONO, 11);
        if (probs_in(d->rel, y + 1)) {
            ud_disc(win, ex + 5, top + 5, 8, P.err);
            int w = vcol(d, y, d->len[y]) - d->hoff;
            if (w > 0)
                ud_fill(win, x0, top + ROW_H - 2, w * CELL, 1, P.err);
        }
        static unsigned char cls_s[4096];
        unsigned char *cls = d->len[y] < (int)sizeof(cls_s) ? cls_s :
                             malloc((size_t)d->len[y] + 1);
        if (!cls)
            continue;
        colour_line(d->lang, d->ln[y], d->len[y], d->st[y], cls);
        draw_line_text(d, y, top, cls);
        if (cls != cls_s)
            free(cls);
    }
    if (focus == FOC_EDITOR && popup == POP_NONE &&
        d->cy >= d->top && d->cy < d->top + rows) {
        int v = vcol(d, d->cy, d->cx) - d->hoff;
        /* in the column between two cells, where no letter reaches */
        ud_fill(win, x0 + v * CELL - 1, y0 + (d->cy - d->top) * ROW_H + 1, 1,
                ROW_H - 2, P.ui);
    }
    ud_unclip(win);
    /* the gutter's edge */
    ud_fill(win, ex + GUTTER_W, y0, 1, bot_y() - y0, P.cur);
}

static void draw_bottom(void)
{
    int x0 = ed_x(), y0 = bot_y();
    ud_fill(win, x0, y0, cw - x0, BOTTOM_H, P.panel);
    ud_fill(win, x0, y0, cw - x0, 1, P.border);
    char title[96];
    struct out *o = NULL;
    if (bottom_tool == BOT_RUN) {
        snprintf(title, sizeof(title), "Run: %s", proj_name);
        o = &run.o;
    } else if (bottom_tool == BOT_TERMINAL) {
        scopy(title, "Terminal: Local", sizeof(title));
        o = &term.o;
    } else {
        snprintf(title, sizeof(title), "Problems: %d", prob_n);
    }
    ud_text_l(win, x0 + 12, ud_baseline(y0, HEAD_H, UI_PX), title, P.ui,
              UD_BOLD, UI_PX);
    int cy = y0 + HEAD_H;
    int rows = (BOTTOM_H - HEAD_H) / ROW_H;
    ud_clip(win, x0, cy, cw - x0, BOTTOM_H - HEAD_H);
    if (o) {
        int input = bottom_tool == BOT_TERMINAL;
        int shown = rows - input;
        /* an empty last line is only where the next one will go */
        int last = o->n - (o->n && !o->len ? 1 : 0);
        int first = last - shown - o->scroll;
        if (first < 0)
            first = 0;
        for (int r = 0; r < shown && first + r < last; r++) {
            int i = first + r;
            uint32_t c = o->kind[i] == 1 ? P.err : o->kind[i] == 2 ? P.dim :
                         P.text;
            if (o->kind[i] == 2 && !strncmp(o->line[i], "Process finished", 16) &&
                strcmp(o->line[i] + strlen(o->line[i]) - 2, " 0"))
                c = P.err;
            ud_cells(win, x0 + 10, cy + r * ROW_H + TEXT_BASE, CELL,
                     o->line[i], c, UD_MONO, TEXT_PX);
        }
        if (input) {
            int ty = cy + (rows - 1) * ROW_H;
            char line[200];
            snprintf(line, sizeof(line), "$ %.*s", term_len, term_in);
            ud_cells(win, x0 + 10, ty + TEXT_BASE, CELL, line, P.ui, UD_MONO,
                     TEXT_PX);
            if (focus == FOC_TERMINAL && popup == POP_NONE)
                ud_fill(win, x0 + 10 + (2 + term_len) * CELL, ty + 2, 2,
                        ROW_H - 4, P.ui);
        }
    } else {
        for (int i = 0; i < prob_n && i < rows; i++) {
            int y = cy + i * TREE_ROW_H;
            ud_disc(win, x0 + 12, y + 5, 12, P.err);
            ud_fill(win, x0 + 17, y + 7, 2, 5, 0xFFFFFF);
            ud_fill(win, x0 + 17, y + 13, 2, 2, 0xFFFFFF);
            int e = ud_text(win, x0 + 32, ud_baseline(y, TREE_ROW_H, UI_PX),
                            probs[i].msg, P.ui, UD_SANS, UI_PX);
            char at[220];
            snprintf(at, sizeof(at), "%s:%d", probs[i].rel, probs[i].line);
            ud_text(win, e + 10, ud_baseline(y, TREE_ROW_H, UI_PX), at, P.dim,
                    UD_SANS, 11);
        }
        if (!prob_n)
            ud_text_l(win, x0 + 12, cy + 20, "No problems found", P.dim,
                      UD_SANS, UI_PX);
    }
    ud_unclip(win);
}

static void draw_status(void)
{
    int y = ch - STATUS_H;
    ud_fill(win, 0, y, cw, STATUS_H, P.panel);
    ud_fill(win, 0, y, cw, 1, P.border);
    int base = ud_baseline(y, STATUS_H, 11);
    int right = cw - 12;
    if (cur >= 0) {
        struct doc *d = docs[cur];
        char b[128];
        snprintf(b, sizeof(b), "%d:%d    LF    UTF-8    4 spaces    %s",
                 d->cy + 1, vcol(d, d->cy, d->cx) + 1, lang_names[d->lang]);
        int w = ud_width(b, UD_SANS, 11);
        right -= w;
        ud_text(win, right, base, b, P.dim, UD_SANS, 11);
    }
    ud_clip(win, 0, y, right - 12, STATUS_H);
    if (note[0] && now_ms() - note_at < 5000) {
        ud_text(win, 12, base, note, P.ui, UD_SANS, 11);
    } else {
        /* where the file is: the project, then each folder, then it */
        char crumbs[400];
        int k = snprintf(crumbs, sizeof(crumbs), "%s", proj_name);
        if (cur >= 0)
            for (const char *s = docs[cur]->rel;
                 *s && k < (int)sizeof(crumbs) - 4; s++) {
                if (s == docs[cur]->rel || *s == '/') {
                    memcpy(crumbs + k, " > ", 3);
                    k += 3;
                    if (*s == '/')
                        continue;
                }
                crumbs[k++] = *s;
            }
        crumbs[k] = 0;
        ud_text(win, 12, base, crumbs, P.dim, UD_SANS, 11);
    }
    ud_unclip(win);
}

/* --- popups -------------------------------------------------------------- */

#define PM_NEW   0
#define PM_OPEN  1
#define PM_CLOSE 2
#define PM_FIRST 3                     /* the projects opened lately */

static int pop_rows(void)
{
    switch (popup) {
    case POP_GOTO: return match_n < POP_ROWS ? match_n : POP_ROWS;
    case POP_OPEN: return recent_n < POP_ROWS ? recent_n : POP_ROWS;
    case POP_PROJECT: return PM_FIRST + (recent_n < 7 ? recent_n : 7);
    default: return 0;
    }
}

static void draw_popup(void)
{
    if (popup == POP_PROJECT) {
        int x = 10, y = BAR_H - 2, w = 280, n = pop_rows();
        ud_round(win, x - 1, y - 1, w + 2, n * POP_ROW_H + 10, 8, P.border);
        ud_round(win, x, y, w, n * POP_ROW_H + 8, 8, P.panel);
        for (int i = 0; i < n; i++) {
            int ry = y + 4 + i * POP_ROW_H;
            if (i == pick)
                ud_round(win, x + 4, ry, w - 8, POP_ROW_H, 4, P.tree_sel);
            int base = ud_baseline(ry, POP_ROW_H, UI_PX);
            if (i == PM_NEW)
                ud_text_l(win, x + 14, base, "New Project...", P.ui, UD_SANS,
                          UI_PX);
            else if (i == PM_OPEN)
                ud_text_l(win, x + 14, base, "Open...", P.ui, UD_SANS, UI_PX);
            else if (i == PM_CLOSE)
                ud_text_l(win, x + 14, base, "Close Project", P.ui, UD_SANS,
                          UI_PX);
            else {
                const char *p = recent[i - PM_FIRST];
                badge(x + 12, ry + 4, 18, base_name(p));
                ud_text(win, x + 38, base, base_name(p), P.ui, UD_SANS, UI_PX);
            }
        }
        return;
    }
    int x = pop_x(), y = POP_Y, n = pop_rows();
    int h = POP_TITLE_H + POP_IN_H + n * POP_ROW_H + 8;
    ud_round(win, x - 1, y - 1, POP_W + 2, h + 2, 10, P.border);
    ud_round(win, x, y, POP_W, h, 10, P.panel);
    const char *title = popup == POP_GOTO ? "Go to File" :
                        popup == POP_NEWFILE ? "New File" :
                        popup == POP_RUNCFG ? "Run Configuration" :
                        "Open Project";
    ud_text_l(win, x + 14, ud_baseline(y, POP_TITLE_H + 4, UI_PX), title, P.ui,
              UD_BOLD, UI_PX);
    if (popup == POP_RUNCFG)
        ud_text_l(win, x + 150, ud_baseline(y, POP_TITLE_H + 4, 11),
                  "Run starts it in the project's folder", P.dim, UD_SANS, 11);
    field(x + 10, y + POP_TITLE_H + 2, POP_W - 20, ask, 1, 1);
    for (int i = 0; i < n; i++) {
        int ry = y + POP_TITLE_H + POP_IN_H + i * POP_ROW_H;
        if (i == pick)
            ud_round(win, x + 6, ry, POP_W - 12, POP_ROW_H, 4, P.tree_sel);
        int base = ud_baseline(ry, POP_ROW_H, UI_PX);
        if (popup == POP_GOTO) {
            const char *rel = ents[matches[i]].rel;
            file_icon(x + 16, ry + (POP_ROW_H - 16) / 2, rel);
            int e = ud_text(win, x + 40, base, base_name(rel), P.ui, UD_SANS,
                            UI_PX);
            if (strchr(rel, '/')) {
                char dir[200];
                scopy(dir, rel, (size_t)(base_name(rel) - rel));
                ud_text(win, e + 10, base, dir, P.dim, UD_SANS, 11);
            }
        } else {
            char shortp[PATH_MAX_];
            home_short(shortp, sizeof(shortp), recent[i]);
            badge(x + 14, ry + 4, 18, base_name(recent[i]));
            int e = ud_text(win, x + 40, base, base_name(recent[i]), P.ui,
                            UD_SANS, UI_PX);
            ud_text(win, e + 10, base, shortp, P.dim, UD_SANS, 11);
        }
    }
}

static void draw(void)
{
    struct ud_look l;
    ud_look(&l);
    P = l.dark ? dark_pal : light_pal;
    if (screen == SCR_WELCOME) {
        draw_welcome();
    } else if (screen == SCR_NEW) {
        draw_new();
    } else {
        draw_toolbar();
        draw_stripe();
        if (left_tool != LEFT_NONE)
            draw_left();
        draw_tabs();
        draw_editor();
        if (find_open)
            draw_findbar();            /* over the editor's ground */
        if (bottom_tool != BOT_NONE)
            draw_bottom();
        draw_status();
    }
    if (popup != POP_NONE)
        draw_popup();
}

/* --- editing --------------------------------------------------------------- */

static void after_edit(struct doc *d)
{
    d->pref = vcol(d, d->cy, d->cx);
    show_caret();
    if (left_tool == LEFT_STRUCTURE)
        find_symbols(d);
}

static int closer_of(int c)
{
    return c == '(' ? ')' : c == '[' ? ']' : c == '{' ? '}' : 0;
}

static void type_char(struct doc *d, int c)
{
    char next = d->cx < d->len[d->cy] ? d->ln[d->cy][d->cx] : 0;
    char prev = d->cx > 0 ? d->ln[d->cy][d->cx - 1] : 0;
    /* typing the closing half that is already there steps over it */
    if (!d->sel && (c == ')' || c == ']' || c == '}' || c == '"' || c == '\'') &&
        next == c) {
        d->cx++;
        caret_moved(d);
        after_edit(d);
        return;
    }
    remember(d, K_TYPE);
    if (d->sel) {
        erase_sel(d);
        next = d->cx < d->len[d->cy] ? d->ln[d->cy][d->cx] : 0;
    }
    /* a closing brace on a line of nothing but indent goes back a step */
    if (c == '}' && d->cx == indent_of(d, d->cy) && d->cx >= TABW &&
        d->cx == d->len[d->cy]) {
        erase(d, d->cy, d->cx - TABW, d->cy, d->cx);
    }
    char s[2] = { (char)c, 0 };
    int pair = closer_of(c);
    int quote = (c == '"' || c == '\'') && !is_id(prev) && !is_id(next) &&
                d->lang != L_TEXT && d->lang != L_MD;
    if ((pair && (!next || next == ' ' || next == ')' || next == ']' ||
                  next == '}' || next == ';' || next == ',') && c != '{') ||
        quote) {
        s[1] = (char)(pair ? pair : c);
        insert(d, s, 2);
        d->cx--;
    } else {
        insert(d, s, 1);
    }
    after_edit(d);
}

static void enter_key(struct doc *d)
{
    remember(d, K_OTHER);
    erase_sel(d);
    int ind = indent_of(d, d->cy);
    if (ind > d->cx)
        ind = d->cx;
    char prev = 0;
    for (int i = d->cx - 1; i >= 0; i--)
        if (d->ln[d->cy][i] != ' ') {
            prev = d->ln[d->cy][i];
            break;
        }
    char next = d->cx < d->len[d->cy] ? d->ln[d->cy][d->cx] : 0;
    int more = prev == '{' || prev == '(' || prev == '[' ||
               (prev == ':' && d->lang == L_PY);
    char *pad = malloc((size_t)ind + TABW + 2);
    if (!pad)
        return;
    memcpy(pad + 1, d->ln[d->cy], (size_t)ind);
    pad[0] = '\n';
    int n = 1 + ind;
    if (more) {
        memset(pad + n, ' ', TABW);
        n += TABW;
    }
    insert(d, pad, n);
    /* between a pair of braces the closing one gets a line of its own */
    if (more && next && closer_of(prev) == next) {
        int y = d->cy, x = d->cx;
        insert(d, pad, 1 + ind);
        d->cy = y;
        d->cx = x;
    }
    free(pad);
    after_edit(d);
}

static void backspace(struct doc *d)
{
    if (d->sel) {
        remember(d, K_OTHER);
        erase_sel(d);
    } else if (d->cx > 0) {
        remember(d, K_ERASE);
        char p = d->ln[d->cy][d->cx - 1];
        char n = d->cx < d->len[d->cy] ? d->ln[d->cy][d->cx] : 0;
        int x0 = prev_char(d, d->cy, d->cx);
        int x1 = d->cx;
        if (n && (closer_of(p) == n || ((p == '"' || p == '\'') && n == p)))
            x1++;                      /* the empty pair goes together */
        else if (d->cx == indent_of(d, d->cy) && d->cx % TABW == 0 &&
                 d->cx >= TABW)
            x0 = d->cx - TABW;         /* indent goes a step at a time */
        erase(d, d->cy, x0, d->cy, x1);
    } else if (d->cy > 0) {
        remember(d, K_OTHER);
        int y = d->cy - 1;
        erase(d, y, d->len[y], d->cy, 0);
    }
    after_edit(d);
}

static void indent_lines(struct doc *d, int out)
{
    int y0 = d->cy, y1 = d->cy, x0, x1;
    if (d->sel)
        sel_bounds(d, &y0, &x0, &y1, &x1);
    remember(d, K_OTHER);
    for (int y = y0; y <= y1; y++) {
        if (out) {
            int k = 0;
            while (k < TABW && k < d->len[y] && d->ln[y][k] == ' ')
                k++;
            if (k) {
                memmove(d->ln[y], d->ln[y] + k, (size_t)(d->len[y] - k + 1));
                d->len[y] -= k;
                if (y == d->cy)
                    d->cx = d->cx > k ? d->cx - k : 0;
                if (d->sel && y == d->sy)
                    d->sx = d->sx > k ? d->sx - k : 0;
            }
        } else {
            line_room(d, y, d->len[y] + TABW);
            memmove(d->ln[y] + TABW, d->ln[y], (size_t)d->len[y] + 1);
            memset(d->ln[y], ' ', TABW);
            d->len[y] += TABW;
            if (y == d->cy)
                d->cx += TABW;
            if (d->sel && y == d->sy)
                d->sx += TABW;
        }
        touched(d, y);
    }
    after_edit(d);
}

static void tab_key(struct doc *d, int shift)
{
    if (shift || d->sel) {
        indent_lines(d, shift);
        return;
    }
    remember(d, K_TYPE);
    int v = vcol(d, d->cy, d->cx);
    char sp[TABW + 1];
    int n = TABW - v % TABW;
    memset(sp, ' ', (size_t)n);
    insert(d, sp, n);
    after_edit(d);
}

/* Ctrl+/: the line (or every line of the selection) commented out, or
   back in if they all were */
static void toggle_comment(struct doc *d)
{
    const char *cm = comment_of(d->lang);
    if (!cm)
        return;
    int cl = (int)strlen(cm);
    int y0 = d->cy, y1 = d->cy, x0, x1;
    if (d->sel)
        sel_bounds(d, &y0, &x0, &y1, &x1);
    int all = 1;
    for (int y = y0; y <= y1; y++) {
        int i = indent_of(d, y);
        if (i < d->len[y] && strncmp(d->ln[y] + i, cm, (size_t)cl))
            all = 0;
    }
    remember(d, K_OTHER);
    for (int y = y0; y <= y1; y++) {
        int i = indent_of(d, y);
        if (all) {
            if (strncmp(d->ln[y] + i, cm, (size_t)cl))
                continue;
            int k = cl + (d->ln[y][i + cl] == ' ' ? 1 : 0);
            memmove(d->ln[y] + i, d->ln[y] + i + k,
                    (size_t)(d->len[y] - i - k + 1));
            d->len[y] -= k;
            if (y == d->cy && d->cx > i)
                d->cx = d->cx - k > i ? d->cx - k : i;
        } else {
            line_room(d, y, d->len[y] + cl + 1);
            memmove(d->ln[y] + i + cl + 1, d->ln[y] + i,
                    (size_t)(d->len[y] - i + 1));
            memcpy(d->ln[y] + i, cm, (size_t)cl);
            d->ln[y][i + cl] = ' ';
            d->len[y] += cl + 1;
            if (y == d->cy && d->cx >= i)
                d->cx += cl + 1;
        }
        touched(d, y);
    }
    d->sel = 0;
    if (y0 == y1 && d->cy + 1 < d->n) {
        d->cy++;                       /* and on to the next, as the IDEs do */
        if (d->cx > d->len[d->cy])
            d->cx = d->len[d->cy];
    }
    after_edit(d);
}

static void duplicate_line(struct doc *d)
{
    remember(d, K_OTHER);
    if (d->sel) {
        int n;
        char *t = sel_text(d, &n);
        int y0, x0, y1, x1;
        sel_bounds(d, &y0, &x0, &y1, &x1);
        d->cy = y1;
        d->cx = x1;
        d->sel = 0;
        if (t) {
            insert(d, t, n);
            free(t);
        }
    } else {
        line_insert(d, d->cy + 1, d->ln[d->cy], d->len[d->cy]);
        touched(d, d->cy);
        d->cy++;
    }
    after_edit(d);
}

static void delete_line(struct doc *d)
{
    remember(d, K_OTHER);
    d->sel = 0;
    if (d->n == 1) {
        erase(d, 0, 0, 0, d->len[0]);
    } else {
        line_remove(d, d->cy);
        touched(d, d->cy > 0 ? d->cy - 1 : 0);
        if (d->cy >= d->n)
            d->cy = d->n - 1;
        d->cx = byte_at(d, d->cy, d->pref);
    }
    after_edit(d);
}

static void copy_sel(struct doc *d, int cut)
{
    int n;
    char *t;
    if (d->sel) {
        t = sel_text(d, &n);
    } else {
        /* nothing selected: the line, as the IDEs do */
        n = d->len[d->cy] + 1;
        t = malloc((size_t)n + 1);
        if (t) {
            memcpy(t, d->ln[d->cy], (size_t)d->len[d->cy]);
            t[n - 1] = '\n';
            t[n] = 0;
        }
    }
    if (!t)
        return;
    sys_clip_set(t, (uint32_t)(n < CLIP_MAX ? n : CLIP_MAX));
    free(t);
    if (cut) {
        if (d->sel) {
            remember(d, K_OTHER);
            erase_sel(d);
            after_edit(d);
        } else {
            delete_line(d);
        }
    }
}

static void paste(struct doc *d)
{
    char b[CLIP_MAX + 1];
    int n = (int)sys_clip_get(b, sizeof(b) - 1);
    if (n <= 0)
        return;
    remember(d, K_OTHER);
    erase_sel(d);
    insert(d, b, n);
    after_edit(d);
}

static void move_caret(struct doc *d, int key, int shift, int ctrl)
{
    if (shift && !d->sel) {
        d->sel = 1;
        d->sy = d->cy;
        d->sx = d->cx;
    } else if (!shift && d->sel) {
        int y0, x0, y1, x1;
        sel_bounds(d, &y0, &x0, &y1, &x1);
        d->sel = 0;
        if (key == KEY_LEFT || key == KEY_RIGHT) {
            d->cy = key == KEY_LEFT ? y0 : y1;
            d->cx = key == KEY_LEFT ? x0 : x1;
            d->pref = vcol(d, d->cy, d->cx);
            show_caret();
            caret_moved(d);
            return;
        }
    }
    int keep = 0;
    switch (key) {
    case KEY_LEFT:
        if (ctrl) {
            while (d->cx > 0 && !is_id(d->ln[d->cy][d->cx - 1]))
                d->cx--;
            while (d->cx > 0 && is_id(d->ln[d->cy][d->cx - 1]))
                d->cx--;
        } else if (d->cx > 0) {
            d->cx = prev_char(d, d->cy, d->cx);
        } else if (d->cy > 0) {
            d->cy--;
            d->cx = d->len[d->cy];
        }
        break;
    case KEY_RIGHT:
        if (ctrl) {
            while (d->cx < d->len[d->cy] && !is_id(d->ln[d->cy][d->cx]))
                d->cx++;
            while (d->cx < d->len[d->cy] && is_id(d->ln[d->cy][d->cx]))
                d->cx++;
        } else if (d->cx < d->len[d->cy]) {
            d->cx = next_char(d, d->cy, d->cx);
        } else if (d->cy + 1 < d->n) {
            d->cy++;
            d->cx = 0;
        }
        break;
    case KEY_UP:
    case KEY_DOWN:
    case KEY_PGUP:
    case KEY_PGDN: {
        int step = key == KEY_UP || key == KEY_DOWN ? 1 : code_rows() - 1;
        int y = d->cy + (key == KEY_UP || key == KEY_PGUP ? -step : step);
        if (y < 0)
            y = 0;
        if (y >= d->n)
            y = d->n - 1;
        d->cy = y;
        d->cx = byte_at(d, y, d->pref);
        keep = 1;
        break;
    }
    case KEY_HOME:
        if (ctrl) {
            d->cy = 0;
            d->cx = 0;
        } else {
            /* first to the text, then to the margin */
            int i = indent_of(d, d->cy);
            d->cx = d->cx == i ? 0 : i;
        }
        break;
    case KEY_END:
        if (ctrl)
            d->cy = d->n - 1;
        d->cx = d->len[d->cy];
        break;
    }
    if (!keep)
        d->pref = vcol(d, d->cy, d->cx);
    if (d->sel && d->sy == d->cy && d->sx == d->cx)
        d->sel = 0;
    caret_moved(d);
    show_caret();
}

static void go_to(const char *rel, int line)
{
    if (doc_open(rel) < 0)
        return;
    struct doc *d = docs[cur];
    d->cy = line > 0 ? (line - 1 < d->n ? line - 1 : d->n - 1) : 0;
    d->cx = indent_of(d, d->cy);
    d->sel = 0;
    d->pref = vcol(d, d->cy, d->cx);
    caret_moved(d);
    /* the line a little way down from the top, with what leads to it */
    d->top = d->cy > 3 ? d->cy - 3 : 0;
    show_caret();
    reveal(rel);
    left_reset();
}

/* The next match after the caret (or before it), going round the end;
   'here' lets one start at the caret itself - as the text is typed the
   match grows where it is rather than jumping on */
static void find_next(int back, int here)
{
    if (cur < 0 || !find_len)
        return;
    struct doc *d = docs[cur];
    int y = d->cy, x = d->cx;
    for (int k = 0; k <= d->n; k++) {
        int found = -1;
        if (!back) {
            found = find_in(d, y, k ? 0 : x + (d->sel || here ? 0 : 1),
                            find_text, find_len);
            if (!k && d->sel && found == d->sx && y == d->sy)
                found = find_in(d, y, x + 1, find_text, find_len);
        } else {
            int lim = k ? d->len[y] : (d->sel ? d->sx : x) - 1;
            for (int i = find_in(d, y, 0, find_text, find_len);
                 i >= 0 && i <= lim;
                 i = find_in(d, y, i + 1, find_text, find_len))
                found = i;
        }
        if (found >= 0) {
            d->sel = 1;
            d->sy = y;
            d->sx = found;
            d->cy = y;
            d->cx = found + find_len;
            d->pref = vcol(d, y, d->cx);
            caret_moved(d);
            show_caret();
            return;
        }
        y = back ? (y + d->n - 1) % d->n : (y + 1) % d->n;
    }
}

/* --- keys ------------------------------------------------------------------ */

static void popup_open(int which)
{
    popup = which;
    pick = 0;
    ask_len = 0;
    ask[0] = 0;
    if (which == POP_RUNCFG) {
        scopy(ask, run_cmd, sizeof(ask));
        ask_len = (int)strlen(ask);
    }
    if (which == POP_GOTO) {
        rescan();
        find_matches();
    }
}

/* Where a new file goes: the folder picked in the tree, or the one the
   file picked there is in */
static void new_file_dir(char *out, size_t cap)
{
    out[0] = 0;
    int k = ent_find(tree_sel);
    if (k < 0)
        return;
    if (ents[k].is_dir) {
        scopy(out, ents[k].rel, cap);
    } else if (strchr(ents[k].rel, '/')) {
        scopy(out, ents[k].rel, (size_t)(base_name(ents[k].rel) - ents[k].rel));
    }
}

static void create_file(const char *name)
{
    char dir[200], rel[256];
    new_file_dir(dir, sizeof(dir));
    if (dir[0])
        join(rel, sizeof(rel), dir, name);
    else
        scopy(rel, name, sizeof(rel));
    /* the folders on the way, if the name has any */
    for (int i = 0; rel[i]; i++)
        if (rel[i] == '/') {
            rel[i] = 0;
            sys_mkdir(rel);
            rel[i] = '/';
        }
    int fd = (int)sys_open(rel, O_READ);
    if (fd >= 0) {
        sys_close(fd);
    } else if (spill(rel, "", 0) < 0) {
        say("Could not make that file");
        return;
    }
    rescan();
    reveal(rel);
    go_to(rel, 1);
}

static void popup_enter(void)
{
    int which = popup;
    popup = POP_NONE;
    if (which == POP_GOTO) {
        if (pick < match_n)
            go_to(ents[matches[pick]].rel, 0);
    } else if (which == POP_NEWFILE) {
        if (ask_len)
            create_file(ask);
    } else if (which == POP_RUNCFG) {
        scopy(run_cmd, ask, sizeof(run_cmd));
        project_save();
        say(run_cmd[0] ? "Run configuration saved" : "Run configuration cleared");
    } else if (which == POP_OPEN) {
        if (ask[0] == '/')
            project_open(ask);
        else if (pick < recent_n)
            project_open(recent[pick]);
    } else if (which == POP_PROJECT) {
        if (pick == PM_NEW) {
            ask[0] = 0;
            ask_len = 0;
            screen = SCR_NEW;
        } else if (pick == PM_OPEN) {
            popup_open(POP_OPEN);
        } else if (pick == PM_CLOSE) {
            project_close();
        } else if (pick - PM_FIRST < recent_n) {
            project_open(recent[pick - PM_FIRST]);
        }
    }
}

static int text_key(int c)
{
    return (unsigned char)c >= 0x20 && c != 0x7F;
}

static void key_popup(int c, int mods)
{
    if (c == 27 && !(mods & WEV_MOD_SHIFT)) {
        popup = POP_NONE;
        return;
    }
    if (c == '\n' || c == '\r') {
        popup_enter();
        return;
    }
    if (c == KEY_UP || c == KEY_DOWN) {
        int n = pop_rows();
        if (n)
            pick = (pick + (c == KEY_UP ? n - 1 : 1)) % n;
        return;
    }
    if (popup == POP_PROJECT)
        return;
    if (c == '\b') {
        if (ask_len)
            ask[--ask_len] = 0;
    } else if (text_key(c) && ask_len < (int)sizeof(ask) - 1) {
        ask[ask_len++] = (char)c;
        ask[ask_len] = 0;
    } else {
        return;
    }
    pick = 0;
    if (popup == POP_GOTO)
        find_matches();
}

static void key_new(int c, int mods)
{
    if (c == 27 && !(mods & WEV_MOD_SHIFT)) {
        screen = proj[0] ? SCR_IDE : SCR_WELCOME;
        return;
    }
    if (c == '\n' || c == '\r') {
        project_create(ask, tpl_pick);
        return;
    }
    if (c == KEY_UP || c == KEY_DOWN) {
        tpl_pick = (tpl_pick + (c == KEY_UP ? TPL_N - 1 : 1)) % TPL_N;
        return;
    }
    if (c == '\b') {
        if (ask_len)
            ask[--ask_len] = 0;
    } else if (text_key(c) && c != '/' && ask_len < 40) {
        ask[ask_len++] = (char)c;
        ask[ask_len] = 0;
    }
}

static void key_terminal(int c, int mods)
{
    if (c == 27 && !(mods & WEV_MOD_SHIFT)) {
        focus = FOC_EDITOR;
        return;
    }
    if (c == '\n' || c == '\r') {
        term_send();
    } else if (c == '\b') {
        if (term_len)
            term_len--;
    } else if (c == KEY_PGUP) {
        term.o.scroll += 4;
    } else if (c == KEY_PGDN) {
        term.o.scroll = term.o.scroll > 4 ? term.o.scroll - 4 : 0;
    } else if (text_key(c) && term_len < (int)sizeof(term_in) - 2) {
        term_in[term_len++] = (char)c;
    }
}

static void key_find(int c, int mods)
{
    if (c == 27 && !(mods & WEV_MOD_SHIFT)) {
        find_open = 0;
        focus = FOC_EDITOR;
        return;
    }
    if (c == '\n' || c == '\r' || c == KEY_DOWN) {
        find_next(0, 0);
    } else if (c == KEY_UP) {
        find_next(1, 0);
    } else if (c == '\b') {
        if (find_len)
            find_text[--find_len] = 0;
    } else if (text_key(c) && find_len < (int)sizeof(find_text) - 1) {
        find_text[find_len++] = (char)c;
        find_text[find_len] = 0;
        /* as it is typed, the first match from where the caret was */
        if (cur >= 0) {
            struct doc *d = docs[cur];
            if (d->sel) {
                d->cy = d->sy;
                d->cx = d->sx;
                d->sel = 0;
            }
            find_next(0, 1);
        }
    }
}

static void key_ide(int c, int mods)
{
    int shift = mods & WEV_MOD_SHIFT, ctrl = mods & WEV_MOD_CTRL,
        alt = mods & WEV_MOD_ALT;
    struct doc *d = cur >= 0 ? docs[cur] : NULL;
    /* the keys that work wherever the focus is */
    if (c == KEY_OPEN || (ctrl && c == 'N')) {
        popup_open(POP_GOTO);
        return;
    }
    if (ctrl && (c == 'r' || c == 'R')) {
        run_start();
        return;
    }
    if (ctrl && c == 'n') {
        popup_open(POP_NEWFILE);
        return;
    }
    if (c == KEY_SAVE) {
        save_all();
        if (d)
            say_name("Saved", base_name(d->rel));
        project_save();
        return;
    }
    if (alt && (c == KEY_LEFT || c == KEY_RIGHT) && doc_n) {
        cur = (cur + (c == KEY_LEFT ? doc_n - 1 : 1)) % doc_n;
        scopy(tree_sel, docs[cur]->rel, sizeof(tree_sel));
        left_reset();
        return;
    }
    if (focus == FOC_TERMINAL) {
        key_terminal(c, mods);
        return;
    }
    if (focus == FOC_FIND) {
        key_find(c, mods);
        return;
    }
    if (c == 27 && !shift) {
        if (find_open)
            find_open = 0;
        else if (d && d->sel)
            d->sel = 0;
        else
            bottom_tool = BOT_NONE;
        return;
    }
    if (!d)
        return;
    if (c == KEY_FIND) {
        find_open = 1;
        focus = FOC_FIND;
        if (d->sel && d->sy == d->cy) {
            int y0, x0, y1, x1;
            sel_bounds(d, &y0, &x0, &y1, &x1);
            int n = x1 - x0 < (int)sizeof(find_text) - 1 ? x1 - x0 :
                    (int)sizeof(find_text) - 1;
            memcpy(find_text, d->ln[y0] + x0, (size_t)n);
            find_text[n] = 0;
            find_len = n;
        }
        return;
    }
    if (c == KEY_UNDO) {
        if (shift)
            restore(d, d->redo, &d->redo_n, d->undo, &d->undo_n);
        else
            restore(d, d->undo, &d->undo_n, d->redo, &d->redo_n);
        after_edit(d);
        return;
    }
    if (c == KEY_INTR || c == KEY_COPY || (ctrl && c == 'x')) {
        copy_sel(d, c != KEY_INTR && c != KEY_COPY);
        return;
    }
    if ((c == KEY_PASTE && shift) || (ctrl && c == 'v')) {
        paste(d);
        return;
    }
    if (c == KEY_EOF) {
        duplicate_line(d);
        return;
    }
    if (ctrl && c == 'y') {
        delete_line(d);
        return;
    }
    if (ctrl && c == '/') {
        toggle_comment(d);
        return;
    }
    if (ctrl && c == 'a') {
        d->sel = 1;
        d->sy = 0;
        d->sx = 0;
        d->cy = d->n - 1;
        d->cx = d->len[d->cy];
        caret_moved(d);
        return;
    }
    if (ctrl && c == 'w') {
        doc_close(cur);
        left_reset();
        return;
    }
    switch (c) {
    case KEY_LEFT: case KEY_RIGHT: case KEY_UP: case KEY_DOWN:
    case KEY_PGUP: case KEY_PGDN: case KEY_HOME: case KEY_END:
        move_caret(d, c, shift, ctrl);
        return;
    case '\n':
    case '\r':
        enter_key(d);
        return;
    case '\b':
        backspace(d);
        return;
    case '\t':
        tab_key(d, shift);
        return;
    }
    if (ctrl || !text_key(c))
        return;
    type_char(d, c);
}

static void key_welcome(int c, int mods)
{
    if ((mods & WEV_MOD_CTRL) && c == 'n') {
        ask[0] = 0;
        ask_len = 0;
        screen = SCR_NEW;
        return;
    }
    if (c == KEY_OPEN) {
        popup_open(POP_OPEN);
        return;
    }
    if ((c == '\n' || c == '\r') && recent_n)
        project_open(recent[0]);
}

static void key(int c, int mods)
{
    if (popup != POP_NONE)
        key_popup(c, mods);
    else if (screen == SCR_NEW)
        key_new(c, mods);
    else if (screen == SCR_WELCOME)
        key_welcome(c, mods);
    else
        key_ide(c, mods);
}

/* --- the mouse ------------------------------------------------------------- */

static int in(int x, int y, int bx, int by, int bw, int bh)
{
    return x >= bx && x < bx + bw && y >= by && y < by + bh;
}

/* The line and byte under a point of the editor */
static void caret_at(struct doc *d, int x, int y, int *ly, int *lx)
{
    int r = (y - code_y()) / ROW_H;
    if (y < code_y())
        r = -1;
    int line = d->top + r;
    if (line < 0)
        line = 0;
    if (line >= d->n)
        line = d->n - 1;
    int v = (x - code_x() + CELL / 2) / CELL + d->hoff;
    if (v < 0)
        v = 0;
    *ly = line;
    *lx = byte_at(d, line, v);
}

static void click_popup(int x, int y)
{
    if (popup == POP_PROJECT) {
        int r = (y - (BAR_H + 2)) / POP_ROW_H;
        if (in(x, y, 10, BAR_H - 2, 280, pop_rows() * POP_ROW_H + 8) &&
            r >= 0 && r < pop_rows()) {
            pick = r;
            popup_enter();
        } else {
            popup = POP_NONE;
        }
        return;
    }
    int px = pop_x(), rows_y = POP_Y + POP_TITLE_H + POP_IN_H;
    int h = POP_TITLE_H + POP_IN_H + pop_rows() * POP_ROW_H + 8;
    if (!in(x, y, px, POP_Y, POP_W, h)) {
        popup = POP_NONE;
        return;
    }
    if (y >= rows_y) {
        int r = (y - rows_y) / POP_ROW_H;
        if (r >= 0 && r < pop_rows()) {
            pick = r;
            popup_enter();
        }
    }
}

static void click_welcome(int x, int y)
{
    int by = (WEL_HEAD_H - WEL_BTN_H) / 2;
    if (in(x, y, cw - 20 - 2 * WEL_BTN_W - 10, by, WEL_BTN_W, WEL_BTN_H)) {
        ask[0] = 0;
        ask_len = 0;
        note[0] = 0;
        screen = SCR_NEW;
        return;
    }
    if (in(x, y, cw - 20 - WEL_BTN_W, by, WEL_BTN_W, WEL_BTN_H)) {
        popup_open(POP_OPEN);
        return;
    }
    if (x > WEL_SIDE_W && y >= WEL_HEAD_H) {
        int r = (y - WEL_HEAD_H) / WEL_ROW_H;
        if (r < recent_n)
            project_open(recent[r]);
    }
}

static void click_new(int x, int y)
{
    if (x < WEL_SIDE_W && y >= TPL_TOP) {
        int r = (y - TPL_TOP) / TPL_ROW_H;
        if (r < TPL_N)
            tpl_pick = r;
        return;
    }
    int by = ch - 20 - WEL_BTN_H;
    if (in(x, y, cw - 20 - DLG_BTN_W, by, DLG_BTN_W, WEL_BTN_H))
        project_create(ask, tpl_pick);
    else if (in(x, y, cw - 30 - 2 * DLG_BTN_W, by, DLG_BTN_W, WEL_BTN_H))
        screen = proj[0] ? SCR_IDE : SCR_WELCOME;
}

static void click_ide(int x, int y, int mods)
{
    int by = (BAR_H - BTN) / 2;
    if (y < BAR_H) {
        if (x < 42 + ud_width(proj_name, UD_BOLD, UI_PX) + 20) {
            popup_open(POP_PROJECT);
        } else if (in(x, y, cw - RUN_R - 8 - CFG_W, by, CFG_W, BTN)) {
            popup_open(POP_RUNCFG);
        } else if (in(x, y, cw - RUN_R, by, BTN, BTN)) {
            run_start();
        } else if (in(x, y, cw - STOP_R, by, BTN, BTN)) {
            run_stop();
        } else if (in(x, y, cw - SEARCH_R, by, BTN, BTN)) {
            popup_open(POP_GOTO);
        }
        return;
    }
    if (y >= ch - STATUS_H)
        return;
    if (x < STRIPE_W) {
        for (int i = 0; i < 2; i++)
            if (in(x, y, 0, stripe_top_y(i), STRIPE_W, STRIPE_ICON))
                toggle_left(i == 0 ? LEFT_PROJECT : LEFT_STRUCTURE);
        for (int j = 0; j < 3; j++)
            if (in(x, y, 0, stripe_bottom_y(j), STRIPE_W, STRIPE_ICON))
                toggle_bottom(j == 0 ? BOT_RUN : j == 1 ? BOT_TERMINAL :
                              BOT_PROBLEMS);
        return;
    }
    if (left_tool != LEFT_NONE && x < STRIPE_W + SIDE_W) {
        if (y < BAR_H + HEAD_H) {
            if (left_tool == LEFT_PROJECT &&
                in(x, y, STRIPE_W + SIDE_W - PLUS_R, BAR_H, PLUS_R, HEAD_H))
                popup_open(POP_NEWFILE);
            return;
        }
        int r = (y - BAR_H - HEAD_H) / TREE_ROW_H;
        if (left_tool == LEFT_PROJECT) {
            int rows[256];
            int n = shown_rows(rows, tree_rows() < 256 ? tree_rows() : 256);
            if (r < 0 || r >= n)
                return;
            struct ent *e = &ents[rows[r]];
            scopy(tree_sel, e->rel, sizeof(tree_sel));
            if (e->is_dir)
                e->open = !e->open;
            else
                go_to(e->rel, 0);
        } else if (r >= 0 && r < sym_n && cur >= 0) {
            go_to(docs[cur]->rel, syms[r].line + 1);
        }
        return;
    }
    if (y < BAR_H + TAB_H) {
        for (int i = 0; i < doc_n; i++)
            if (x >= tab_x[i] && x < tab_x[i + 1]) {
                if (x >= tab_x[i + 1] - 24)
                    doc_close(i);
                else
                    cur = i;
                if (cur >= 0)
                    scopy(tree_sel, docs[cur]->rel, sizeof(tree_sel));
                focus = FOC_EDITOR;
                left_reset();
                return;
            }
        return;
    }
    if (find_open && y < BAR_H + TAB_H + FIND_H) {
        focus = FOC_FIND;
        return;
    }
    if (bottom_tool != BOT_NONE && y >= bot_y()) {
        if (bottom_tool == BOT_TERMINAL) {
            focus = FOC_TERMINAL;
        } else if (bottom_tool == BOT_PROBLEMS) {
            int r = (y - bot_y() - HEAD_H) / TREE_ROW_H;
            if (y >= bot_y() + HEAD_H && r < prob_n)
                go_to(probs[r].rel, probs[r].line);
        } else {
            focus = FOC_EDITOR;
        }
        return;
    }
    if (cur < 0)
        return;
    struct doc *d = docs[cur];
    focus = FOC_EDITOR;
    int ly, lx;
    caret_at(d, x, y, &ly, &lx);
    if (mods & WEV_MOD_SHIFT) {
        if (!d->sel) {
            d->sel = 1;
            d->sy = d->cy;
            d->sx = d->cx;
        }
    } else {
        d->sel = 0;
        d->sy = ly;
        d->sx = lx;
    }
    d->cy = ly;
    d->cx = lx;
    d->pref = vcol(d, ly, lx);
    caret_moved(d);
    dragging = x >= code_x() - CODE_PAD && y >= code_y();
}

static void drag_to(int x, int y)
{
    if (!dragging || cur < 0)
        return;
    struct doc *d = docs[cur];
    int ly, lx;
    caret_at(d, x, y, &ly, &lx);
    if (!d->sel) {
        d->sel = 1;
        /* sy, sx: where the button went down */
    }
    d->cy = ly;
    d->cx = lx;
    if (d->sy == d->cy && d->sx == d->cx)
        d->sel = 0;
    show_caret();
}

static void scroll(int x, int y, int notches)
{
    if (screen != SCR_IDE)
        return;
    if (left_tool == LEFT_PROJECT && x >= STRIPE_W && x < STRIPE_W + SIDE_W) {
        tree_top -= notches * 2;
        if (tree_top < 0)
            tree_top = 0;
        return;
    }
    if (bottom_tool != BOT_NONE && y >= bot_y()) {
        struct out *o = bottom_tool == BOT_RUN ? &run.o :
                        bottom_tool == BOT_TERMINAL ? &term.o : NULL;
        if (o) {
            o->scroll += notches * 3;
            if (o->scroll < 0)
                o->scroll = 0;
            if (o->scroll > o->n)
                o->scroll = o->n;
        }
        return;
    }
    if (cur < 0)
        return;
    struct doc *d = docs[cur];
    d->top -= notches * 3;
    if (d->top > d->n - 1)
        d->top = d->n - 1;
    if (d->top < 0)
        d->top = 0;
}

/* --- the loop ------------------------------------------------------------ */

static void finish(void)
{
    save_all();
    project_save();
    if (run.pid >= 0)
        sys_kill(run.pid, SIGKILL);
    if (term.pid >= 0)
        sys_kill(term.pid, SIGKILL);
}

int main(void)
{
    static const int sizes[3][2] = { { W2, H2 }, { W1, H1 }, { W0, H0 } };
    for (int i = 0; i < 3 && win < 0; i++) {
        win = (int)win_create(sizes[i][0], sizes[i][1], "Code");
        cw = sizes[i][0];
        ch = sizes[i][1];
    }
    if (win < 0)
        return 1;
    ulocale_window_title(win, "Code", "Kod");
    if (sys_getcwd(home, sizeof(home)) < 0)
        scopy(home, "/", sizeof(home));
    recent_load();
    /* the project worked on last comes back, as it was */
    if (recent_n && project_open(recent[0]) < 0)
        screen = SCR_WELCOME;
    draw();
    for (;;) {
        struct win_event ev;
        int redraw = 0;
        while (win_poll(win, &ev) == 1) {
            redraw = 1;
            switch (ev.type) {
            case WEV_CLOSE:
                finish();
                return 0;
            case WEV_SIZE:
                cw = ev.x;
                ch = ev.y;
                show_caret();
                break;
            case WEV_KEY:
                key(ev.val, ev.x);
                if (screen == SCR_IDE && popup == POP_NONE &&
                    focus == FOC_EDITOR)
                    show_caret();
                break;
            case WEV_CLICK:
                if (popup != POP_NONE)
                    click_popup(ev.x, ev.y);
                else if (screen == SCR_WELCOME)
                    click_welcome(ev.x, ev.y);
                else if (screen == SCR_NEW)
                    click_new(ev.x, ev.y);
                else
                    click_ide(ev.x, ev.y, ev.val & ~0xFF);
                break;
            case WEV_DRAG:
                drag_to(ev.x, ev.y);
                break;
            case WEV_RELEASE:
                dragging = 0;
                break;
            case WEV_SCROLL:
                scroll(ev.x, ev.y, ev.val);
                break;
            }
        }
        if (screen == SCR_IDE) {
            poll_procs(&redraw);
            /* files appear from the terminal and from programs too */
            if (now_ms() - scanned_at > 3000) {
                int before = ent_n;
                rescan();
                redraw |= before != ent_n;
            }
        }
        if (note[0] && now_ms() - note_at > 5000 && now_ms() - note_at < 5200)
            redraw = 1;
        if (redraw)
            draw();
        sys_sleep(redraw ? 10 : 30);
    }
}
