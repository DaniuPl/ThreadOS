/* gmend: Mend - the backup, on a stick
 *
 * docs/weave/canvas/NotesMend.dc.html: whether the files are copied,
 * and when, and where to; the days there are copies of, knots on a
 * thread, one chosen; a folder as it was that day, each file said to be
 * the same, changed since or deleted since; and a file brought back -
 * beside the one there is, never over it.
 *
 * The copies live on a FAT stick (kernel/vol.c), so that the machine's
 * own disk going wrong does not take them with it:
 *
 *   /MEND/days/YYYY-MM-DD.txt   what the home held that day: a line
 *                               "#mend 1 TIME" (when the copy was
 *                               made), then "PATH SIZE MTIME DAY" for
 *                               every file, tab apart - DAY says which
 *                               day's folder below holds its bytes
 *   /MEND/store/YYYY-MM-DD/...  the files new or changed that day
 *
 * A copy writes only what changed since the one before, and a day's
 * list still names every file, so any day can be brought back whole.
 * The home is copied but for what begins with a dot (the system's own),
 * files up to 16 MiB each.
 *
 * What it does is said on the serial port, for scripts/tests/t_mend.py:
 *
 *   [mend] drawn W H
 *   [mend] stick LABEL KIND on disk N | [mend] no stick
 *   [mend] days N: DAY DAY ...
 *   [mend] copied N files, M new or changed, to LABEL
 *   [mend] chose DAY
 *   [mend] row NAME STATE
 *   [mend] brought back PATH as NAME
 *
 * Built against musl: a home of any size, files of any length.
 */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "ugui.h"

/* --- where things are (scripts/tests/t_mend.py reads these) ---------- */

#define W1 606
#define H1 600
#define W0 560
#define H0 470
#define PAD_X 24
#define PAD_Y 20
#define BADGE 50
#define STATE_PX 20
#define STATE_SUB_PX 13
#define DAYS_Y 88             /* the heading "Go back to a day" */
#define DAYS_PX 12
#define THREAD_Y 112          /* the thread's line, under the heading */
#define KNOT_D 14
#define KNOT_ON 18
#define DAY_PX 11
#define CARD_Y 172
#define CARD_HEAD 36
#define ROW_H 38
#define ROW_PX 13
#define ROWS_SHOWN 5
#define BTN_H 38
#define BTN_PX 13
#define NOTE_PX 12

#define WELD 0x1E8A4C
#define SAFFRON 0xB86E00
#define MADDER 0xC9302C

/* --- the copies ----------------------------------------------------- */

struct entry {
    char path[400];           /* from the home, "Documents/notes.txt" */
    long long size, mtime;
    char store[11];           /* the day whose folder holds its bytes */
};

static struct entry *ents;    /* the chosen day's list */
static int nents, cap_ents;
static long long day_time;    /* when that day's copy was made */

#define DAYS_MAX 64
static char days[DAYS_MAX][11];
static int ndays, pick = -1;

static struct vol_info stick;
static int have_stick;
static char home[256];
static char shown[400];       /* the folder shown, from the home ("" = it) */
static long long last_copy;   /* when the newest copy was made, 0 never */
static char note[160];

static int win = -1, cw, ch;
static struct ud_look L;

static void say(const char *fmt, const char *a, const char *b)
{
    printf("[mend] ");
    printf(fmt, a, b);
    printf("\n");
    fflush(stdout);
}

static void today(char *out)
{
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    char b[48];
    snprintf(b, sizeof(b), "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1,
             tm.tm_mday);
    snprintf(out, 11, "%.10s", b);
}

static int day_tm(const char *day, struct tm *tm)
{
    memset(tm, 0, sizeof(*tm));
    if (sscanf(day, "%d-%d-%d", &tm->tm_year, &tm->tm_mon, &tm->tm_mday) != 3)
        return -1;
    tm->tm_year -= 1900;
    tm->tm_mon -= 1;
    time_t t = timegm(tm);
    gmtime_r(&t, tm);                 /* the day of the week with it */
    return 0;
}

static const char *const wday[7] = { "Sunday", "Monday", "Tuesday",
    "Wednesday", "Thursday", "Friday", "Saturday" };

/* "Tue 17", or "Today" */
static void day_label(const char *day, char *out, size_t cap)
{
    char t[11];
    today(t);
    struct tm tm;
    if (!strcmp(day, t)) {
        snprintf(out, cap, "Today");
    } else if (day_tm(day, &tm) == 0) {
        snprintf(out, cap, "%.3s %d", wday[tm.tm_wday], tm.tm_mday);
    } else {
        snprintf(out, cap, "%s", day);
    }
}

/* --- the stick -------------------------------------------------------- */

static void find_stick(void)
{
    struct vol_info v[8];
    int n = vol_list(v, 8);
    have_stick = n > 0;
    if (!have_stick) {
        say("no stick%s%s", "", "");
        return;
    }
    stick = v[0];
    char d[12];
    snprintf(d, sizeof(d), "%d", stick.disk + 1);
    printf("[mend] stick %s %s on disk %s\n",
           stick.label[0] ? stick.label : "(no label)", stick.kind, d);
    fflush(stdout);
}

static const char *stick_name(void)
{
    static char b[40];
    if (stick.label[0] && strcmp(stick.label, "NO NAME"))
        snprintf(b, sizeof(b), "%s", stick.label);
    else
        snprintf(b, sizeof(b), "the USB stick");
    return b;
}

static int mounted(void)
{
    return have_stick && vol_mount(stick.disk, stick.lba) == 0;
}

static int by_name(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

static void scan_days(void)
{
    ndays = 0;
    last_copy = 0;
    if (!mounted())
        return;
    struct vol_entry e;
    for (int i = 0; vol_dir("/MEND/days", i, &e) == 1 && ndays < DAYS_MAX;
         i++) {
        size_t n = strlen(e.name);
        if (e.is_dir || n != 14 || strcmp(e.name + 10, ".txt"))
            continue;
        snprintf(days[ndays++], 11, "%.10s", e.name);
    }
    qsort(days, (size_t)ndays, sizeof(days[0]), by_name);
    printf("[mend] days %d:", ndays);
    for (int i = 0; i < ndays; i++)
        printf(" %s", days[i]);
    printf("\n");
    fflush(stdout);
}

/* A day's list, from the stick */
static void load_day(const char *day)
{
    nents = 0;
    day_time = 0;
    char path[64];
    snprintf(path, sizeof(path), "/MEND/days/%s.txt", day);
    struct vol_entry st;
    if (!mounted() || vol_stat(path, &st) != 1 || st.is_dir)
        return;
    char *t = malloc(st.size + 1);
    if (!t)
        return;
    int got = vol_read(path, 0, t, st.size);
    if (got < 0) {
        free(t);
        return;
    }
    t[got] = 0;
    for (char *p = t; *p; ) {
        char *e = strchr(p, '\n');
        if (e)
            *e = 0;
        if (!strncmp(p, "#mend 1 ", 8)) {
            day_time = atoll(p + 8);
        } else if (*p && *p != '#') {
            char *f[4] = { p, 0, 0, 0 };
            int k = 1;
            for (char *q = p; *q && k < 4; q++)
                if (*q == '\t') {
                    *q = 0;
                    f[k++] = q + 1;
                }
            if (k == 4) {
                if (nents == cap_ents) {
                    cap_ents = cap_ents ? cap_ents * 2 : 256;
                    ents = realloc(ents, (size_t)cap_ents * sizeof(*ents));
                }
                struct entry *en = &ents[nents++];
                snprintf(en->path, sizeof(en->path), "%s", f[0]);
                en->size = atoll(f[1]);
                en->mtime = atoll(f[2]);
                snprintf(en->store, sizeof(en->store), "%.10s", f[3]);
            }
        }
        if (!e)
            break;
        p = e + 1;
    }
    free(t);
}

/* --- a copy, now ----------------------------------------------------- */

static FILE *list_out;
static char *list_buf;
static size_t list_len;
static int copied, changed;
static const char *copy_day;

static struct entry *prev_of(const char *rel)
{
    for (int i = 0; i < nents; i++)
        if (!strcmp(ents[i].path, rel))
            return &ents[i];
    return NULL;
}

static int put_file(const char *abs, const char *rel, long long size)
{
    if (size > 16 * 1024 * 1024)
        return -1;
    FILE *f = fopen(abs, "rb");
    if (!f)
        return -1;
    char *b = malloc((size_t)size + 1);
    if (!b) {
        fclose(f);
        return -1;
    }
    size_t got = fread(b, 1, (size_t)size, f);
    fclose(f);
    char dst[480];
    snprintf(dst, sizeof(dst), "/MEND/store/%s/%s", copy_day, rel);
    char *slash = strrchr(dst, '/');
    *slash = 0;
    vol_mkdir(dst);
    *slash = '/';
    int r = vol_write(dst, b, (uint32_t)got);
    free(b);
    return r;
}

static void copy_tree(const char *rel)
{
    char abs[700];
    snprintf(abs, sizeof(abs), "%s%s%s", home, rel[0] ? "/" : "", rel);
    DIR *d = opendir(abs);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        char r2[400], a2[700];
        snprintf(r2, sizeof(r2), "%s%s%s", rel, rel[0] ? "/" : "", e->d_name);
        snprintf(a2, sizeof(a2), "%s/%s", home, r2);
        struct stat st;
        if (stat(a2, &st) < 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            copy_tree(r2);
            continue;
        }
        if (!S_ISREG(st.st_mode) || st.st_size > 16 * 1024 * 1024)
            continue;
        struct entry *p = prev_of(r2);
        const char *store = copy_day;
        if (p && p->size == (long long)st.st_size &&
            p->mtime == (long long)st.st_mtime) {
            store = p->store;          /* the same: its bytes are there */
        } else {
            if (put_file(a2, r2, (long long)st.st_size) < 0)
                continue;
            changed++;
        }
        copied++;
        fprintf(list_out, "%s\t%lld\t%lld\t%s\n", r2, (long long)st.st_size,
                (long long)st.st_mtime, store);
    }
    closedir(d);
}

static void copy_now(void)
{
    if (!have_stick || !mounted()) {
        snprintf(note, sizeof(note), "There is no stick to copy to");
        return;
    }
    char day[11];
    today(day);
    copy_day = day;
    /* what the newest copy holds: what is unchanged is not copied again */
    if (ndays)
        load_day(days[ndays - 1]);
    else
        nents = 0;
    copied = changed = 0;
    list_buf = NULL;
    list_len = 0;
    list_out = open_memstream(&list_buf, &list_len);
    if (!list_out)
        return;
    fprintf(list_out, "#mend 1 %lld\n", (long long)time(NULL));
    copy_tree("");
    fclose(list_out);
    vol_mkdir("/MEND/days");
    char path[64];
    snprintf(path, sizeof(path), "/MEND/days/%s.txt", day);
    int r = vol_write(path, list_buf, (uint32_t)list_len);
    free(list_buf);
    char n1[16], n2[64];
    snprintf(n1, sizeof(n1), "%d", copied);
    snprintf(n2, sizeof(n2), "%d new or changed, to %s", changed,
             stick_name());
    if (r == 0)
        say("copied %s files, %s", n1, n2);
    snprintf(note, sizeof(note), r == 0 ? "Copied %d files, %d of them new "
             "or changed" : "The copy could not be written", copied, changed);
    scan_days();
    pick = ndays - 1;
    if (pick >= 0)
        load_day(days[pick]);
}

/* --- a folder as it was ---------------------------------------------- */

#define ST_SAME    0
#define ST_CHANGED 1
#define ST_DELETED 2
static const char *const state_word[3] = {
    "the same", "changed since", "deleted since"
};

#define ROWS_MAX 256
static struct row {
    char name[128];
    int is_dir, state, ent;  /* ent: the file's entry, or -1 */
} rows[ROWS_MAX];
static int nrows, sel = -1, row_top;

static int state_of(const struct entry *e)
{
    char abs[700];
    struct stat st;
    snprintf(abs, sizeof(abs), "%s/%s", home, e->path);
    if (stat(abs, &st) < 0)
        return ST_DELETED;
    if ((long long)st.st_size != e->size || (long long)st.st_mtime != e->mtime)
        return ST_CHANGED;
    return ST_SAME;
}

static void lay_rows(void)
{
    nrows = 0;
    size_t sl = strlen(shown);
    for (int i = 0; i < nents && nrows < ROWS_MAX; i++) {
        const char *p = ents[i].path;
        if (sl && (strncmp(p, shown, sl) || p[sl] != '/'))
            continue;
        const char *rest = sl ? p + sl + 1 : p;
        const char *slash = strchr(rest, '/');
        char name[128];
        snprintf(name, sizeof(name), "%.*s",
                 slash ? (int)(slash - rest) : (int)strlen(rest), rest);
        int st = state_of(&ents[i]);
        int r = 0;
        while (r < nrows && strcmp(rows[r].name, name))
            r++;
        if (r == nrows) {
            snprintf(rows[r].name, sizeof(rows[r].name), "%s", name);
            rows[r].is_dir = slash != NULL;
            rows[r].state = st;
            rows[r].ent = slash ? -1 : i;
            nrows++;
        } else if (st != ST_SAME && rows[r].state == ST_SAME) {
            rows[r].state = ST_CHANGED;   /* a folder: something in it */
        }
    }
    if (sel >= nrows)
        sel = -1;
    for (int r = 0; r < nrows; r++) {
        char line[200];
        snprintf(line, sizeof(line), "%.150s%s", rows[r].name,
                 rows[r].is_dir ? "/" : "");
        say("row %s %s", line, state_word[rows[r].state]);
    }
}

static void choose_day(int i)
{
    if (i < 0 || i >= ndays)
        return;
    pick = i;
    load_day(days[i]);
    say("chose %s%s", days[i], "");
    sel = -1;
    row_top = 0;
    lay_rows();
}

/* Beside it, never over it: "notes (Tuesday).txt" */
static void bring_back(void)
{
    if (sel < 0 || rows[sel].is_dir || rows[sel].ent < 0 || pick < 0)
        return;
    const struct entry *e = &ents[rows[sel].ent];
    char src[480];
    snprintf(src, sizeof(src), "/MEND/store/%s/%s", e->store, e->path);
    struct tm tm;
    day_tm(days[pick], &tm);
    char dir[400], base[200], ext[40] = "", name[300], abs[800];
    const char *slash = strrchr(e->path, '/');
    snprintf(dir, sizeof(dir), "%.*s", slash ? (int)(slash - e->path) : 0,
             e->path);
    snprintf(base, sizeof(base), "%.199s", slash ? slash + 1 : e->path);
    char *dot = strrchr(base, '.');
    if (dot && dot != base) {
        snprintf(ext, sizeof(ext), "%s", dot);
        *dot = 0;
    }
    struct stat st;
    for (int k = 1; k < 100; k++) {
        if (k == 1)
            snprintf(name, sizeof(name), "%s (%s)%s", base, wday[tm.tm_wday],
                     ext);
        else
            snprintf(name, sizeof(name), "%s (%s %d)%s", base,
                     wday[tm.tm_wday], k, ext);
        snprintf(abs, sizeof(abs), "%s/%s%s%s", home, dir, dir[0] ? "/" : "",
                 name);
        if (stat(abs, &st) < 0)
            break;
    }
    if (!mounted())
        return;
    char *b = malloc((size_t)e->size + 1);
    if (!b)
        return;
    int got = vol_read(src, 0, b, (uint32_t)e->size);
    if (got < 0) {
        free(b);
        snprintf(note, sizeof(note), "The copy of %.100s is not on the stick",
                 base);
        return;
    }
    if (dir[0]) {
        char d2[600];
        snprintf(d2, sizeof(d2), "%s/%s", home, dir);
        mkdir(d2, 0755);
    }
    FILE *f = fopen(abs, "wb");
    if (f) {
        fwrite(b, 1, (size_t)got, f);
        fclose(f);
        say("brought back %s as %s", e->path, name);
        snprintf(note, sizeof(note), "Brought back as \"%.130s\"", name);
    }
    free(b);
    lay_rows();
}

/* --- drawing ------------------------------------------------------ */

static int weave(void)
{
    return L.style == UD_STYLE_WEAVE;
}

static uint32_t mix(uint32_t a, uint32_t b, int alpha)
{
    uint32_t r = (((a >> 16) & 255) * alpha + ((b >> 16) & 255) * (255 - alpha)) / 255;
    uint32_t g = (((a >> 8) & 255) * alpha + ((b >> 8) & 255) * (255 - alpha)) / 255;
    uint32_t bl = ((a & 255) * alpha + (b & 255) * (255 - alpha)) / 255;
    return r << 16 | g << 8 | bl;
}

static void when_words(long long t, char *out, size_t cap)
{
    time_t now = time(NULL), tt = (time_t)t;
    struct tm a, b;
    gmtime_r(&tt, &a);
    gmtime_r(&now, &b);
    long long days_ago = (long long)now / 86400 - t / 86400;
    if (days_ago <= 0)
        snprintf(out, cap, "Today at %d:%02d", a.tm_hour, a.tm_min);
    else if (days_ago == 1)
        snprintf(out, cap, "Yesterday at %d:%02d", a.tm_hour, a.tm_min);
    else
        snprintf(out, cap, "On %s %d %s", wday[a.tm_wday], a.tm_mday,
                 (const char *[]){ "January", "February", "March", "April",
                 "May", "June", "July", "August", "September", "October",
                 "November", "December" }[a.tm_mon]);
    (void)b;
}

static struct { int x, y, w; const char *label; } btn[2];

static void draw(void)
{
    ud_look(&L);
    ud_hold(win, 1);
    uint32_t bg = L.bg;
    ud_fill(win, 0, 0, cw, ch, bg);
    int x = PAD_X, y = PAD_Y;

    /* where things stand */
    uint32_t badge = !have_stick ? (L.dark ? 0x4B505E : 0x8E897F)
                   : last_copy ? WELD : SAFFRON;
    ud_disc(win, x, y, BADGE, badge);
    int cx = x + BADGE / 2, cy = y + BADGE / 2;
    if (have_stick && last_copy) {
        ud_line(win, cx - 9, cy, cx - 3, cy + 6, 48, 0xFFFFFF);
        ud_line(win, cx - 3, cy + 6, cx + 9, cy - 7, 48, 0xFFFFFF);
    } else {
        ud_line(win, cx, cy - 11, cx, cy + 3, 48, 0xFFFFFF);
        ud_disc(win, cx - 2, cy + 7, 4, 0xFFFFFF);
    }
    char sub[200], when[64];
    const char *title;
    if (!have_stick) {
        title = "No stick to copy to";
        snprintf(sub, sizeof(sub), "Put in a USB stick with FAT on it");
    } else if (!last_copy) {
        title = "Not copied yet";
        snprintf(sub, sizeof(sub), "To %s \xC2\xB7 Copy now makes the first "
                 "copy", stick_name());
    } else {
        title = "Your files are copied";
        when_words(last_copy, when, sizeof(when));
        snprintf(sub, sizeof(sub), "%s, to %s", when, stick_name());
    }
    int tx = x + BADGE + 14;
    ud_text(win, tx, y + 22, title, L.text, UD_BOLD, STATE_PX);
    ud_text(win, tx, y + 42, sub, L.dim, UD_SANS, STATE_SUB_PX);

    /* the days, knots on a thread */
    ud_text(win, x, DAYS_Y + DAYS_PX, "GO BACK TO A DAY", L.dim, UD_BOLD,
            DAYS_PX);
    int left = x + 10, right = cw - PAD_X - 10;
    if (ndays) {
        ud_fill(win, left, THREAD_Y, right - left, 2, WELD);
        int first = ndays > 8 ? ndays - 8 : 0, n = ndays - first;
        for (int k = 0; k < n; k++) {
            int i = first + k;
            int kx = n == 1 ? (left + right) / 2
                            : left + (right - left) * k / (n - 1);
            int on = i == pick, d = on ? KNOT_ON : KNOT_D;
            ud_disc(win, kx - d / 2, THREAD_Y + 1 - d / 2, d, WELD);
            if (!on)
                ud_disc(win, kx - d / 2 + 2, THREAD_Y + 1 - d / 2 + 2, d - 4,
                        bg);
            char lab[24];
            day_label(days[i], lab, sizeof(lab));
            int lw = ud_width(lab, on ? UD_BOLD : UD_SANS, DAY_PX);
            ud_text(win, kx - lw / 2, THREAD_Y + 28, lab,
                    on ? L.text : L.dim, on ? UD_BOLD : UD_SANS, DAY_PX);
        }
    } else {
        ud_text(win, x, THREAD_Y + 6, have_stick ? "No copies yet"
                : "No stick", L.dim, UD_SANS, ROW_PX);
    }

    /* the folder as it was */
    uint32_t card = weave() ? (L.dark ? PU_N_CARD : PU_L_CARD) : L.field;
    uint32_t line = weave() ? (L.dark ? PU_N_CARD_LINE : PU_L_CARD_LINE)
                            : L.line;
    int ch_h = CARD_HEAD + ROWS_SHOWN * ROW_H;
    int cwid = cw - 2 * PAD_X;
    ud_round(win, x, CARD_Y, cwid, ch_h, 12, line);
    ud_round(win, x + 1, CARD_Y + 1, cwid - 2, ch_h - 2, 11, card);
    char head[200], lab[24] = "";
    if (pick >= 0)
        day_label(days[pick], lab, sizeof(lab));
    snprintf(head, sizeof(head), "%s%.140s, as it was on %s",
             shown[0] ? "" : "Home",
             shown[0] ? (strrchr(shown, '/') ? strrchr(shown, '/') + 1
                                             : shown) : "",
             pick >= 0 ? lab : "-");
    int hx = x + 14;
    if (shown[0]) {
        /* up a level: a click on the heading */
        int my = CARD_Y + CARD_HEAD / 2;
        ud_line(win, hx + 6, my - 5, hx + 1, my, 28, L.accent);
        ud_line(win, hx + 1, my, hx + 6, my + 5, 28, L.accent);
        hx += 16;
    }
    ud_text(win, hx, CARD_Y + 23, head, L.text, UD_BOLD, ROW_PX);
    ud_stitch(win, x + 1, CARD_Y + CARD_HEAD - 1, cwid - 2, 0, line);
    for (int k = 0; k < ROWS_SHOWN && row_top + k < nrows; k++) {
        const struct row *r = &rows[row_top + k];
        int ry = CARD_Y + CARD_HEAD + k * ROW_H;
        if (row_top + k == sel)
            ud_fill(win, x + 1, ry, cwid - 2, ROW_H, mix(L.accent, card, 31));
        uint32_t dye = r->is_dir ? 0x4A6BE0 : 0xC77F0A;
        const char *dot = strrchr(r->name, '.');
        if (!r->is_dir && dot && (!strcmp(dot, ".csv") || !strcmp(dot, ".xls")))
            dye = 0x2F9A5C;
        ud_round(win, x + 14, ry + (ROW_H - 12) / 2, 16, 12, 3, dye);
        ud_text(win, x + 40, ry + 24, r->name, L.text, UD_SANS, ROW_PX);
        uint32_t sc = r->state == ST_CHANGED ? SAFFRON
                    : r->state == ST_DELETED ? MADDER : L.dim;
        ud_text_right(win, x + cwid - 14, ry + 24, state_word[r->state], sc,
                      UD_SANS, 12);
        if (k < ROWS_SHOWN - 1)
            ud_stitch(win, x + 14, ry + ROW_H - 1, cwid - 28, 0, line);
    }

    /* bring back, copy now */
    int by = CARD_Y + ch_h + 16;
    char bl[200];
    if (sel >= 0 && !rows[sel].is_dir)
        snprintf(bl, sizeof(bl), "Bring back %s", rows[sel].name);
    else
        snprintf(bl, sizeof(bl), "Bring back a file");
    btn[0].label = bl;
    btn[0].x = x;
    btn[0].y = by;
    btn[0].w = ud_width(bl, UD_BOLD, BTN_PX) + 36;
    btn[1].label = "Copy now";
    btn[1].x = x + btn[0].w + 10;
    btn[1].y = by;
    btn[1].w = ud_width("Copy now", UD_SANS, BTN_PX) + 28;
    int can_bring = sel >= 0 && !rows[sel].is_dir;
    ud_round(win, btn[0].x, by, btn[0].w, BTN_H, 9,
             can_bring ? WELD : mix(WELD, bg, 102));
    ud_text_mid(win, btn[0].x, by, btn[0].w, BTN_H, bl, 0xFFFFFF, UD_BOLD,
                BTN_PX);
    ud_round(win, btn[1].x, by, btn[1].w, BTN_H, 9, line);
    ud_round(win, btn[1].x + 1, by + 1, btn[1].w - 2, BTN_H - 2, 8, card);
    ud_text_mid(win, btn[1].x, by, btn[1].w, BTN_H, "Copy now",
                have_stick ? L.text : L.dim, UD_SANS, BTN_PX);

    const char *foot = note[0] ? note
        : "Bringing a file back never writes over the one you have: it "
          "comes back beside it.";
    ud_text(win, x, by + BTN_H + 22, foot, L.dim, UD_SANS, NOTE_PX);
    ud_hold(win, 0);
}

/* --- clicks ------------------------------------------------------- */

static void click(int x, int y)
{
    for (int i = 0; i < 2; i++)
        if (x >= btn[i].x && x < btn[i].x + btn[i].w && y >= btn[i].y &&
            y < btn[i].y + BTN_H) {
            if (i == 0)
                bring_back();
            else {
                copy_now();
                last_copy = day_time;
                if (pick >= 0)
                    lay_rows();
            }
            return;
        }
    /* a knot */
    if (ndays && y >= THREAD_Y - 14 && y < THREAD_Y + 34) {
        int left = PAD_X + 10, right = cw - PAD_X - 10;
        int first = ndays > 8 ? ndays - 8 : 0, n = ndays - first, best = -1;
        int bd = 1 << 30;
        for (int k = 0; k < n; k++) {
            int kx = n == 1 ? (left + right) / 2
                            : left + (right - left) * k / (n - 1);
            int dd = kx > x ? kx - x : x - kx;
            if (dd < bd) {
                bd = dd;
                best = first + k;
            }
        }
        if (best >= 0 && bd < 30 && best != pick)
            choose_day(best);
        return;
    }
    /* the folder's heading: back up a level */
    if (y >= CARD_Y && y < CARD_Y + CARD_HEAD && shown[0]) {
        char *s = strrchr(shown, '/');
        if (s)
            *s = 0;
        else
            shown[0] = 0;
        sel = -1;
        row_top = 0;
        lay_rows();
        return;
    }
    /* a row: a file chosen, a folder opened */
    if (y >= CARD_Y + CARD_HEAD && y < CARD_Y + CARD_HEAD + ROWS_SHOWN * ROW_H) {
        int r = row_top + (y - CARD_Y - CARD_HEAD) / ROW_H;
        if (r >= nrows)
            return;
        if (rows[r].is_dir) {
            size_t n = strlen(shown);
            snprintf(shown + n, sizeof(shown) - n, "%s%s", n ? "/" : "",
                     rows[r].name);
            sel = -1;
            row_top = 0;
            lay_rows();
        } else {
            sel = r;
        }
    }
}

int main(void)
{
    if (!getcwd(home, sizeof(home)))
        strcpy(home, "/");
    static const int sz[2][2] = { { W1, H1 }, { W0, H0 } };
    for (int i = 0; i < 2 && win < 0; i++) {
        win = (int)win_create(sz[i][0], sz[i][1], "Mend");
        cw = sz[i][0];
        ch = sz[i][1];
    }
    if (win < 0)
        return 1;
    win_truecolour(win);
    find_stick();
    scan_days();
    /* the folder people keep their files in, when there is one */
    struct stat st;
    char docs[300];
    snprintf(docs, sizeof(docs), "%s/Documents", home);
    if (stat(docs, &st) == 0 && S_ISDIR(st.st_mode))
        snprintf(shown, sizeof(shown), "Documents");
    if (ndays) {
        load_day(days[ndays - 1]);
        last_copy = day_time;
        choose_day(ndays - 1);
    }
    draw();
    printf("[mend] drawn %d %d\n", cw, ch);
    fflush(stdout);
    for (;;) {
        struct win_event ev;
        int redraw = 0;
        while (win_poll(win, &ev) == 1) {
            redraw = 1;
            if (ev.type == WEV_CLOSE) {
                vol_umount();
                return 0;
            }
            if (ev.type == WEV_SIZE && ev.x > 0 && ev.y > 0) {
                cw = ev.x;
                ch = ev.y;
            } else if (ev.type == WEV_CLICK) {
                click(ev.x, ev.y);
            } else if (ev.type == WEV_SCROLL) {
                row_top -= ev.val;
                if (row_top > nrows - ROWS_SHOWN)
                    row_top = nrows - ROWS_SHOWN;
                if (row_top < 0)
                    row_top = 0;
            }
        }
        if (redraw)
            draw();
        sys_sleep(30);
    }
}
