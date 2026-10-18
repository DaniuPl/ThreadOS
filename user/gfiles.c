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
 * the directory in the window's own title, "Open" that hands the
 * chosen file to its program, and "Remove" that asks first.
 *
 * Then what a person expects of one after using any other: the
 * folders as a tree down the left, so that where one is stands in the
 * picture of where everything is; a folder made from here; a file cut
 * or copied in one folder and pasted in another; and a double click
 * that opens, instead of a click and then a button.
 *
 * The kernel reads only the directory a program is standing in, so
 * the tree is read by walking: into every folder that is open in it,
 * one readdir each, and back to where the list is. That is a handful
 * of system calls twice a second, for a tree a person opened by hand.
 *
 * The one thing that depends on the processor mode is reading a
 * directory: protected mode hands back the size in the return value,
 * long mode fills in a structure. Everything else - the list, the
 * clicking, the drawing - is one piece of code compiled twice.
 */

#include "uapi.h"
#include "ugui.h"

#define W0 500
#define H0 280
#define HEAD_H 32             /* the heading and the three buttons */
#define BTN_H 22
#define TOOL_H 30             /* the second row: Up, New folder, Cut, Copy, Paste */
#define TREE_W 140            /* the folder tree down the left */
#define LIST_X 141            /* the list, right of the tree and its line */
#define ROW_Y0 68             /* the first row: under both bars, and a gap */
#define NAME_X 177            /* a name, right of its picture */
#define ROW_H 20
#define FOOT_H 24             /* the status line along the bottom */
#define MAXF 64               /* names kept; the window scrolls through them */
#define NAME_LEN 32
#define EDIT_W 56             /* the three buttons, kept at the right edge */
#define DEL_W 70
#define NAME_W 70
#define UP_W 40               /* the tools, from the left edge */
#define NEW_W 86
#define CUT_W 44
#define COPY_W 50
#define PASTE_W 54
#define TOOL_GAP 6
#define TREE_Y0 70            /* the tree's first line */
#define TREE_ROW_H 18
#define TREE_STEP 12          /* how far in a folder sits from its parent */
#define MAXNODES 48
#define PATH_LEN 64
#define DOUBLE_TICKS 50       /* two clicks this close (100 Hz) are one double */

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
/* A new name, and a new folder's name, are asked for in the status
   line, the way the editor asks for a file name: a window to move,
   focus and close, for one word of text, is a window nobody wants. */
#define ASK_RENAME 1
#define ASK_FOLDER 2
static int asking;            /* 0, or which of the two */
static char answer[NAME_LEN];
static int alen;
static char note[48];
static char here[PATH_LEN];   /* the directory we are looking at */

/* What Cut or Copy picked up: the whole path, because the paste
   happens somewhere else */
static char clip[PATH_LEN];
static int clip_cut;
static int clip_dir;

/* A double click is two clicks on one row, close together */
static uint32_t last_click;
static int last_row = -1;

/* The tree: every folder on the way to here, and every folder a person
   opened by hand, flattened into the lines it is drawn as */
struct node {
    char path[PATH_LEN];
    int depth;
    int open;
};
static struct node nodes[MAXNODES];
static int nnodes;
static int ttop;              /* the first line of the tree shown */
static char opened[MAXNODES][PATH_LEN];
static int nopened;

static int cw = W0, ch = H0;
static int winid = -1;

/* The buttons hold the right-hand edge, whatever the window has been
   pulled to - nailed to a number they end up in the middle of a
   maximised window. */
static int del_x(void)  { return cw - 10 - DEL_W; }
static int edit_x(void) { return del_x() - 6 - EDIT_W; }
static int name_x(void) { return edit_x() - 6 - NAME_W; }

/* The tools sit from the left, one after the other */
#define TOOL_UP    0
#define TOOL_NEW   1
#define TOOL_CUT   2
#define TOOL_COPY  3
#define TOOL_PASTE 4
#define TOOLS      5
static const int tool_w[TOOLS] = { UP_W, NEW_W, CUT_W, COPY_W, PASTE_W };
static const char *const tool_name[TOOLS] = {
    "Up", "New folder", "Cut", "Copy", "Paste"
};

static int tool_x(int t)
{
    int x = 10;
    for (int i = 0; i < t; i++)
        x += tool_w[i] + TOOL_GAP;
    return x;
}

static int rows_shown(void)
{
    int n = (ch - ROW_Y0 - FOOT_H - 2) / ROW_H;
    return n < 1 ? 1 : n;
}

static int tree_shown(void)
{
    int n = (ch - TREE_Y0 - FOOT_H - 2) / TREE_ROW_H;
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

static int slen(const char *s)
{
    int n = 0;
    while (s[n])
        n++;
    return n;
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

/* "dir" and "name" as one path; 0 when it does not fit */
static int join(char *out, const char *dir, const char *name)
{
    int p = 0;
    for (int i = 0; dir[i]; i++)
        out[p++] = dir[i];
    if (p == 0 || out[p - 1] != '/')
        out[p++] = '/';
    for (int i = 0; name[i]; i++) {
        if (p >= PATH_LEN - 1)
            return 0;
        out[p++] = name[i];
    }
    out[p] = '\0';
    return 1;
}

/* The last part of a path - what a folder is called in the tree */
static const char *last_part(const char *path)
{
    const char *n = path;
    for (const char *s = path; *s; s++)
        if (*s == '/' && s[1])
            n = s + 1;
    return n;
}

/* Whether 'path' stands directly in the folder 'dir' */
static int lives_in(const char *path, const char *dir)
{
    int dl = (int)(last_part(path) - path);   /* up to and with the slash */
    if (dl > 1)
        dl--;                          /* "/a/b" -> "/a", but "/b" -> "/" */
    if (dl != slen(dir))
        return 0;
    for (int i = 0; i < dl; i++)
        if (path[i] != dir[i])
            return 0;
    return 1;
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

/* What the system keeps for itself stays out of sight unless the
   'hidden' setting asks for it: the swap file and the settings in the
   root are nothing a person opens, and a list that starts with them
   invites removing them. Names starting with a dot are hidden the way
   they are everywhere else. */
static int show_hidden;

static void read_show_hidden(void)
{
    struct cfg_entry c;
    show_hidden = 0;
    for (uint32_t i = 0; sys_cfg_get(i, &c) == 0; i++)
        if (same(c.name, "hidden"))
            show_hidden = c.value[0] == '1';
}

static int hidden(const char *dir, const char *name)
{
    static const char *const system_files[] = {
        "system.cfg", "swap.sys", "icons.pos", "update.key", "desktop.icons"
    };
    if (show_hidden)
        return 0;
    if (name[0] == '.')
        return 1;
    if (!same(dir, "/"))
        return 0;
    for (int i = 0; i < (int)(sizeof(system_files) / sizeof(system_files[0])); i++)
        if (same(name, system_files[i]))
            return 1;
    return 0;
}

/* --- the tree ------------------------------------------------------ */

static int is_opened(const char *path)
{
    for (int i = 0; i < nopened; i++)
        if (same(opened[i], path))
            return 1;
    return 0;
}

static void set_opened(const char *path, int on)
{
    for (int i = 0; i < nopened; i++)
        if (same(opened[i], path)) {
            if (!on)                   /* the last one into its place */
                ecopy(opened[i], opened[--nopened], PATH_LEN);
            return;
        }
    if (on && nopened < MAXNODES)
        ecopy(opened[nopened++], path, PATH_LEN);
}

/* Every folder on the way to here is open: the tree shows where one
   is, whatever was closed by hand before. */
static void open_the_way(void)
{
    char part[PATH_LEN];
    set_opened("/", 1);
    for (int i = 1; here[i - 1]; i++)
        if (here[i] == '/' || here[i] == '\0') {
            for (int j = 0; j < i; j++)
                part[j] = here[j];
            part[i] = '\0';
            set_opened(part, 1);
        }
}

/* The tree, read by walking: the root, and below every open folder
   the folders in it, put straight after it - so that reading on down
   the array reaches them next, and their own children land below
   them in turn. No recursion, and no list per level on the stack. */
static void build_tree(void)
{
    nnodes = 1;
    ecopy(nodes[0].path, "/", PATH_LEN);
    nodes[0].depth = 0;
    nodes[0].open = 0;
    for (int i = 0; i < nnodes; i++) {
        nodes[i].open = is_opened(nodes[i].path);
        if (!nodes[i].open || sys_chdir(nodes[i].path) < 0)
            continue;
        int at = i + 1;
        struct entry e;
        for (int j = 0; j < 128 && nnodes < MAXNODES; j++) {
            if (read_entry(j, &e) != 0)
                break;
            if (!e.is_dir || hidden(nodes[i].path, e.name))
                continue;
            char p[PATH_LEN];
            if (!join(p, nodes[i].path, e.name))
                continue;
            for (int k = nnodes; k > at; k--)
                nodes[k] = nodes[k - 1];
            ecopy(nodes[at].path, p, PATH_LEN);
            nodes[at].depth = nodes[i].depth + 1;
            nodes[at].open = 0;
            nnodes++;
            at++;
        }
    }
    /* Back to where the list is. If that folder has gone - removed
       from a terminal while it was being shown - the root is where
       there still is something to show. */
    if (sys_chdir(here) < 0) {
        sys_chdir("/");
        ecopy(here, "/", sizeof(here));
    }
    if (ttop > nnodes - 1)
        ttop = 0;
}

/* --- the list ------------------------------------------------------ */

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
    read_show_hidden();
    where_am_i();                      /* what hidden() compares with */
    count = 0;
    for (int i = 0; i < 128 && count < MAXF; i++) {
        if (read_entry(i, &items[count]) != 0)
            break;
        if (!hidden(here, items[count].name))
            count++;
    }
    if (top > count - 1)
        top = 0;
    if (chosen >= count)
        chosen = -1;
    build_tree();
}

/* Walking somewhere: into a folder of the list, up, or to a line of
   the tree. The way there opens in the tree. */
static int go_to(const char *path)
{
    if (sys_chdir(path) < 0) {
        ecopy(note, "cannot go in there", sizeof(note));
        return -1;
    }
    chosen = -1;
    top = 0;
    last_row = -1;
    note[0] = '\0';
    where_am_i();
    open_the_way();
    reread();
    return 0;
}

static int listed(const char *name)
{
    for (int i = 0; i < count; i++)
        if (same(items[i].name, name))
            return 1;
    return 0;
}

static int pick(const char *name)
{
    for (int i = 0; i < count; i++)
        if (same(items[i].name, name))
            return chosen = i;
    return -1;
}

/* --- what the buttons do ------------------------------------------ */

static void open_chosen(void)
{
    if (chosen < 0 || chosen >= count)
        return;
    if (items[chosen].is_dir) {
        go_to(items[chosen].name);
        return;
    }
    /* A file goes to the program that opens it: a picture to the
       viewer, everything else to the editor. The name is what tells
       them apart - looking inside the file to decide would mean
       reading every file in the directory to draw a list. */
    const char *nm = items[chosen].name;
    int ln = slen(nm);
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

/* Whether the folder 'name' in here has anything in it - the kernel
   removes only an empty one, and saying so before asking is better
   than asking and then failing. */
static int folder_empty(const char *name)
{
    if (sys_chdir(name) < 0)
        return 0;
    struct entry e;
    int empty = read_entry(0, &e) != 0;
    sys_chdir(here);
    return empty;
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
    int dir = items[chosen].is_dir;
    if (dir && !folder_empty(items[chosen].name)) {
        ecopy(note, "the folder is not empty - not removing it",
              sizeof(note));
        return;
    }

    int p = 0;
    const char *q = dir ? "Remove the folder '" : "Remove '";
    for (int i = 0; q[i]; i++)
        ask_line[p++] = q[i];
    for (int i = 0; items[chosen].name[i] && p < 52; i++)
        ask_line[p++] = items[chosen].name[i];
    ask_line[p++] = '\'';
    ask_line[p++] = '?';
    ask_line[p] = '\0';

    ug_ask(&ask, ask_line, dir ? "It is empty. This cannot be undone."
                               : "This cannot be undone.", "Remove", "Keep");
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
    char gone[PATH_LEN];
    join(gone, here, items[chosen].name);
    if (sys_unlink(items[chosen].name) < 0) {
        ecopy(note, "it cannot be removed", sizeof(note));
    } else {
        ecopy(note, "removed", sizeof(note));
        if (same(clip, gone))
            clip[0] = '\0';            /* nothing left to paste */
    }
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
    pick(what);
}

static void make_folder(const char *what)
{
    if (!what[0])
        return;
    if (listed(what) || sys_mkdir(what) < 0) {
        ecopy(note, "cannot - is the name taken?", sizeof(note));
        return;
    }
    ecopy(note, "folder made", sizeof(note));
    reread();
    pick(what);
}

static void go_up(void)
{
    if (same(here, "/")) {
        ecopy(note, "this is the root already", sizeof(note));
        return;
    }
    go_to("..");
}

static void take(int cut)
{
    if (chosen < 0 || chosen >= count) {
        ecopy(note, "no file picked", sizeof(note));
        return;
    }
    if (!cut && items[chosen].is_dir) {
        /* Copying a folder means copying everything in it, folder by
           folder - moving one is a rename, and that is what Cut is. */
        ecopy(note, "a folder can be cut and pasted, not copied",
              sizeof(note));
        return;
    }
    if (!join(clip, here, items[chosen].name)) {
        ecopy(note, "the path is too long", sizeof(note));
        clip[0] = '\0';
        return;
    }
    clip_cut = cut;
    clip_dir = items[chosen].is_dir;
    ecopy(note, cut ? "cut - now Paste where it goes"
                    : "copied - now Paste where it goes", sizeof(note));
}

/* A copy in the folder its original is in needs a name of its own:
   "report.txt" becomes "report-copy.txt", then "report-copy2.txt". */
static int free_name(char *out, const char *name)
{
    if (!listed(name)) {
        ecopy(out, name, NAME_LEN);
        return 1;
    }
    int ln = slen(name), dot = ln;
    for (int i = ln - 1; i > 0; i--)
        if (name[i] == '.') {
            dot = i;
            break;
        }
    for (int k = 1; k < 10; k++) {
        char suffix[8] = "-copy";
        if (k > 1) {
            suffix[5] = (char)('0' + k);
            suffix[6] = '\0';
        }
        int p = 0;
        for (int i = 0; i < dot && p < NAME_LEN - 12; i++)
            out[p++] = name[i];
        for (int i = 0; suffix[i]; i++)
            out[p++] = suffix[i];
        for (int i = dot; i < ln && p < NAME_LEN - 1; i++)
            out[p++] = name[i];
        out[p] = '\0';
        if (!listed(out))
            return 1;
    }
    return 0;
}

static char chunk[4096];

/* A copy is the bytes, read and written again: two files that share
   blocks would be one file with two names. */
static int copy_file(const char *from, const char *to)
{
    int in = (int)sys_open(from, O_READ);
    if (in < 0)
        return -1;
    int out = (int)sys_open(to, O_WRITE | O_CREATE | O_TRUNC);
    if (out < 0) {
        sys_close(in);
        return -1;
    }
    int ok = 0;
    for (;;) {
        int n = (int)sys_read(in, chunk, sizeof(chunk));
        if (n < 0) {
            ok = -1;
            break;
        }
        if (n == 0)
            break;
        if ((int)sys_write(out, chunk, (uint32_t)n) != n) {
            ok = -1;                   /* the disk is full */
            break;
        }
    }
    sys_close(in);
    sys_close(out);
    if (ok < 0)
        sys_unlink(to);                /* not half a file */
    return ok;
}

static void paste(void)
{
    if (!clip[0]) {
        ecopy(note, "nothing to paste - Cut or Copy first", sizeof(note));
        return;
    }
    const char *name = last_part(clip);
    char target[NAME_LEN];
    if (clip_cut) {
        if (lives_in(clip, here)) {
            ecopy(note, "it is in this folder already", sizeof(note));
            return;
        }
        ecopy(target, name, NAME_LEN);
        /* A rename across folders: the blocks stay where they are, and
           the kernel refuses a folder into itself or below it. */
        if (listed(target) || sys_rename(clip, target) < 0) {
            ecopy(note, listed(target) ? "the name is taken here"
                                       : "it cannot go here", sizeof(note));
            return;
        }
        ecopy(note, clip_dir ? "folder moved here" : "moved here",
              sizeof(note));
        clip[0] = '\0';
    } else {
        if (!free_name(target, name) || copy_file(clip, target) < 0) {
            ecopy(note, "it cannot be copied here", sizeof(note));
            reread();
            return;
        }
        ecopy(note, "copied here", sizeof(note));
    }
    reread();
    pick(target);
}

static void ask_for(int what)
{
    if (what == ASK_RENAME && chosen < 0) {
        ecopy(note, "no file picked", sizeof(note));
        return;
    }
    asking = what;
    alen = 0;
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

/* As much of 's' as fits in 'w' pixels, ending in ".." if cut */
static void fit(char *out, int max, const char *s, int w, int font, int px)
{
    ecopy(out, s, max);
    if (ud_width(out, font, px) <= w)
        return;
    int n = slen(out);
    if (n > max - 3)
        n = max - 3;
    while (n > 1) {
        n--;
        out[n] = '.';
        out[n + 1] = '.';
        out[n + 2] = '\0';
        if (ud_width(out, font, px) <= w)
            return;
    }
}

static void draw_tree(int win, const struct ud_look *l)
{
    int y0 = HEAD_H + TOOL_H + 1;
    ud_fill(win, 0, y0, TREE_W, ch - FOOT_H - y0, l->row);
    ud_fill(win, TREE_W, y0, 1, ch - FOOT_H - y0, l->line);
    int shown = tree_shown();
    for (int i = 0; i < shown; i++) {
        int idx = ttop + i;
        if (idx >= nnodes)
            break;
        const struct node *n = &nodes[idx];
        int y = TREE_Y0 + i * TREE_ROW_H;
        int x = 8 + n->depth * TREE_STEP;
        int here_one = same(n->path, here);
        if (here_one)
            ud_round(win, 4, y, TREE_W - 8, TREE_ROW_H, 5, l->accent);
        uint32_t ink = here_one ? l->on_accent : l->text;
        uint32_t quiet = here_one ? l->on_accent : l->dim;

        /* The arrow: down when the folder is open, right when not */
        int ay = y + TREE_ROW_H / 2;
        if (n->open) {
            ud_line(win, x, ay - 2, x + 3, ay + 1, 20, quiet);
            ud_line(win, x + 3, ay + 1, x + 6, ay - 2, 20, quiet);
        } else {
            ud_line(win, x + 2, ay - 3, x + 5, ay, 20, quiet);
            ud_line(win, x + 5, ay, x + 2, ay + 3, 20, quiet);
        }
        uint32_t fc = here_one ? l->on_accent : 0x5AB4F5;
        ud_round(win, x + 11, y + 4, 5, 3, 1, fc);
        ud_round(win, x + 11, y + 6, 12, 8, 2, fc);

        char label[NAME_LEN + 4];
        fit(label, (int)sizeof(label), idx == 0 ? "Disk" : last_part(n->path),
            TREE_W - (x + 28) - 6, here_one ? UD_BOLD : UD_SANS, 11);
        ud_text(win, x + 28, ud_baseline(y, TREE_ROW_H, 11), label, ink,
                here_one ? UD_BOLD : UD_SANS, 11);
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
                   asking == ASK_RENAME ? l.danger : l.row,
                   asking == ASK_RENAME ? l.on_accent : l.text);
    ug_draw_button(win, &l, edit_x(), by, EDIT_W, BTN_H, "Open", l.accent,
                   l.on_accent);
    ug_draw_button(win, &l, del_x(), by, DEL_W, BTN_H, "Remove",
                   ask.on ? l.danger : l.row, ask.on ? l.on_accent : l.danger);
    ud_fill(win, 0, HEAD_H, cw, 1, l.line);

    /* The tools, and what is waiting to be pasted */
    int ty = HEAD_H + 1 + (TOOL_H - 1 - BTN_H) / 2;
    for (int t = 0; t < TOOLS; t++) {
        int lit = (t == TOOL_NEW && asking == ASK_FOLDER) ||
                  (t == TOOL_PASTE && clip[0]);
        ug_draw_button(win, &l, tool_x(t), ty, tool_w[t], BTN_H, tool_name[t],
                       lit ? l.accent : l.row, lit ? l.on_accent : l.text);
    }
    if (clip[0]) {
        char what[NAME_LEN + 12];
        int q = 0;
        const char *pre = clip_cut ? "cut: " : "copied: ";
        for (int i = 0; pre[i]; i++)
            what[q++] = pre[i];
        ecopy(what + q, last_part(clip), (int)sizeof(what) - q);
        int x = tool_x(TOOLS) + 6;
        char shown_as[NAME_LEN + 12];
        fit(shown_as, (int)sizeof(shown_as), what, cw - x - 12, UD_SANS, 11);
        ud_text(win, x, ud_baseline(HEAD_H + 1, TOOL_H - 1, 11), shown_as,
                l.dim, UD_SANS, 11);
    }
    ud_fill(win, 0, HEAD_H + TOOL_H, cw, 1, l.line);

    draw_tree(win, &l);

    for (int i = 0; i < shown; i++) {
        int idx = top + i;
        if (idx >= count)
            break;
        int y = ROW_Y0 + i * ROW_H;
        int picked = idx == chosen;
        if (picked)
            ud_round(win, LIST_X + 6, y, cw - LIST_X - 12, ROW_H, 6, l.accent);
        uint32_t ink = picked ? l.on_accent : l.text;
        uint32_t quiet = picked ? l.on_accent : l.dim;
        int base = ud_baseline(y, ROW_H, UD_PX);
        draw_kind(win, &l, LIST_X + 12, y, items[idx].is_dir, picked);
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
        const char *q = asking == ASK_FOLDER ? "Folder name: " : "New name: ";
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
        ud_text(win, 12, fb, "A double click opens, Backspace goes up",
                l.dim, UD_SANS, 11);
    }

    /* Last, and therefore over everything: a question one can scroll
       away from is not a question. */
    ui.win = win;
    ui.w = cw;
    ui.h = ch;
    ug_modal_draw(&ui, &ask);
}

/* --- the mouse ----------------------------------------------------- */

static void clicked(int x, int y)
{
    int body = HEAD_H + TOOL_H;
    if (y < HEAD_H) {
        if (x >= del_x())
            delete_chosen();
        else if (x >= edit_x())
            open_chosen();
        else if (x >= name_x())
            ask_for(ASK_RENAME);
        return;
    }
    if (y < body) {
        for (int t = 0; t < TOOLS; t++) {
            if (x < tool_x(t) || x >= tool_x(t) + tool_w[t])
                continue;
            if (t == TOOL_UP)
                go_up();
            else if (t == TOOL_NEW)
                ask_for(ASK_FOLDER);
            else if (t == TOOL_CUT)
                take(1);
            else if (t == TOOL_COPY)
                take(0);
            else
                paste();
        }
        return;
    }
    if (y >= ch - FOOT_H)
        return;

    if (x < TREE_W) {
        int i = (y - TREE_Y0) / TREE_ROW_H + ttop;
        if (y < TREE_Y0 || i >= nnodes)
            return;
        /* The arrow opens and closes; the name goes there */
        int ax = 8 + nodes[i].depth * TREE_STEP;
        if (x < ax + 10) {
            set_opened(nodes[i].path, !nodes[i].open);
            build_tree();
        } else {
            char path[PATH_LEN];
            ecopy(path, nodes[i].path, PATH_LEN);   /* the tree is rebuilt */
            go_to(path);
        }
        return;
    }

    if (y >= ROW_Y0) {
        int r = (y - ROW_Y0) / ROW_H + top;
        if (r < 0 || r >= count)
            return;
        uint32_t now = sys_getticks();
        if (r == last_row && r == chosen && now - last_click < DOUBLE_TICKS) {
            last_row = -1;
            open_chosen();
            return;
        }
        chosen = r;
        last_row = r;
        last_click = now;
        note[0] = '\0';
    }
}

void _start(void)
{
    int win = (int)win_create(W0, H0, "Files");
    if (win < 0)
        sys_exit(1);
    winid = win;

    where_am_i();
    open_the_way();
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
            } else if (ev.type == WEV_SCROLL && ev.x < TREE_W) {
                /* The pointer says which of the two is meant */
                int shown = tree_shown();
                ttop -= ev.val * 3;
                if (ttop > nnodes - shown)
                    ttop = nnodes - shown;
                if (ttop < 0)
                    ttop = 0;
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
                        int what = asking;
                        asking = 0;
                        answer[alen] = '\0';
                        if (!alen)
                            ecopy(note, what == ASK_FOLDER ? "no folder made"
                                                           : "the name is unchanged",
                                  sizeof(note));
                        else if (what == ASK_FOLDER)
                            make_folder(answer);
                        else
                            rename_to(answer);
                        alen = 0;
                    } else if (c == '\b') {
                        if (alen)
                            alen--;
                        else
                            asking = 0;      /* nothing typed = never mind */
                    } else if (c >= 0x20 && c < 0x7F && c != '/' &&
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
                clicked(ev.x, ev.y);
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
            int was = count, was_tree = nnodes;
            char kept[48];
            ecopy(kept, note, sizeof(kept));
            reread();
            ecopy(note, kept, sizeof(note));
            if (was != count || was_tree != nnodes)
                again = 1;
        }
        if (again)
            draw();
        sys_sleep(100);
    }
}
