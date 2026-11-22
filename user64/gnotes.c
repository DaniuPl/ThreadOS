/* gnotes: Notes - notes and lists, in the serif
 *
 * docs/weave/canvas/NotesMend.dc.html: the notes down the left, each
 * with its dye and its first words; the chosen one on the right, its
 * day, its title large in the serif, a list's items with a knot each to
 * tick off, and what is written under them.
 *
 * A note is a file in ~/Notes - plain text any editor opens and Find
 * searches: its first line is the title, a line that begins "[ ] " or
 * "[x] " is an item of a list, ticked or not, and the rest is what is
 * written. The file is named after the title and saved a moment after
 * the typing stops, when another note is chosen and when Notes closes.
 *
 *   Ctrl+N    a new note
 *   Ctrl+L    the line the caret is on an item of a list, or not
 *   Ctrl+S    save now
 *   Ctrl+D    delete the note (asked first)
 *
 * What it does is said on the serial port, for scripts/tests/t_notes.py:
 *
 *   [notes] drawn W H
 *   [notes] list N: TITLE | TITLE | ...
 *   [notes] opened TITLE
 *   [notes] ticked LINE on|off
 *   [notes] saved PATH
 *   [notes] new | [notes] deleted TITLE
 *
 * Built against musl: notes of any length, a folder of any size.
 */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "ugui.h"

/* --- where things are (scripts/tests/t_notes.py reads these) ---------- */

#define W1 900
#define H1 640
#define W0 720
#define H0 470
#define NAV_W 230
#define NAV_PAD 10
#define NEW_H 34              /* "New note", at the top of the list */
#define NOTE_H 52             /* a note in the list */
#define NOTE_GAP 4
#define NOTE_PX 13
#define FIRST_PX 12
#define ART_X 36              /* the note, from the list's edge */
#define ART_Y 30
#define DATE_PX 12
#define TITLE_PX 30
#define TITLE_LINE 38
#define ITEM_PX 18
#define ITEM_H 30
#define ITEM_GAP 8
#define KNOT 18
#define KNOT_GAP 12
#define PARA_PX 17
#define PARA_LINE 27
#define BLOCK_GAP 14
#define FOOT_H 26             /* "Saved", at the foot */

/* the design's dyes, by the note's title */
static const uint32_t dyes[5] = {
    0x3F5EE8, 0xC2366F, 0xB86E00, 0x6B675F, 0x1E8A4C
};

/* --- the notes ------------------------------------------------------- */

#define NOTES_MAX 256
struct note {
    char file[256];           /* its name in ~/Notes */
    char title[96];
    char first[96];           /* the first words after the title */
    long long mtime;
};
static struct note notes[NOTES_MAX];
static int nnotes, cur = -1, list_top;
static char dir[300];

/* The open note, line by line */
#define LINES_MAX 2000
static char *line[LINES_MAX];
static int nlines;
static int cy, cx;            /* the caret: line, byte */
static int dirty;
static long long typed_at;    /* when the typing last changed it, in ms */
static int scroll;            /* how far down the note is shown */

static int win = -1, cw, ch;
static struct ud_look L;
static struct ug ui;
static struct ug_item ui_room[4];
static struct ug_modal ask;

static long long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static uint32_t mix(uint32_t a, uint32_t b, int alpha)
{
    uint32_t r = (((a >> 16) & 255) * alpha + ((b >> 16) & 255) * (255 - alpha)) / 255;
    uint32_t g = (((a >> 8) & 255) * alpha + ((b >> 8) & 255) * (255 - alpha)) / 255;
    uint32_t bl = ((a & 255) * alpha + (b & 255) * (255 - alpha)) / 255;
    return r << 16 | g << 8 | bl;
}

static uint32_t dye_of(const char *title)
{
    uint32_t h = 2166136261u;           /* FNV-1a: the same note, the same dye */
    for (const unsigned char *p = (const unsigned char *)title; *p; p++)
        h = (h ^ *p) * 16777619u;
    return dyes[h % 5];
}

static int item(const char *s)
{
    return (s[0] == '[' && (s[1] == ' ' || s[1] == 'x' || s[1] == 'X') &&
            s[2] == ']' && s[3] == ' ') ? (s[1] == ' ' ? 1 : 2) : 0;
}

static char *dup_n(const char *s, int n)
{
    char *d = malloc((size_t)n + 1);
    if (!d)
        return NULL;
    memcpy(d, s, (size_t)n);
    d[n] = 0;
    return d;
}

static void clear_lines(void)
{
    for (int i = 0; i < nlines; i++)
        free(line[i]);
    nlines = 0;
}

static void set_text(const char *t, int n)
{
    clear_lines();
    int s = 0;
    for (int i = 0; i <= n && nlines < LINES_MAX; i++) {
        if (i == n || t[i] == '\n') {
            int e = i;
            if (e > s && t[e - 1] == '\r')
                e--;
            line[nlines++] = dup_n(t + s, e - s);
            s = i + 1;
        }
    }
    /* a file's last newline ends its last line rather than starting one */
    if (nlines > 1 && !line[nlines - 1][0] && n && t[n - 1] == '\n') {
        free(line[--nlines]);
    }
    if (!nlines)
        line[nlines++] = dup_n("", 0);
}

static void path_of(const char *file, char *out, size_t cap)
{
    snprintf(out, cap, "%s/%s", dir, file);
}

/* The title and the first words, from a note's text */
static void heads(const char *t, struct note *n)
{
    const char *e = strchr(t, '\n');
    int len = e ? (int)(e - t) : (int)strlen(t);
    snprintf(n->title, sizeof(n->title), "%.*s", len, t);
    if (!n->title[0])
        snprintf(n->title, sizeof(n->title), "A note");
    n->first[0] = 0;
    for (const char *p = e; p && *p; ) {
        p++;
        const char *q = strchr(p, '\n');
        int l = q ? (int)(q - p) : (int)strlen(p);
        const char *s = p;
        if (item(s)) {
            s += 4;
            l -= 4;
        }
        if (l > 0) {
            snprintf(n->first, sizeof(n->first), "%.*s", l, s);
            break;
        }
        p = q;
    }
}

static int by_time(const void *a, const void *b)
{
    const struct note *x = a, *y = b;
    return x->mtime < y->mtime ? 1 : x->mtime > y->mtime ? -1
                                     : strcmp(x->title, y->title);
}

static void list_say(void)
{
    printf("[notes] list %d:", nnotes);
    for (int i = 0; i < nnotes; i++)
        printf("%s %s", i ? " |" : "", notes[i].title);
    printf("\n");
    fflush(stdout);
}

static void scan(void)
{
    nnotes = 0;
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)) && nnotes < NOTES_MAX) {
        size_t n = strlen(e->d_name);
        if (e->d_name[0] == '.' || n < 5 || strcmp(e->d_name + n - 4, ".txt"))
            continue;
        char path[600];
        path_of(e->d_name, path, sizeof(path));
        struct stat st;
        if (stat(path, &st) < 0 || !S_ISREG(st.st_mode))
            continue;
        FILE *f = fopen(path, "r");
        if (!f)
            continue;
        char head[1024];
        size_t got = fread(head, 1, sizeof(head) - 1, f);
        fclose(f);
        head[got] = 0;
        struct note *nt = &notes[nnotes++];
        snprintf(nt->file, sizeof(nt->file), "%s", e->d_name);
        heads(head, nt);
        nt->mtime = (long long)st.st_mtime;
    }
    closedir(d);
    qsort(notes, (size_t)nnotes, sizeof(notes[0]), by_time);
}

static int index_of_file(const char *file)
{
    for (int i = 0; i < nnotes; i++)
        if (!strcmp(notes[i].file, file))
            return i;
    return -1;
}

/* A title as a file's name: what a folder can hold */
static void file_of(const char *title, char *out, size_t cap)
{
    size_t n = 0;
    for (const char *p = title; *p && n + 5 < cap && n < 60; p++)
        out[n++] = (*p == '/' || *p == '\\' || *p == ':' || *p < ' ')
                   ? '-' : *p;
    while (n && out[n - 1] == ' ')
        n--;
    if (!n)
        n = (size_t)snprintf(out, cap, "A note");
    snprintf(out + n, cap - n, ".txt");
}

static void save(void)
{
    if (cur < 0 || !dirty)
        return;
    struct note *n = &notes[cur];
    char want[256], path[600], old[600];
    file_of(nlines && line[0][0] ? line[0] : "A note", want, sizeof(want));
    /* named after its title: another note of that name keeps its own */
    if (strcmp(want, n->file)) {
        char probe[256];
        snprintf(probe, sizeof(probe), "%s", want);
        for (int k = 2; index_of_file(probe) >= 0 && k < 100; k++) {
            char base[200];
            snprintf(base, sizeof(base), "%.*s", (int)strlen(want) - 4, want);
            snprintf(probe, sizeof(probe), "%s %d.txt", base, k);
        }
        path_of(n->file, old, sizeof(old));
        path_of(probe, path, sizeof(path));
        if (n->file[0])
            rename(old, path);
        snprintf(n->file, sizeof(n->file), "%s", probe);
    }
    path_of(n->file, path, sizeof(path));
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    for (int i = 0; i < nlines; i++)
        fprintf(f, "%s\n", line[i]);
    fclose(f);
    dirty = 0;
    char *t = malloc(4096);
    if (t) {
        int k = 0;
        for (int i = 0; i < nlines && k < 4000; i++)
            k += snprintf(t + k, (size_t)(4096 - k), "%s\n", line[i]);
        heads(t, n);
        free(t);
    }
    n->mtime = (long long)time(NULL);
    printf("[notes] saved %s\n", path);
    fflush(stdout);
}

static void open_note(int i)
{
    save();
    if (i < 0 || i >= nnotes)
        return;
    cur = i;
    char path[600];
    path_of(notes[i].file, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    char *t = NULL;
    int n = 0;
    if (f) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz > 0 && sz < 4 * 1024 * 1024) {
            t = malloc((size_t)sz);
            if (t)
                n = (int)fread(t, 1, (size_t)sz, f);
        }
        fclose(f);
    }
    set_text(t ? t : "", n);
    free(t);
    cy = 0;
    cx = (int)strlen(line[0]);
    scroll = 0;
    dirty = 0;
    printf("[notes] opened %s\n", notes[i].title);
    fflush(stdout);
}

static void new_note(void)
{
    save();
    if (nnotes == NOTES_MAX)
        return;
    memmove(&notes[1], &notes[0], (size_t)nnotes * sizeof(notes[0]));
    nnotes++;
    memset(&notes[0], 0, sizeof(notes[0]));
    snprintf(notes[0].title, sizeof(notes[0].title), "A note");
    notes[0].mtime = (long long)time(NULL);
    cur = 0;
    list_top = 0;
    set_text("", 0);
    cy = cx = 0;
    scroll = 0;
    dirty = 1;
    printf("[notes] new\n");
    fflush(stdout);
}

static void delete_note(void)
{
    if (cur < 0)
        return;
    char path[600];
    if (notes[cur].file[0]) {
        path_of(notes[cur].file, path, sizeof(path));
        unlink(path);
    }
    printf("[notes] deleted %s\n", notes[cur].title);
    fflush(stdout);
    memmove(&notes[cur], &notes[cur + 1],
            (size_t)(nnotes - cur - 1) * sizeof(notes[0]));
    nnotes--;
    dirty = 0;
    cur = -1;
    clear_lines();
    if (nnotes)
        open_note(0);
}

/* --- editing ---------------------------------------------------------- */

static void touched(void)
{
    dirty = 1;
    typed_at = now_ms();
}

static void insert(const char *s, int n)
{
    char *l = line[cy];
    int len = (int)strlen(l);
    char *d = malloc((size_t)len + (size_t)n + 1);
    if (!d)
        return;
    memcpy(d, l, (size_t)cx);
    memcpy(d + cx, s, (size_t)n);
    memcpy(d + cx + n, l + cx, (size_t)(len - cx) + 1);
    free(l);
    line[cy] = d;
    cx += n;
    touched();
}

static void split(void)
{
    if (nlines == LINES_MAX)
        return;
    char *l = line[cy];
    int it = cy > 0 ? item(l) : 0;
    /* Enter on an empty item ends the list */
    if (it && !l[4] && cx >= 4) {
        line[cy] = dup_n("", 0);
        free(l);
        cx = 0;
        touched();
        return;
    }
    const char *rest = l + cx;
    char *next;
    if (it && cx >= 4) {
        next = malloc(strlen(rest) + 5);
        if (!next)
            return;
        sprintf(next, "[ ] %s", rest);
    } else {
        next = dup_n(rest, (int)strlen(rest));
    }
    l[cx] = 0;
    memmove(&line[cy + 2], &line[cy + 1],
            (size_t)(nlines - cy - 1) * sizeof(line[0]));
    line[cy + 1] = next;
    nlines++;
    cy++;
    cx = it && cx >= 4 ? 4 : 0;
    touched();
}

static void back(void)
{
    if (cx > 0) {
        int k = cx - 1;
        while (k > 0 && ((unsigned char)line[cy][k] & 0xC0) == 0x80)
            k--;
        /* at an item's text start: the knot goes, not the letter */
        if (cy > 0 && item(line[cy]) && cx == 4)
            k = 0;
        memmove(line[cy] + k, line[cy] + cx, strlen(line[cy] + cx) + 1);
        cx = k;
        touched();
        return;
    }
    if (cy == 0)
        return;
    char *prev = line[cy - 1], *l = line[cy];
    size_t pl = strlen(prev);
    char *d = malloc(pl + strlen(l) + 1);
    if (!d)
        return;
    sprintf(d, "%s%s", prev, l);
    free(prev);
    free(l);
    line[cy - 1] = d;
    memmove(&line[cy], &line[cy + 1], (size_t)(nlines - cy - 1) * sizeof(line[0]));
    nlines--;
    cy--;
    cx = (int)pl;
    touched();
}

static void toggle_item_line(int y)
{
    if (y <= 0 || y >= nlines)
        return;
    char *l = line[y];
    if (item(l)) {
        memmove(l, l + 4, strlen(l + 4) + 1);
        if (y == cy)
            cx = cx >= 4 ? cx - 4 : 0;
    } else {
        char *d = malloc(strlen(l) + 5);
        if (!d)
            return;
        sprintf(d, "[ ] %s", l);
        free(l);
        line[y] = d;
        if (y == cy)
            cx += 4;
    }
    touched();
}

static void tick(int y)
{
    int it = item(line[y]);
    if (!it)
        return;
    line[y][1] = it == 1 ? 'x' : ' ';
    printf("[notes] ticked %d %s\n", y, it == 1 ? "on" : "off");
    fflush(stdout);
    touched();
    save();                   /* a tick is a thing done: kept at once */
}

/* --- the note, laid out ---------------------------------------------- */

/* A piece of a line on the screen: the bytes [s, e) of line l, at x, y
   (y the baseline), in its font */
#define PIECES_MAX 4000
static struct piece {
    int l, s, e, x, y, font, px, kind;
} pieces[PIECES_MAX];
static int npieces, doc_h;

#define P_TITLE 0
#define P_ITEM  1
#define P_PARA  2

static int art_x(void)
{
    return NAV_W + 1 + ART_X;
}

static int art_w(void)
{
    return cw - art_x() - ART_X;
}

/* The words of [s, e) that fit in w, broken at a space where it can be */
static int fits(const char *t, int s, int e, int font, int px, int w)
{
    char buf[1024];
    int n = e - s < (int)sizeof(buf) - 1 ? e - s : (int)sizeof(buf) - 1;
    memcpy(buf, t + s, (size_t)n);
    buf[n] = 0;
    if (ud_width(buf, font, px) <= w)
        return s + n;
    int good = -1;
    for (int k = 1; k <= n; k++) {
        if (k < n && ((unsigned char)buf[k] & 0xC0) == 0x80)
            continue;
        char c = buf[k];
        buf[k] = 0;
        int wd = ud_width(buf, font, px);
        buf[k] = c;
        if (wd > w)
            break;
        if (c == ' ' || k == n)
            good = k;
    }
    if (good <= 0) {
        /* one word wider than the line: as much of it as fits */
        int k = 1;
        while (k < n) {
            char c = buf[k];
            buf[k] = 0;
            int wd = ud_width(buf, font, px);
            buf[k] = c;
            if (wd > w)
                break;
            k++;
        }
        good = k > 1 ? k - 1 : 1;
    }
    return s + good + (buf[good] == ' ' ? 1 : 0);
}

static void lay(void)
{
    npieces = 0;
    int y = ART_Y + DATE_PX + BLOCK_GAP;
    int x0 = art_x(), w = art_w();
    int prev_item = 0;
    for (int i = 0; i < nlines && npieces < PIECES_MAX - 1; i++) {
        const char *t = line[i];
        int len = (int)strlen(t);
        int kind = i == 0 ? P_TITLE : item(t) ? P_ITEM : P_PARA;
        if (kind == P_PARA && !len) {
            y += BLOCK_GAP;
            prev_item = 0;
            struct piece *p = &pieces[npieces++];
            *p = (struct piece){ i, 0, 0, x0, y, UD_SERIF, PARA_PX, P_PARA };
            continue;
        }
        if (kind == P_ITEM && prev_item)
            y += ITEM_GAP;
        else if (i > 0 && kind == P_PARA && prev_item)
            y += BLOCK_GAP / 2;
        int font = kind == P_TITLE ? UD_SERIF_BOLD : UD_SERIF;
        int px = kind == P_TITLE ? TITLE_PX : kind == P_ITEM ? ITEM_PX
                                                             : PARA_PX;
        int step = kind == P_TITLE ? TITLE_LINE : kind == P_ITEM ? ITEM_H
                                                                 : PARA_LINE;
        int s = kind == P_ITEM ? 4 : 0;
        int lx = kind == P_ITEM ? x0 + KNOT + KNOT_GAP : x0;
        int lw = kind == P_ITEM ? w - KNOT - KNOT_GAP : w;
        do {
            int e = s < len ? fits(t, s, len, font, px, lw) : len;
            struct piece *p = &pieces[npieces++];
            int base = y + (step + px * 73 / 100) / 2;
            *p = (struct piece){ i, s, e, lx, base, font, px, kind };
            y += step;
            s = e;
        } while (s < len && npieces < PIECES_MAX - 1);
        if (kind == P_TITLE)
            y += BLOCK_GAP;
        prev_item = kind == P_ITEM;
    }
    doc_h = y;
}

static int text_w(const char *t, int s, int e, int font, int px)
{
    char buf[1024];
    int n = e - s < (int)sizeof(buf) - 1 ? e - s : (int)sizeof(buf) - 1;
    memcpy(buf, t + s, (size_t)n);
    buf[n] = 0;
    return ud_width(buf, font, px);
}

static int caret_piece(void)
{
    int best = -1;
    for (int k = 0; k < npieces; k++)
        if (pieces[k].l == cy && pieces[k].s <= cx)
            best = k;
    return best;
}

/* --- drawing ------------------------------------------------------ */

static int weave(void)
{
    return L.style == UD_STYLE_WEAVE;
}

static void date_words(long long mtime, char *out, size_t cap)
{
    static const char *const day[7] = { "Sunday", "Monday", "Tuesday",
        "Wednesday", "Thursday", "Friday", "Saturday" };
    static const char *const mon[12] = { "January", "February", "March",
        "April", "May", "June", "July", "August", "September", "October",
        "November", "December" };
    time_t t = (time_t)mtime;
    struct tm tm;
    gmtime_r(&t, &tm);
    snprintf(out, cap, "%s, %d %s \xC2\xB7 in Notes", day[tm.tm_wday],
             tm.tm_mday, mon[tm.tm_mon]);
}

static const char *fit_end(const char *s, int font, int px, int w, char *buf,
                           int cap)
{
    if (ud_width(s, font, px) <= w)
        return s;
    int n = (int)strlen(s);
    if (n > cap - 4)
        n = cap - 4;
    while (n > 0) {
        memcpy(buf, s, (size_t)n);
        while (n > 0 && ((unsigned char)buf[n] & 0xC0) == 0x80)
            n--;
        memcpy(buf + n, "\xE2\x80\xA6", 4);
        if (ud_width(buf, font, px) <= w)
            return buf;
        n--;
    }
    return "";
}

static void draw_nav(void)
{
    uint32_t side = weave() ? (L.dark ? 0x17191F : 0xF6F3EC) : L.field;
    ud_fill(win, 0, 0, NAV_W, ch, side);
    ud_fill(win, NAV_W, 0, 1, ch, weave() ? (L.dark ? PU_N_CARD_LINE
                                                    : PU_L_CARD_LINE) : L.line);
    /* New note: a dashed row, the way to begin one */
    int x = NAV_PAD, y = NAV_PAD, w = NAV_W - 2 * NAV_PAD;
    uint32_t dash = L.dark ? 0x4A4F5C : 0xCFC8BA;
    ud_ring(win, x, y, w, NEW_H, 9, dash);
    for (int k = x + 12; k < x + w - 12; k += 6) {
        ud_fill(win, k, y, 3, 1, side);
        ud_fill(win, k, y + NEW_H - 1, 3, 1, side);
    }
    ud_line(win, x + 18, y + NEW_H / 2, x + 28, y + NEW_H / 2, 28, L.accent);
    ud_line(win, x + 23, y + NEW_H / 2 - 5, x + 23, y + NEW_H / 2 + 5, 28,
            L.accent);
    ud_text(win, x + 38, ud_baseline(y, NEW_H, NOTE_PX), "New note", L.dim,
            UD_BOLD, NOTE_PX);
    y += NEW_H + NOTE_GAP + 4;
    char cut[128];
    for (int i = list_top; i < nnotes && y < ch; i++) {
        struct note *n = &notes[i];
        if (i == cur)
            ud_round(win, x, y, w, NOTE_H, 9, mix(L.accent, side, 31));
        ud_round(win, x + 10, y + 13, 8, 8, 2, dye_of(n->title));
        ud_text(win, x + 24, y + 22,
                fit_end(n->title, UD_BOLD, NOTE_PX, w - 34, cut, sizeof(cut)),
                L.text, UD_BOLD, NOTE_PX);
        ud_text(win, x + 10, y + 40,
                fit_end(n->first, UD_SANS, FIRST_PX, w - 20, cut, sizeof(cut)),
                L.dim, UD_SANS, FIRST_PX);
        y += NOTE_H + NOTE_GAP;
    }
}

static void draw_note(void)
{
    int x0 = NAV_W + 1;
    ud_fill(win, x0, 0, cw - x0, ch, weave() ? (L.dark ? PU_N_CARD : 0xFAF8F4)
                                             : L.bg);
    if (cur < 0) {
        ud_text(win, art_x(), ART_Y + 20, "No notes yet - Ctrl+N begins one",
                L.dim, UD_SANS, 14);
        return;
    }
    lay();
    int bottom = ch - FOOT_H - 20;
    /* the caret in sight */
    int cp = caret_piece();
    if (cp >= 0) {
        struct piece *p = &pieces[cp];
        if (p->y - scroll > bottom)
            scroll = p->y - bottom + 8;
        if (p->y - p->px - scroll < ART_Y)
            scroll = p->y - p->px - ART_Y;
        if (scroll < 0)
            scroll = 0;
    }
    ud_clip(win, x0, 0, cw - x0, bottom + 8);
    char date[96];
    date_words(notes[cur].mtime, date, sizeof(date));
    ud_text(win, art_x(), ART_Y + DATE_PX - scroll, date, L.dim, UD_SANS,
            DATE_PX);
    uint32_t done_ink = L.dark ? 0x6A7080 : 0x8A857B;
    char buf[1024];
    for (int k = 0; k < npieces; k++) {
        struct piece *p = &pieces[k];
        int y = p->y - scroll;
        if (y < -40 || y > bottom + 40)
            continue;
        const char *t = line[p->l];
        int it = p->kind == P_ITEM ? item(t) : 0;
        if (it && p->s == 4) {
            int ky = y - p->px * 73 / 200 - KNOT / 2;
            int kx = art_x();
            ud_disc(win, kx, ky, KNOT, L.accent);
            if (it == 1)
                ud_disc(win, kx + 2, ky + 2, KNOT - 4,
                        weave() ? (L.dark ? PU_N_CARD : 0xFAF8F4) : L.bg);
        }
        int n = p->e - p->s < (int)sizeof(buf) - 1 ? p->e - p->s
                                                    : (int)sizeof(buf) - 1;
        memcpy(buf, t + p->s, (size_t)n);
        buf[n] = 0;
        uint32_t ink = it == 2 ? done_ink : p->kind == P_PARA
                       ? (L.dark ? 0xD4D6DE : 0x2B2A27) : L.text;
        ud_text(win, p->x, y, buf, ink, p->font, p->px);
        if (it == 2 && n) {
            /* ticked off: struck through */
            int tw = ud_width(buf, p->font, p->px);
            ud_fill(win, p->x, y - p->px * 30 / 100, tw, 1, done_ink);
        }
    }
    /* the caret */
    if (cp >= 0) {
        struct piece *p = &pieces[cp];
        int at = cx < p->e || p->e == p->s ? cx : p->e;
        int x = p->x + text_w(line[p->l], p->s, at, p->font, p->px);
        ud_fill(win, x, p->y - scroll - p->px * 80 / 100, 2, p->px + 4,
                L.accent);
    }
    ud_unclip(win);
    /* the foot: whether it is kept */
    const char *st = dirty ? "Editing" : "Saved";
    int sw = ud_width(st, UD_SANS, 12) + 20;
    int fy = ch - FOOT_H - 14;
    uint32_t dash = L.dark ? 0x4A4F5C : 0xCFC8BA;
    uint32_t bg = weave() ? (L.dark ? PU_N_CARD : 0xFAF8F4) : L.bg;
    ud_ring(win, art_x(), fy, sw, FOOT_H, FOOT_H / 2, dash);
    for (int k = art_x() + FOOT_H / 2; k < art_x() + sw - FOOT_H / 2; k += 6) {
        ud_fill(win, k, fy, 3, 1, bg);
        ud_fill(win, k, fy + FOOT_H - 1, 3, 1, bg);
    }
    ud_text_mid(win, art_x(), fy, sw, FOOT_H, st, L.dim, UD_SANS, 12);
}

static void draw(void)
{
    ud_look(&L);
    ud_hold(win, 1);
    draw_nav();
    draw_note();
    ug_modal_draw(&ui, &ask);
    ud_hold(win, 0);
}

/* --- clicks ------------------------------------------------------- */

static void click(int x, int y)
{
    if (x < NAV_W) {
        int ty = NAV_PAD;
        if (y >= ty && y < ty + NEW_H) {
            new_note();
            return;
        }
        ty += NEW_H + NOTE_GAP + 4;
        for (int i = list_top; i < nnotes; i++, ty += NOTE_H + NOTE_GAP)
            if (y >= ty && y < ty + NOTE_H) {
                if (i != cur)
                    open_note(i);
                return;
            }
        return;
    }
    if (cur < 0)
        return;
    lay();
    /* a knot: ticked, or not */
    for (int k = 0; k < npieces; k++) {
        struct piece *p = &pieces[k];
        if (p->kind != P_ITEM || p->s != 4)
            continue;
        int ky = p->y - scroll - p->px * 73 / 200 - KNOT / 2;
        if (x >= art_x() - 4 && x < art_x() + KNOT + 4 && y >= ky - 4 &&
            y < ky + KNOT + 4) {
            tick(p->l);
            return;
        }
    }
    /* the caret where the click was: the piece on that row, the letter
       nearest */
    int best = -1, bd = 1 << 30;
    for (int k = 0; k < npieces; k++) {
        int py = pieces[k].y - scroll - pieces[k].px / 2;
        int dd = py > y ? py - y : y - py;
        if (dd < bd) {
            bd = dd;
            best = k;
        }
    }
    if (best < 0)
        return;
    struct piece *p = &pieces[best];
    const char *t = line[p->l];
    int at = p->s;
    while (at < p->e) {
        int nx = at + 1;
        while (nx < p->e && ((unsigned char)t[nx] & 0xC0) == 0x80)
            nx++;
        int mid = p->x + (text_w(t, p->s, at, p->font, p->px) +
                          text_w(t, p->s, nx, p->font, p->px)) / 2;
        if (x < mid)
            break;
        at = nx;
    }
    cy = p->l;
    cx = at;
}

/* --- keys --------------------------------------------------------- */

/* Ctrl with a letter comes as the letter with WEV_MOD_CTRL - but for
   the few the keyboard gives codes of their own (kernel/kbdmap.c):
   Ctrl+S is KEY_SAVE, Ctrl+D the end of the input */
#define KEY_EOF 0x04

static void key(int c, int mods)
{
    int ctrl = mods & WEV_MOD_CTRL;
    if (ctrl && (c == 'n' || c == 'N')) {
        new_note();
        return;
    }
    if (cur < 0)
        return;
    if (c == KEY_SAVE) {
        dirty = 1;
        save();
    } else if (ctrl && (c == 'l' || c == 'L')) {
        toggle_item_line(cy);
    } else if (c == KEY_EOF) {
        pu_sheet(&ask, "Delete this note?",
                 "It is gone from ~/Notes - Mend's copies still have it.",
                 "Delete", "Keep it", 1);
    } else if (ctrl) {
        return;                       /* no letter for the other chords */
    } else if (c == '\n' || c == '\r') {
        split();
    } else if (c == '\b' || c == 127) {
        back();
    } else if (c == KEY_LEFT) {
        if (cx > 0) {
            cx--;
            while (cx > 0 && ((unsigned char)line[cy][cx] & 0xC0) == 0x80)
                cx--;
        } else if (cy > 0) {
            cy--;
            cx = (int)strlen(line[cy]);
        }
    } else if (c == KEY_RIGHT) {
        int len = (int)strlen(line[cy]);
        if (cx < len) {
            cx++;
            while (cx < len && ((unsigned char)line[cy][cx] & 0xC0) == 0x80)
                cx++;
        } else if (cy + 1 < nlines) {
            cy++;
            cx = 0;
        }
    } else if (c == KEY_UP || c == KEY_DOWN) {
        int ny = cy + (c == KEY_UP ? -1 : 1);
        if (ny >= 0 && ny < nlines) {
            cy = ny;
            int len = (int)strlen(line[cy]);
            if (cx > len)
                cx = len;
        }
    } else if (c == KEY_HOME) {
        cx = cy > 0 && item(line[cy]) ? 4 : 0;
    } else if (c == KEY_END) {
        cx = (int)strlen(line[cy]);
    } else if (c == '\t') {
        insert("    ", 4);
    } else if (c >= 32 && c < 127) {
        char s[1] = { (char)c };
        insert(s, 1);
    } else if (c >= 0xA0 && c < 0x100) {
        /* a Latin-1 letter from the keyboard, as UTF-8 */
        char s[2] = { (char)(0xC0 | (c >> 6)), (char)(0x80 | (c & 0x3F)) };
        insert(s, 2);
    }
}

int main(void)
{
    char home[256];
    if (!getcwd(home, sizeof(home)))
        strcpy(home, "/");
    snprintf(dir, sizeof(dir), "%s/Notes", strcmp(home, "/") ? home : "");
    mkdir(dir, 0755);
    static const int sz[2][2] = { { W1, H1 }, { W0, H0 } };
    for (int i = 0; i < 2 && win < 0; i++) {
        win = (int)win_create(sz[i][0], sz[i][1], "Notes");
        cw = sz[i][0];
        ch = sz[i][1];
    }
    if (win < 0)
        return 1;
    win_truecolour(win);
    pu_start(&ui, win, cw, ch, ui_room, 4);
    scan();
    list_say();
    if (nnotes)
        open_note(0);
    draw();
    printf("[notes] drawn %d %d\n", cw, ch);
    fflush(stdout);
    for (;;) {
        struct win_event ev;
        int redraw = 0;
        while (win_poll(win, &ev) == 1) {
            redraw = 1;
            if (ev.type == WEV_CLOSE) {
                save();
                return 0;
            }
            if (ask.on) {
                if (ug_modal_event(&ui, &ask, &ev) == UG_PRESSED &&
                    ask.answer == 1)
                    delete_note();
                continue;
            }
            if (ev.type == WEV_SIZE && ev.x > 0 && ev.y > 0) {
                cw = ev.x;
                ch = ev.y;
                pu_start(&ui, win, cw, ch, ui_room, 4);
            } else if (ev.type == WEV_KEY) {
                key(ev.val & 0xFF, ev.x);
            } else if (ev.type == WEV_CLICK) {
                click(ev.x, ev.y);
            } else if (ev.type == WEV_SCROLL) {
                if (ev.x < NAV_W) {
                    list_top -= ev.val;
                    if (list_top > nnotes - 1)
                        list_top = nnotes - 1;
                    if (list_top < 0)
                        list_top = 0;
                }
            }
        }
        /* kept a moment after the typing stops */
        if (dirty && cur >= 0 && now_ms() - typed_at > 1500) {
            save();
            redraw = 1;
        }
        if (redraw)
            draw();
        sys_sleep(30);
    }
}
