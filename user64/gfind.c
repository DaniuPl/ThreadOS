/* gfind: Find - one field for everything on the machine
 *
 * docs/weave/canvas/Find.dc.html: a large field, under it where to look
 * - everything, files by name, words inside files, Settings, programs,
 * Weft's history - each with how many it found, and when and where it
 * looks; the results down the left in their groups, the chosen one on
 * the right opened where the word is, its lines with the word marked,
 * and Open in Code and Show in Files. Up and down move from one match
 * to the next, through a file's matches and on to the next result;
 * Enter opens the file at the one you are on.
 *
 * Started as `gfind [WORDS]`, it looks for WORDS at once. What it does
 * is said on the serial port, for scripts/tests/t_find.py:
 *
 *   [find] drawn W H
 *   [find] searched WORDS: N found - files A, inside B, settings C,
 *          programs D, web E
 *   [find] filter NAME
 *   [find] at NAME:LINE          (or NAME, for a result with no line)
 *   [find] changed WHEN | [find] in WHERE
 *   [find] open code PATH LINE | files DIR | settings SECTION |
 *          program NAME | weft URI
 *
 * Built against musl: it walks folders of any depth and reads files of
 * any length.
 */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "ugui.h"

/* --- where things are (scripts/tests/t_find.py reads these) ------------ */

#define W1 1000
#define H1 620
#define W0 760
#define H0 470
#define SIDE 22               /* the top block's sides */
#define FIELD_Y 18
#define FIELD_H 52
#define FIELD_PX 18
#define FIELD_TEXT_X 48       /* the words, after the magnifier */
#define COUNT_PX 12           /* "14 found in 0.04 s" */
#define PILL_Y 84             /* FIELD_Y + FIELD_H + 14 */
#define PILL_H 32
#define PILL_GAP 8
#define PILL_PX 13
#define PILL_PAD 12
#define PILL_DOT 8
#define TOP_PAD 12            /* under the pills, down to the stitch */
#define LIST_PCT 42           /* the list's share of the width (540/1300) */
#define LIST_PAD 14
#define LIST_TOP 12
#define HEAD_H 20             /* a group's heading */
#define HEAD_PX 11
#define ROW_H 48
#define ROW_TILE 32
#define ROW_NAME_PX 14
#define ROW_WHERE_PX 12
#define GROUP_GAP 14
#define PREV_PAD 24
#define PREV_TOP 18
#define PREV_TILE 40
#define PREV_NAME_PX 16
#define PREV_SUB_PX 12
#define BTN_H 34
#define BTN_PX 13
#define CODE_GAP 14           /* between the heading and the lines */
#define CODE_LINE 22
#define CODE_PX 13
#define NUM_W 52
#define HINT_PX 12

/* Night's field, where Linen's is white: the lines' box and the field
   (look.h's NIGHT_APP_FIELD) */
#define FIND_N_FIELD 0x121419

/* --- what is found -------------------------------------------------- */

#define K_FILE    0
#define K_INSIDE  1
#define K_SETTING 2
#define K_PROGRAM 3
#define K_WEB     4
#define KINDS     5

static const char *const kind_name[KINDS] = {
    "Files", "Inside files", "Settings", "Programs", "Weft history"
};
/* the design's dyes for each, the same in both themes - they are tiles
   and dots, each on its own ground */
static const uint32_t kind_dye[KINDS] = {
    0x4A6BE0, 0x1E8A4C, 0x6B675F, 0xC2366F, 0x1E7F86
};

#define HIT_MAX   400
#define MATCH_MAX 64
struct hit {
    int kind;
    char name[96];
    char where[240];          /* the folder, the section, the address */
    char path[512];           /* what opens it: a file, a section, a
                                 program, an address */
    char quote[96];           /* the first line the words are in */
    int matches;              /* how many times, inside a file */
    int nlines;
    int line[MATCH_MAX];      /* the lines they are on, from 1 */
    long long mtime;
};
static struct hit *hits;
static int nhit;
static int count[KINDS];

/* Settings' rows, by the words they show and the section they are in -
   user/gsettings.c's own labels (t_find holds each to that file), so a
   search for "pointer" finds where the pointer's speed is set */
static const struct { const char *label, *section; } settings[] = {
    { "Wi-Fi", "Wi-Fi" }, { "Bluetooth", "Bluetooth" },
    { "Network", "Network" }, { "Sound", "Sound" },
    { "General", "General" }, { "Appearance", "Appearance" },
    { "Desktop & Dock", "Desktop & Dock" }, { "Display", "Display" },
    { "Screen Lock", "Screen Lock" }, { "Mouse", "Mouse" },
    { "Files", "Files" }, { "Region", "Region" }, { "About", "About" },
    { "Devices", "Devices" }, { "Updates", "Updates" },
    { "Search for devices", "Bluetooth" }, { "Paired", "Bluetooth" },
    { "Gateway", "Network" }, { "Name server", "Network" },
    { "Volume", "Sound" }, { "Mute", "Sound" }, { "Play a tone", "Sound" },
    { "Computer name", "General" },
    { "Windows opening and closing", "General" },
    { "Colour of the shell prompt", "General" },
    { "Theme", "Appearance" }, { "Background", "Appearance" },
    { "Size of the dock", "Appearance" },
    { "Wallpaper file", "Appearance" },
    { "Interface style", "Desktop & Dock" },
    { "Position on screen", "Desktop & Dock" },
    { "Magnify the icons under the pointer", "Desktop & Dock" },
    { "Hide the menu bar when it is not used", "Desktop & Dock" },
    { "Scale", "Display" }, { "Resolution", "Display" },
    { "Turn the screen off when inactive for", "Screen Lock" },
    { "Dim the screen before it turns off", "Screen Lock" },
    { "Lock the screen after it turns off", "Screen Lock" },
    { "Unlock with", "Screen Lock" }, { "New PIN", "Screen Lock" },
    { "Pointer speed", "Mouse" }, { "Natural scrolling", "Mouse" },
    { "Date order", "Region" }, { "Numbers", "Region" },
    { "Language", "Region" },
    { "Installed version", "Updates" }, { "Check for updates", "Updates" },
    { "Restore previous system", "Updates" },
};
#define SETTINGS_N ((int)(sizeof(settings) / sizeof(settings[0])))

/* The programs a person starts, by the names the desktop gives them
   (kernel/desktop.c's known_apps) */
static const struct { const char *label, *prog; } programs[] = {
    { "Clock", "gclock" }, { "Files", "gfiles" }, { "Monitor", "gmon" },
    { "Terminal", "gterm" }, { "Web", "web" }, { "Settings", "gsettings" },
    { "Editor", "gedit" }, { "Tasks", "gtasks" }, { "Mixer", "gmikser" },
    { "Appearance", "gappear" }, { "Calculator", "gcalc" },
    { "Calendar", "gcal" }, { "Paint", "gpaint" }, { "Code", "gcode" },
    { "Accounts", "gusers" }, { "Gallery", "ggallery" },
    { "Camera", "gcamera" }, { "Recorder", "grecord" },
    { "Swatch", "gswatch" }, { "Find", "gfind" }, { "Notes", "gnotes" },
    { "Mend", "gmend" }, { "Skein", "gskein" },
};
#define PROGRAMS_N ((int)(sizeof(programs) / sizeof(programs[0])))

/* --- the window's state ---------------------------------------------- */

static int win = -1, cw, ch;
static struct ud_look L;
static char query[96];        /* what is typed */
static char searched[96];     /* what the results are for */
static int filter;            /* 0 everything, then 1 + K_* */
static int when;              /* 0 any time, 1 today, 2 this week, 3 this month */
static int scope;             /* 0 everywhere (/), 1 the home */
static int sel = -1, mi;      /* the result chosen and its match */
static int list_top;          /* the list's first row shown */
static long long took_ms;
static int stopped;           /* the walk reached its limit */
static char home[256];

static const char *const when_word[4] = {
    "any time", "today", "this week", "this month"
};
static const char *const scope_word[2] = { "everywhere", "home" };

/* What the chosen file holds, for its lines on the right */
static char *shown;
static int shown_len;
static char shown_path[512];

static uint32_t mix(uint32_t a, uint32_t b, int alpha)
{
    uint32_t r = (((a >> 16) & 255) * alpha + ((b >> 16) & 255) * (255 - alpha)) / 255;
    uint32_t g = (((a >> 8) & 255) * alpha + ((b >> 8) & 255) * (255 - alpha)) / 255;
    uint32_t bl = ((a & 255) * alpha + (b & 255) * (255 - alpha)) / 255;
    return r << 16 | g << 8 | bl;
}

static void say(const char *fmt, const char *a, const char *b)
{
    printf("[find] ");
    printf(fmt, a, b);
    printf("\n");
    fflush(stdout);
}

static int lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

/* Where 'q' is in 'h', without regard to case, from 'from' - or -1 */
static int find_in(const char *h, int hlen, const char *q, int from)
{
    int ql = (int)strlen(q);
    if (!ql)
        return -1;
    for (int i = from; i + ql <= hlen; i++) {
        int k = 0;
        while (k < ql && lower((unsigned char)h[i + k]) ==
                         lower((unsigned char)q[k]))
            k++;
        if (k == ql)
            return i;
    }
    return -1;
}

static int has(const char *h, const char *q)
{
    return find_in(h, (int)strlen(h), q, 0) >= 0;
}

static struct hit *add(int kind)
{
    if (nhit == HIT_MAX)
        return NULL;
    struct hit *h = &hits[nhit++];
    memset(h, 0, sizeof(*h));
    h->kind = kind;
    count[kind]++;
    return h;
}

/* A folder as the design writes it: from the home, its parts apart */
static void where_of(const char *dir, char *out, size_t cap)
{
    const char *p = dir;
    size_t hl = strlen(home);
    if (hl > 1 && !strncmp(dir, home, hl) && (dir[hl] == '/' || !dir[hl]))
        p = dir[hl] ? dir + hl + 1 : "";
    size_t n = 0;
    if (!*p) {
        snprintf(out, cap, "%s", p == dir ? "/" : "Home");
        return;
    }
    for (; *p && n + 4 < cap; p++) {
        if (*p == '/' && p != dir) {
            out[n++] = ' ';
            out[n++] = '/';
            out[n++] = ' ';
        } else if (*p != '/') {
            out[n++] = *p;
        }
    }
    out[n] = 0;
}

static int too_old(long long mtime)
{
    static const int days[4] = { 0, 1, 7, 31 };
    if (!when)
        return 0;
    long long now = (long long)time(NULL);
    if (when == 1)
        return mtime / 86400 != now / 86400;
    return now - mtime > (long long)days[when] * 86400;
}

/* Words inside a file: the lines they are on and how often */
static void look_inside(const char *path, const char *name, const char *dir,
                        long long size, long long mtime, long *budget)
{
    if (size <= 0 || size > 256 * 1024 || *budget < size)
        return;
    FILE *f = fopen(path, "rb");
    if (!f)
        return;
    char *t = malloc((size_t)size + 1);
    if (!t) {
        fclose(f);
        return;
    }
    int n = (int)fread(t, 1, (size_t)size, f);
    fclose(f);
    *budget -= n;
    t[n] = 0;
    /* text only: a NUL in its first part says it is not */
    for (int i = 0; i < n && i < 1024; i++)
        if (!t[i]) {
            free(t);
            return;
        }
    int at = find_in(t, n, searched, 0);
    if (at < 0) {
        free(t);
        return;
    }
    struct hit *h = add(K_INSIDE);
    if (!h) {
        free(t);
        return;
    }
    snprintf(h->name, sizeof(h->name), "%s", name);
    snprintf(h->path, sizeof(h->path), "%s", path);
    h->mtime = mtime;
    int line = 1, last_line = 0;
    for (int i = 0, pos = 0; at >= 0; ) {
        for (; pos < at; pos++)
            if (t[pos] == '\n')
                line++;
        h->matches++;
        if (line != last_line && h->nlines < MATCH_MAX)
            h->line[h->nlines++] = line;
        if (!last_line) {
            int s = at, e = at;
            while (s > 0 && t[s - 1] != '\n')
                s--;
            while (e < n && t[e] != '\n')
                e++;
            while (s < e && (t[s] == ' ' || t[s] == '\t'))
                s++;
            int len = e - s < (int)sizeof(h->quote) - 1
                          ? e - s : (int)sizeof(h->quote) - 1;
            memcpy(h->quote, t + s, (size_t)len);
            h->quote[len] = 0;
        }
        last_line = line;
        i = at + (int)strlen(searched);
        at = find_in(t, n, searched, i);
    }
    char folder[200];
    where_of(dir, folder, sizeof(folder));
    snprintf(h->where, sizeof(h->where), "%.150s \xC2\xB7 \"%.80s\"",
             folder, h->quote);
    free(t);
}

/* The walk: folders under 'root', by name and by what is inside. A
   limit on how many it looks at and on how much it reads keeps a disk
   with a compiler on it from holding the answer back for minutes. */
#define WALK_FILES  6000
#define WALK_BYTES  (32L * 1024 * 1024)
#define WALK_DEPTH  16

static int walked;

static void walk(const char *dir, int depth, long *budget)
{
    if (depth > WALK_DEPTH || walked >= WALK_FILES)
        return;
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)) && walked < WALK_FILES) {
        const char *nm = e->d_name;
        /* not what the system keeps for itself: the dot files and
           folders, Code's and the Trash's among them */
        if (nm[0] == '.')
            continue;
        char path[512];
        if (snprintf(path, sizeof(path), "%s%s%s", dir,
                     dir[strlen(dir) - 1] == '/' ? "" : "/", nm) >=
            (int)sizeof(path))
            continue;
        struct stat st;
        if (stat(path, &st) < 0)
            continue;
        walked++;
        if (S_ISDIR(st.st_mode)) {
            walk(path, depth + 1, budget);
            continue;
        }
        if (!S_ISREG(st.st_mode) || too_old((long long)st.st_mtime))
            continue;
        if (has(nm, searched)) {
            struct hit *h = add(K_FILE);
            if (h) {
                snprintf(h->name, sizeof(h->name), "%s", nm);
                snprintf(h->path, sizeof(h->path), "%s", path);
                where_of(dir, h->where, sizeof(h->where));
                h->mtime = (long long)st.st_mtime;
            }
        }
        look_inside(path, nm, dir, (long long)st.st_size,
                    (long long)st.st_mtime, budget);
    }
    if (walked >= WALK_FILES)
        stopped = 1;
    closedir(d);
}

/* Weft's history: ~/.weft's "history <address> <title>" lines */
static void look_web(void)
{
    char path[300];
    snprintf(path, sizeof(path), "%s/.weft", home);
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char ln[1024];
    while (fgets(ln, sizeof(ln), f)) {
        if (strncmp(ln, "history\t", 8))
            continue;
        char *uri = ln + 8, *title = strchr(uri, '\t');
        if (!title)
            continue;
        *title++ = 0;
        title[strcspn(title, "\r\n")] = 0;
        if (!has(uri, searched) && !has(title, searched))
            continue;
        struct hit *h = add(K_WEB);
        if (!h)
            break;
        snprintf(h->name, sizeof(h->name), "%s", title[0] ? title : uri);
        snprintf(h->where, sizeof(h->where), "%s", uri);
        snprintf(h->path, sizeof(h->path), "%s", uri);
    }
    fclose(f);
}

static void search(void)
{
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    snprintf(searched, sizeof(searched), "%s", query);
    nhit = 0;
    memset(count, 0, sizeof(count));
    stopped = 0;
    walked = 0;
    if (searched[0]) {
        long budget = WALK_BYTES;
        walk(scope ? home : "/", 0, &budget);
        for (int i = 0; i < SETTINGS_N; i++) {
            if (!has(settings[i].label, searched) &&
                !has(settings[i].section, searched))
                continue;
            struct hit *h = add(K_SETTING);
            if (!h)
                break;
            snprintf(h->name, sizeof(h->name), "%s", settings[i].label);
            snprintf(h->where, sizeof(h->where), "Settings, %s",
                     settings[i].section);
            snprintf(h->path, sizeof(h->path), "%s", settings[i].section);
        }
        for (int i = 0; i < PROGRAMS_N; i++) {
            if (!has(programs[i].label, searched) &&
                !has(programs[i].prog, searched))
                continue;
            struct hit *h = add(K_PROGRAM);
            if (!h)
                break;
            snprintf(h->name, sizeof(h->name), "%s", programs[i].label);
            snprintf(h->where, sizeof(h->where), "A program \xC2\xB7 %s",
                     programs[i].prog);
            snprintf(h->path, sizeof(h->path), "%s", programs[i].prog);
        }
        look_web();
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    took_ms = (long long)(t1.tv_sec - t0.tv_sec) * 1000 +
              (t1.tv_nsec - t0.tv_nsec) / 1000000;
    printf("[find] searched %s: %d found - files %d, inside %d, settings %d, "
           "programs %d, web %d\n", searched, nhit, count[K_FILE],
           count[K_INSIDE], count[K_SETTING], count[K_PROGRAM], count[K_WEB]);
    fflush(stdout);
}

/* --- the rows of the list ------------------------------------------- */

/* A row is a group's heading (-1 - kind) or a result (its index) */
#define ROWS_MAX (HIT_MAX + KINDS)
static int rows[ROWS_MAX], nrows;

static int shows(int kind)
{
    return filter == 0 || filter == kind + 1;
}

static void lay_rows(void)
{
    nrows = 0;
    for (int k = 0; k < KINDS; k++) {
        if (!shows(k) || !count[k])
            continue;
        rows[nrows++] = -1 - k;
        for (int i = 0; i < nhit; i++)
            if (hits[i].kind == k)
                rows[nrows++] = i;
    }
}

static int row_h(int r)
{
    if (rows[r] >= 0)
        return ROW_H;
    return HEAD_H + (r ? GROUP_GAP : 0);
}

static int first_hit(void)
{
    for (int r = 0; r < nrows; r++)
        if (rows[r] >= 0)
            return rows[r];
    return -1;
}

/* --- the chosen one ----------------------------------------------- */

static void load_shown(void)
{
    if (sel < 0 || (hits[sel].kind != K_FILE && hits[sel].kind != K_INSIDE)) {
        free(shown);
        shown = NULL;
        shown_len = 0;
        shown_path[0] = 0;
        return;
    }
    if (!strcmp(shown_path, hits[sel].path))
        return;
    free(shown);
    shown = NULL;
    shown_len = 0;
    snprintf(shown_path, sizeof(shown_path), "%s", hits[sel].path);
    FILE *f = fopen(shown_path, "rb");
    if (!f)
        return;
    shown = malloc(256 * 1024 + 1);
    if (shown) {
        shown_len = (int)fread(shown, 1, 256 * 1024, f);
        shown[shown_len] = 0;
        for (int i = 0; i < shown_len && i < 1024; i++)
            if (!shown[i]) {
                shown_len = -1;       /* not text */
                break;
            }
    }
    fclose(f);
}

static int cur_line(void)
{
    if (sel < 0 || hits[sel].kind != K_INSIDE || !hits[sel].nlines)
        return 0;
    return hits[sel].line[mi < hits[sel].nlines ? mi : 0];
}

static void say_at(void)
{
    if (sel < 0)
        return;
    char ln[16] = "";
    if (cur_line())
        snprintf(ln, sizeof(ln), ":%d", cur_line());
    say("at %s%s", hits[sel].name, ln);
}

static void choose(int i)
{
    sel = i;
    mi = 0;
    load_shown();
    say_at();
}

/* --- drawing ------------------------------------------------------ */

static int weave(void)
{
    return L.style == UD_STYLE_WEAVE;
}

static uint32_t card(void)
{
    return weave() ? (L.dark ? PU_N_CARD : PU_L_CARD) : L.field;
}

static uint32_t edge(void)
{
    return weave() ? (L.dark ? PU_N_RAISED_LINE : 0xDDD6C9) : L.line;
}

static int list_w(void)
{
    return cw * LIST_PCT / 100;
}

/* Words cut to a width, with an ellipsis */
static const char *fit(const char *s, int font, int px, int w, char *buf,
                       int cap)
{
    if (ud_width(s, font, px) <= w)
        return s;
    int n = (int)strlen(s);
    if (n > cap - 4)
        n = cap - 4;
    while (n > 0) {
        memcpy(buf, s, (size_t)n);
        /* not inside a letter of more than one byte */
        while (n > 0 && ((unsigned char)buf[n] & 0xC0) == 0x80)
            n--;
        memcpy(buf + n, "\xE2\x80\xA6", 4);
        if (ud_width(buf, font, px) <= w)
            return buf;
        n--;
    }
    return "";
}

/* The pills: where to look, and when and where it looks */
#define PILLS (KINDS + 1)
struct pill {
    int x, y, w;
};
static struct pill pill[PILLS + 2];
static int top_h;

static const char *pill_name(int i)
{
    return i ? kind_name[i - 1] : "Everything";
}

static int pill_count(int i)
{
    return i ? count[i - 1] : nhit;
}

static void lay_pills(void)
{
    int x = SIDE, y = PILL_Y, right = cw - SIDE;
    char num[12], b[48];
    for (int i = 0; i < PILLS; i++) {
        snprintf(num, sizeof(num), "%d", pill_count(i));
        int w = PILL_PAD + PILL_DOT + 8 + ud_width(pill_name(i), UD_BOLD, PILL_PX) +
                8 + ud_width(num, UD_SANS, PILL_PX) + PILL_PAD;
        if (x + w > right && x > SIDE) {
            x = SIDE;
            y += PILL_H + PILL_GAP;
        }
        pill[i] = (struct pill){ x, y, w };
        x += w + PILL_GAP;
    }
    /* when and where, at the right of the last row - or a row of their
       own when there is no room there */
    int w2[2];
    snprintf(b, sizeof(b), "Changed: %s", when_word[when]);
    w2[0] = ud_width(b, UD_SANS, PILL_PX) + 2 * PILL_PAD;
    snprintf(b, sizeof(b), "In: %s", scope_word[scope]);
    w2[1] = ud_width(b, UD_SANS, PILL_PX) + 2 * PILL_PAD;
    if (x + w2[0] + PILL_GAP + w2[1] > right) {
        y += PILL_H + PILL_GAP;
    }
    pill[PILLS + 1] = (struct pill){ right - w2[1], y, w2[1] };
    pill[PILLS] = (struct pill){ right - w2[1] - PILL_GAP - w2[0], y, w2[0] };
    top_h = y + PILL_H + TOP_PAD;
}

static void draw_magnifier(int x, int y, uint32_t c)
{
    /* the design's: a ring of 6 and its handle, at 18 across */
    ud_ring(win, x + 4, y + 4, 12, 12, 6, c);
    ud_ring(win, x + 5, y + 5, 10, 10, 5, c);
    ud_line(win, x + 14, y + 14, x + 18, y + 18, 34, c);
}

static void draw_top(void)
{
    uint32_t bg = card();
    ud_fill(win, 0, 0, cw, top_h, bg);
    /* the field: rounder at the top, its stitch the accent */
    int fx = SIDE, fy = FIELD_Y, fw = cw - 2 * SIDE;
    uint32_t field = L.dark ? FIND_N_FIELD : 0xFFFFFF;
    ud_round(win, fx, fy, fw, FIELD_H, 12, edge());
    ud_round(win, fx, fy + FIELD_H / 2, fw, FIELD_H / 2, 5, edge());
    ud_round(win, fx + 1, fy + 1, fw - 2, FIELD_H - 2, 11, field);
    ud_round(win, fx + 1, fy + FIELD_H / 2, fw - 2, FIELD_H / 2 - 1, 4, field);
    ud_fill(win, fx + 4, fy + FIELD_H - 2, fw - 8, 2, L.accent);
    draw_magnifier(fx + 18, fy + (FIELD_H - 18) / 2, L.accent);
    int base = ud_baseline(fy, FIELD_H, FIELD_PX);
    char found[64] = "";
    if (searched[0])
        snprintf(found, sizeof(found), "%d found in %lld.%02lld s%s", nhit,
                 took_ms / 1000, (took_ms % 1000) / 10,
                 stopped ? " (stopped there)" : "");
    int fwid = ud_width(found, UD_SANS, COUNT_PX);
    ud_text(win, fx + fw - 18 - fwid, ud_baseline(fy, FIELD_H, COUNT_PX),
            found, L.dim, UD_SANS, COUNT_PX);
    char cut[128];
    int room = fw - FIELD_TEXT_X - 18 - fwid - 16;
    if (query[0]) {
        const char *t = fit(query, UD_SANS, FIELD_PX, room, cut, sizeof(cut));
        int end = fx + FIELD_TEXT_X + ud_width(t, UD_SANS, FIELD_PX);
        ud_text(win, fx + FIELD_TEXT_X, base, t, L.text, UD_SANS, FIELD_PX);
        ud_fill(win, end + 1, fy + 15, 2, FIELD_H - 30, L.accent);
    } else {
        ud_text(win, fx + FIELD_TEXT_X, base,
                fit("Files, words in them, settings, programs", UD_SANS,
                    FIELD_PX, room, cut, sizeof(cut)),
                L.dim, UD_SANS, FIELD_PX);
        ud_fill(win, fx + FIELD_TEXT_X - 2, fy + 15, 2, FIELD_H - 30,
                L.accent);
    }

    /* the pills */
    char num[12], b[48];
    uint32_t raised = weave() ? (L.dark ? PU_N_RAISED : PU_L_CARD) : L.field;
    for (int i = 0; i < PILLS; i++) {
        struct pill *p = &pill[i];
        int on = i == filter;
        uint32_t fg = on ? L.on_accent : L.text;
        ud_round(win, p->x, p->y, p->w, PILL_H, PILL_H / 2,
                 on ? L.accent : edge());
        if (!on)
            ud_round(win, p->x + 1, p->y + 1, p->w - 2, PILL_H - 2,
                     PILL_H / 2 - 1, raised);
        int x = p->x + PILL_PAD;
        ud_disc(win, x, p->y + (PILL_H - PILL_DOT) / 2, PILL_DOT,
                i ? kind_dye[i - 1] : (on ? L.on_accent : L.accent));
        x += PILL_DOT + 8;
        int pb = ud_baseline(p->y, PILL_H, PILL_PX);
        ud_text(win, x, pb, pill_name(i), fg, UD_BOLD, PILL_PX);
        x += ud_width(pill_name(i), UD_BOLD, PILL_PX) + 8;
        snprintf(num, sizeof(num), "%d", pill_count(i));
        ud_text(win, x, pb, num, mix(fg, on ? L.accent : raised, 178),
                UD_SANS, PILL_PX);
    }
    uint32_t dash = L.dark ? 0x4A4F5C : 0xCFC8BA;
    for (int i = 0; i < 2; i++) {
        struct pill *p = &pill[PILLS + i];
        ud_ring(win, p->x, p->y, p->w, PILL_H, PILL_H / 2, dash);
        /* dashed along its straight sides */
        for (int x = p->x + PILL_H / 2 + 3; x < p->x + p->w - PILL_H / 2;
             x += 6) {
            ud_fill(win, x, p->y, 3, 1, bg);
            ud_fill(win, x, p->y + PILL_H - 1, 3, 1, bg);
        }
        if (i == 0)
            snprintf(b, sizeof(b), "Changed: %s", when_word[when]);
        else
            snprintf(b, sizeof(b), "In: %s", scope_word[scope]);
        ud_text_mid(win, p->x, p->y, p->w, PILL_H, b, L.dark ? 0xB4B9C6 :
                    0x4A4740, UD_SANS, PILL_PX);
    }
    ud_stitch(win, 0, top_h - 1, cw, 0, L.dark ? 0x3A3F4C : 0xDDD6C9);
}

/* A file's tile: its kind of file, the way the design letters it */
static void glyph_of(const struct hit *h, char *g, uint32_t *dye)
{
    *dye = kind_dye[h->kind];
    if (h->kind == K_SETTING) {
        strcpy(g, "S");
        return;
    }
    if (h->kind == K_WEB) {
        strcpy(g, "W");
        return;
    }
    if (h->kind == K_PROGRAM) {
        g[0] = h->name[0];
        g[1] = 0;
        return;
    }
    const char *dot = strrchr(h->name, '.');
    if (dot && dot != h->name && strlen(dot) <= 4) {
        snprintf(g, 6, "%s", dot);
        if (!strcmp(dot, ".c") || !strcmp(dot, ".cpp") || !strcmp(dot, ".cc"))
            *dye = 0x4A6BE0;
        else if (!strcmp(dot, ".h") || !strcmp(dot, ".hpp"))
            *dye = 0x9C6FE0;
        else
            *dye = L.dark ? 0x4B505E : 0x4A4740;
    } else {
        strcpy(g, "\xC2\xB7");
        *dye = L.dark ? 0x4B505E : 0x4A4740;
    }
}

static void draw_list(void)
{
    int lw = list_w();
    uint32_t bg = card();
    ud_fill(win, 0, top_h, lw, ch - top_h, bg);
    ud_fill(win, lw, top_h, 1, ch - top_h, weave() ? (L.dark ? PU_N_CARD_LINE
                                                     : PU_L_CARD_LINE) : L.line);
    if (!searched[0] || !nrows) {
        ud_text(win, LIST_PAD + 8, top_h + LIST_TOP + 20,
                searched[0] ? "Nothing found" : "Type what to find, then Enter",
                L.dim, UD_SANS, ROW_NAME_PX);
        return;
    }
    ud_clip(win, 0, top_h, lw, ch - top_h);
    int y = top_h + LIST_TOP;
    char cut[256], g[8];
    for (int r = list_top; r < nrows && y < ch; r++) {
        if (rows[r] < 0) {
            int k = -1 - rows[r];
            if (r > list_top)
                y += GROUP_GAP;
            ud_disc(win, LIST_PAD + 8, y + 2, 8, kind_dye[k]);
            char caps[32];
            int n = 0;
            for (const char *s = kind_name[k]; *s && n < 31; s++)
                caps[n++] = (char)(*s >= 'a' && *s <= 'z' ? *s - 32 : *s);
            caps[n] = 0;
            ud_text(win, LIST_PAD + 24, y + 10, caps, L.dim, UD_BOLD, HEAD_PX);
            y += HEAD_H;
            continue;
        }
        const struct hit *h = &hits[rows[r]];
        int on = rows[r] == sel;
        int x = LIST_PAD, w = lw - 2 * LIST_PAD;
        if (on)
            ud_round(win, x, y, w, ROW_H, 9, mix(L.accent, bg, 31));
        uint32_t dye;
        glyph_of(h, g, &dye);
        int ty = y + (ROW_H - ROW_TILE) / 2;
        ud_round(win, x + 10, ty, ROW_TILE, ROW_TILE, 8, dye);
        ud_text_mid(win, x + 10, ty, ROW_TILE, ROW_TILE, g, 0xFFFFFF, UD_MONO,
                    12);
        int tx = x + 10 + ROW_TILE + 12, tw = w - (tx - x) - 24;
        ud_text(win, tx, y + 21,
                fit(h->name, UD_BOLD, ROW_NAME_PX, tw, cut, sizeof(cut)),
                L.text, UD_BOLD, ROW_NAME_PX);
        ud_text(win, tx, y + 38,
                fit(h->where, UD_SANS, ROW_WHERE_PX, tw, cut, sizeof(cut)),
                L.dim, UD_SANS, ROW_WHERE_PX);
        if (on)
            ud_disc(win, x + w - 16, y + ROW_H / 2 - 3, 6, L.accent);
        y += ROW_H;
    }
    ud_unclip(win);
}

/* The right side's buttons, as laid out for a click */
#define BTNS 2
static struct { int x, y, w; const char *label; } btn[BTNS];
static int nbtn;

static void lay_buttons(int right, int y)
{
    static const char *const first[KINDS] = {
        "Open in Code", "Open in Code", "Open Settings", "Open", "Open in Weft"
    };
    nbtn = 0;
    if (sel < 0)
        return;
    int k = hits[sel].kind;
    const char *labels[BTNS] = { first[k], NULL };
    if (k == K_FILE || k == K_INSIDE)
        labels[1] = "Show in Files";
    int x = right;
    for (int i = BTNS - 1; i >= 0; i--) {
        if (!labels[i])
            continue;
        int w = ud_width(labels[i], i ? UD_SANS : UD_BOLD, BTN_PX) + 28;
        x -= w;
        btn[i].x = x;
        btn[i].y = y;
        btn[i].w = w;
        btn[i].label = labels[i];
        x -= 8;
        nbtn = nbtn > i + 1 ? nbtn : i + 1;
    }
}

static void changed_words(long long mtime, char *out, size_t cap)
{
    long long now = (long long)time(NULL);
    long long days = now / 86400 - mtime / 86400;
    if (days <= 0)
        snprintf(out, cap, "changed today");
    else if (days == 1)
        snprintf(out, cap, "changed yesterday");
    else if (days < 7)
        snprintf(out, cap, "changed %lld days ago", days);
    else {
        /* longer ago than a week: the day it was */
        static const char *const mon[12] = { "Jan", "Feb", "Mar", "Apr",
            "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
        time_t t = (time_t)mtime;
        struct tm tm;
        gmtime_r(&t, &tm);
        snprintf(out, cap, "changed %d %s %d", tm.tm_mday, mon[tm.tm_mon],
                 tm.tm_year + 1900);
    }
}

/* One line of the file, the words marked where they are */
static void draw_line(int x, int y, int w, const char *s, int len,
                      int number, int marked, uint32_t box)
{
    char num[12];
    if (marked)
        ud_fill(win, x, y, w, CODE_LINE, mix(L.accent, box, 13));
    snprintf(num, sizeof(num), "%d", number);
    int nb = y + 16;
    ud_text_right(win, x + NUM_W - 14, nb, num, L.dark ? 0x5F6573 : 0xA8A296,
                  UD_MONO, CODE_PX);
    char text[160];
    int n = 0;
    for (int i = 0; i < len && n < (int)sizeof(text) - 1; i++)
        text[n++] = s[i] == '\t' ? ' ' : s[i];
    text[n] = 0;
    int tx = x + NUM_W;
    ud_clip(win, tx, y, w - NUM_W - 8, CODE_LINE);
    ud_text(win, tx, nb, text, L.text, UD_MONO, CODE_PX);
    /* the marks over it: the word's ground, its line under it, the word
       in the link's colour */
    int ql = (int)strlen(searched);
    for (int at = find_in(text, n, searched, 0); at >= 0;
         at = find_in(text, n, searched, at + ql)) {
        char a[160], m[96];
        memcpy(a, text, (size_t)at);
        a[at] = 0;
        int mlen = ql < (int)sizeof(m) - 1 ? ql : (int)sizeof(m) - 1;
        memcpy(m, text + at, (size_t)mlen);
        m[mlen] = 0;
        int mx = tx + ud_width(a, UD_MONO, CODE_PX);
        int mw = ud_width(m, UD_MONO, CODE_PX);
        ud_fill(win, mx - 1, y + 2, mw + 2, CODE_LINE - 4,
                mix(L.accent, box, 46));
        ud_fill(win, mx - 1, y + CODE_LINE - 4, mw + 2, 2, L.accent);
        ud_text(win, mx, nb, m, L.dark ? PU_N_LINK : 0x2A45C0, UD_MONO,
                CODE_PX);
    }
    ud_unclip(win);
}

static void draw_preview(void)
{
    int x0 = list_w() + 1, w = cw - x0;
    ud_fill(win, x0, top_h, w, ch - top_h, L.bg);
    if (sel < 0)
        return;
    const struct hit *h = &hits[sel];
    int x = x0 + PREV_PAD, right = cw - PREV_PAD, y = top_h + PREV_TOP;
    char g[8], cut[256], sub[320], chg[48];
    uint32_t dye;
    glyph_of(h, g, &dye);
    ud_round(win, x, y, PREV_TILE, PREV_TILE, 10, dye);
    ud_text_mid(win, x, y, PREV_TILE, PREV_TILE, g, 0xFFFFFF, UD_MONO, 13);
    if (h->kind == K_INSIDE || h->kind == K_FILE) {
        char folder[200];
        const char *slash = strrchr(h->path, '/');
        char dir[512];
        snprintf(dir, sizeof(dir), "%.*s", slash ? (int)(slash - h->path) : 0,
                 h->path);
        where_of(dir[0] ? dir : "/", folder, sizeof(folder));
        changed_words(h->mtime, chg, sizeof(chg));
        if (h->kind == K_INSIDE)
            snprintf(sub, sizeof(sub), "%s \xC2\xB7 %d match%s \xC2\xB7 %s",
                     folder, h->matches, h->matches == 1 ? "" : "es", chg);
        else
            snprintf(sub, sizeof(sub), "%s \xC2\xB7 %s", folder, chg);
    } else {
        snprintf(sub, sizeof(sub), "%s", h->where);
    }
    /* the buttons beside the name where there is room, under it where
       there is not */
    lay_buttons(right, y + (PREV_TILE - BTN_H) / 2);
    int name_room = (nbtn ? btn[0].x - 12 : right) - (x + PREV_TILE + 12);
    if (name_room < 160 && nbtn) {
        lay_buttons(right, y + PREV_TILE + 10);
        name_room = right - (x + PREV_TILE + 12);
    }
    int nx = x + PREV_TILE + 12;
    ud_text(win, nx, y + 17, fit(h->name, UD_BOLD, PREV_NAME_PX, name_room,
                                 cut, sizeof(cut)),
            L.text, UD_BOLD, PREV_NAME_PX);
    ud_text(win, nx, y + 35, fit(sub, UD_SANS, PREV_SUB_PX, name_room, cut,
                                 sizeof(cut)),
            L.dim, UD_SANS, PREV_SUB_PX);
    uint32_t raised = weave() ? (L.dark ? PU_N_RAISED : PU_L_CARD) : L.field;
    for (int i = 0; i < nbtn; i++) {
        if (!i) {
            ud_round(win, btn[i].x, btn[i].y, btn[i].w, BTN_H, 9, L.accent);
            ud_text_mid(win, btn[i].x, btn[i].y, btn[i].w, BTN_H,
                        btn[i].label, L.on_accent, UD_BOLD, BTN_PX);
        } else {
            ud_round(win, btn[i].x, btn[i].y, btn[i].w, BTN_H, 9, edge());
            ud_round(win, btn[i].x + 1, btn[i].y + 1, btn[i].w - 2,
                     BTN_H - 2, 8, raised);
            ud_text_mid(win, btn[i].x, btn[i].y, btn[i].w, BTN_H,
                        btn[i].label, L.text, UD_SANS, BTN_PX);
        }
    }
    y = (nbtn && btn[0].y > y + 4 ? btn[0].y + BTN_H : y + PREV_TILE) +
        CODE_GAP;

    /* the lines, for a file - over the hint, which takes two lines
       where one is not wide enough */
    static const char hint1[] = "Up and down move between the matches;";
    static const char hint2[] = "Enter opens the file at the one you are on.";
    int hint_y = ch - 16;
    int two = h->kind == K_INSIDE &&
              ud_width(hint1, UD_SANS, HINT_PX) + 4 +
              ud_width(hint2, UD_SANS, HINT_PX) > right - x;
    int hint_top = two ? hint_y - 16 : hint_y;
    if (h->kind == K_INSIDE || h->kind == K_FILE) {
        uint32_t box = L.dark ? FIND_N_FIELD : 0xFFFFFF;
        int bh = hint_top - 22 - y;
        int fit_lines = (bh - 24) / CODE_LINE;
        if (fit_lines < 1)
            return;
        bh = fit_lines * CODE_LINE + 24;
        ud_round(win, x, y, right - x, bh, 12,
                 weave() ? (L.dark ? PU_N_CARD_LINE : PU_L_CARD_LINE) : L.line);
        ud_round(win, x + 1, y + 1, right - x - 2, bh - 2, 11, box);
        if (shown_len < 0) {
            ud_text(win, x + 16, y + 30, "Not text: open it to see it", L.dim,
                    UD_SANS, CODE_PX);
        } else if (shown) {
            int want = cur_line() ? cur_line() : 1;
            int first = want - fit_lines / 3;
            /* near the end, the box filled from further up */
            int total = 1;
            for (int i = 0; i < shown_len; i++)
                if (shown[i] == '\n' && i + 1 < shown_len)
                    total++;
            if (first > total - fit_lines + 1)
                first = total - fit_lines + 1;
            if (first < 1)
                first = 1;
            int line = 1, p = 0;
            while (line < first && p < shown_len) {
                if (shown[p++] == '\n')
                    line++;
            }
            for (int k = 0; k < fit_lines && p < shown_len; k++, line++) {
                int e = p;
                while (e < shown_len && shown[e] != '\n')
                    e++;
                int marked = h->kind == K_INSIDE &&
                             find_in(shown + p, e - p, searched, 0) >= 0;
                draw_line(x + 1, y + 12 + k * CODE_LINE, right - x - 2,
                          shown + p, e - p, line, marked, box);
                p = e + 1;
            }
        }
        if (h->kind != K_INSIDE) {
            ud_text(win, x, hint_y, "Enter opens the file.", L.dim, UD_SANS,
                    HINT_PX);
        } else if (two) {
            ud_text(win, x, hint_top, hint1, L.dim, UD_SANS, HINT_PX);
            ud_text(win, x, hint_y, hint2, L.dim, UD_SANS, HINT_PX);
        } else {
            ud_text(win, x, hint_y, hint1, L.dim, UD_SANS, HINT_PX);
            ud_text(win, x + ud_width(hint1, UD_SANS, HINT_PX) + 4, hint_y,
                    hint2, L.dim, UD_SANS, HINT_PX);
        }
    } else {
        ud_text(win, x, y + 16, "Enter opens it.", L.dim, UD_SANS, HINT_PX);
    }
}

static void draw(void)
{
    ud_look(&L);
    ud_hold(win, 1);
    lay_pills();
    draw_top();
    draw_list();
    draw_preview();
    ud_hold(win, 0);
}

/* --- doing what was chosen ------------------------------------------ */

static void start(char *const argv[])
{
    if (sys_fork() == 0) {
        sys_exec(argv[0], argv);
        sys_exit(1);
    }
}

static void open_hit(int which)
{
    if (sel < 0)
        return;
    const struct hit *h = &hits[sel];
    char ln[16], dir[512];
    if (h->kind == K_FILE || h->kind == K_INSIDE) {
        if (which == 0) {
            snprintf(ln, sizeof(ln), "%d", cur_line() ? cur_line() : 1);
            printf("[find] open code %s %s\n", h->path, ln);
            fflush(stdout);
            char *const argv[] = { "gcode", (char *)h->path, ln, NULL };
            start(argv);
        } else {
            const char *slash = strrchr(h->path, '/');
            snprintf(dir, sizeof(dir), "%.*s",
                     slash && slash != h->path ? (int)(slash - h->path) : 1,
                     h->path);
            say("open files %s%s", dir, "");
            char *const argv[] = { "gfiles", dir, NULL };
            start(argv);
        }
    } else if (h->kind == K_SETTING) {
        say("open settings %s%s", h->path, "");
        char *const argv[] = { "gsettings", "--section", (char *)h->path,
                               NULL };
        start(argv);
    } else if (h->kind == K_PROGRAM) {
        say("open program %s%s", h->path, "");
        char *const argv[] = { (char *)h->path, NULL };
        start(argv);
    } else {
        say("open weft %s%s", h->path, "");
        char *const argv[] = { "weft", (char *)h->path, NULL };
        start(argv);
    }
}

/* Up and down: through the chosen file's matches, then on to the next
   result shown */
static void step(int d)
{
    if (!nrows)
        return;
    if (sel >= 0 && hits[sel].kind == K_INSIDE) {
        int n = hits[sel].nlines;
        if (mi + d >= 0 && mi + d < n) {
            mi += d;
            say_at();
            return;
        }
    }
    int r = 0;
    while (r < nrows && rows[r] != sel)
        r++;
    for (r += d; r >= 0 && r < nrows; r += d)
        if (rows[r] >= 0)
            break;
    if (r < 0 || r >= nrows)
        return;
    sel = rows[r];
    mi = d < 0 && hits[sel].kind == K_INSIDE ? hits[sel].nlines - 1 : 0;
    if (mi < 0)
        mi = 0;
    load_shown();
    say_at();
    /* keep it in sight */
    if (r < list_top)
        list_top = r;
    int y = top_h + LIST_TOP;
    for (int k = list_top; k <= r; k++)
        y += row_h(k);
    while (y > ch && list_top < r) {
        y -= row_h(list_top);
        list_top++;
    }
}

static void searched_now(void)
{
    search();
    lay_rows();
    list_top = 0;
    sel = -1;
    int f = first_hit();
    if (f >= 0)
        choose(f);
    else
        load_shown();
}

static void key(int c)
{
    if (c == '\n' || c == '\r') {
        if (strcmp(query, searched) || !nhit) {
            /* new words: everything they find, the pills saying where -
               a filter kept from the words before could hide all of it */
            if (strcmp(query, searched))
                filter = 0;
            searched_now();
        } else {
            open_hit(0);
        }
    } else if (c == '\b' || c == 127) {
        size_t n = strlen(query);
        while (n && ((unsigned char)query[n - 1] & 0xC0) == 0x80)
            n--;
        if (n)
            query[n - 1] = 0;
    } else if (c == 27) {
        query[0] = 0;
    } else if (c == KEY_UP || c == KEY_DOWN) {
        step(c == KEY_UP ? -1 : 1);
    } else if (c >= 32 && c < 256) {
        size_t n = strlen(query);
        if (n + 2 < sizeof(query)) {
            query[n] = (char)c;
            query[n + 1] = 0;
        }
    }
}

static void click(int x, int y)
{
    for (int i = 0; i < PILLS + 2; i++) {
        struct pill *p = &pill[i];
        if (x < p->x || x >= p->x + p->w || y < p->y || y >= p->y + PILL_H)
            continue;
        if (i < PILLS) {
            filter = i;
            say("filter %s%s", pill_name(i), "");
            lay_rows();
            list_top = 0;
            if (sel < 0 || !shows(hits[sel].kind)) {
                sel = -1;
                int f = first_hit();
                if (f >= 0)
                    choose(f);
                else
                    load_shown();
            }
        } else if (i == PILLS) {
            when = (when + 1) % 4;
            say("changed %s%s", when_word[when], "");
            if (searched[0])
                searched_now();
        } else {
            scope = !scope;
            say("in %s%s", scope_word[scope], "");
            if (searched[0])
                searched_now();
        }
        return;
    }
    for (int i = 0; i < nbtn; i++)
        if (x >= btn[i].x && x < btn[i].x + btn[i].w && y >= btn[i].y &&
            y < btn[i].y + BTN_H) {
            open_hit(i);
            return;
        }
    if (x < list_w() && y >= top_h + LIST_TOP) {
        /* the rows as draw_list lays them */
        int yy = top_h + LIST_TOP;
        for (int r = list_top; r < nrows; r++) {
            if (rows[r] < 0) {
                if (r > list_top)
                    yy += GROUP_GAP;
                yy += HEAD_H;
                continue;
            }
            if (y >= yy && y < yy + ROW_H) {
                if (rows[r] != sel)
                    choose(rows[r]);
                return;
            }
            yy += ROW_H;
        }
    }
}

int main(int argc, char **argv)
{
    hits = calloc(HIT_MAX, sizeof(*hits));
    if (!hits)
        return 1;
    if (!getcwd(home, sizeof(home)))
        strcpy(home, "/");
    static const int sz[2][2] = { { W1, H1 }, { W0, H0 } };
    for (int i = 0; i < 2 && win < 0; i++) {
        win = (int)win_create(sz[i][0], sz[i][1], "Find");
        cw = sz[i][0];
        ch = sz[i][1];
    }
    if (win < 0)
        return 1;
    win_truecolour(win);
    for (int i = 1; i < argc; i++) {
        size_t n = strlen(query);
        snprintf(query + n, sizeof(query) - n, "%s%s", n ? " " : "", argv[i]);
    }
    if (query[0])
        searched_now();
    draw();
    printf("[find] drawn %d %d\n", cw, ch);
    fflush(stdout);
    for (;;) {
        struct win_event ev;
        int redraw = 0;
        while (win_poll(win, &ev) == 1) {
            redraw = 1;
            if (ev.type == WEV_CLOSE)
                return 0;
            if (ev.type == WEV_SIZE && ev.x > 0 && ev.y > 0) {
                cw = ev.x;
                ch = ev.y;
            } else if (ev.type == WEV_KEY) {
                key(ev.val & 0xFF);
            } else if (ev.type == WEV_CLICK) {
                click(ev.x, ev.y);
            } else if (ev.type == WEV_SCROLL && ev.x < list_w()) {
                list_top -= ev.val;
                if (list_top > nrows - 1)
                    list_top = nrows - 1;
                if (list_top < 0)
                    list_top = 0;
            }
        }
        if (redraw)
            draw();
        sys_sleep(30);
    }
}
