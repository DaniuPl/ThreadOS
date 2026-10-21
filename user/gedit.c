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
#include "ulocale.h"
#ifdef GCODE_IDE
#include "uterm.h"
#endif

/* Drawn the way the desktop draws, in its theme: the page is the
   theme's field, the text DejaVu Sans Mono on a grid of cells (ud_cells)
   - an editor's columns have to stay columns - with the line numbers in
   a margin of their own, and the status line along the bottom. */
#define CELL     8            /* a column */
#define TEXT_PX  12
#define TEXT_BASE 12          /* the baseline, from a row's top */
#define GUTTER   34           /* the line numbers' margin */

#ifdef GCODE_IDE
#define IDE_W0 560
#define IDE_H0 360
#define IDE_ACTIVITY_W 32
#define IDE_SIDE_W 188
#define IDE_HEADER_H 32
#define IDE_TAB_H 26
#define IDE_BREAD_H 20
#define IDE_EDIT_TOP 80
#define IDE_TEXT_X 228
#define IDE_LIST_ROW_H 18
#define IDE_TAB_N 3
#define IDE_TERM_H 128
#define IDE_TERM_HEADER_H 24
#define IDE_TERM_LINES 12
#define IDE_TERM_COLS 72
#define IDE_TERM_INPUT_MAX 96
#define IDE_TERM_HISTORY 8
#define IDE_TERM_OUTFILE ".code-term.out"
#define IDE_TERM_ERRFILE ".code-term.err"
#else
#define W0 440                /* the size the window opens with */
#define H0 320
#endif

#define MAXL 200              /* lines a file may have */
#define MAXC 96               /* and characters in a line */
#define ROW_H 16
#define PAD 4
#define TEXT_X 40             /* a line's first column: the margin and a gap */
#define STATUS_H 24
#define NAME_MAX 63

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
static char note[80];         /* the last thing that happened */
static char old_title[NAME_MAX + 12];

#ifdef GCODE_IDE
static int  cw = IDE_W0, ch = IDE_H0;
#else
static int  cw = W0, ch = H0; /* the content area, as the desktop says */
#endif
#ifdef GCODE_IDE
static char project_path[512];
static int project_top;
static char tabs[IDE_TAB_N][NAME_MAX + 1];
static int tab_n, tab_active = -1;
static int terminal_open, terminal_focus;
static int terminal_pid = -1, terminal_input_fd = -1;
static uint32_t terminal_out_shown, terminal_err_shown;
static char terminal_out_path[256], terminal_err_path[256];
static char terminal_input[IDE_TERM_INPUT_MAX];
static int terminal_len, terminal_cursor;
static char terminal_history[IDE_TERM_HISTORY][IDE_TERM_INPUT_MAX];
static int terminal_history_n, terminal_history_pos = -1;
static char terminal_lines[IDE_TERM_LINES][IDE_TERM_COLS + 1];
static int terminal_line_n = 1, terminal_line_len, terminal_scroll;
static unsigned char file_is_dir[64];
#endif

/* Asking for a file name: the status line becomes the question and
   what is typed goes into 'answer' until Enter. 0 = editing. */
#define ASK_NONE 0
#define ASK_SAVE 1
#define ASK_OPEN 2
#define ASK_FIND 3
#ifdef GCODE_IDE
#define ASK_NEW  4
#endif
static int  asking;
static char answer[NAME_MAX + 1];
static int  alen;
/* What was looked for last time. Ctrl+F and Enter on an empty line
   means "the same thing again", which is what a person expects after
   the first match is not the one they wanted. */
static char last_find[NAME_MAX + 1];

static int editor_x(void)
{
#ifdef GCODE_IDE
    return IDE_TEXT_X;
#else
    return TEXT_X;
#endif
}

static int editor_top(void)
{
#ifdef GCODE_IDE
    return IDE_EDIT_TOP;
#else
    return PAD;
#endif
}

#ifdef GCODE_IDE
static int ide_terminal_height(void)
{
    int available = ch - STATUS_H - IDE_EDIT_TOP - ROW_H;
    if (available < IDE_TERM_HEADER_H + ROW_H * 2)
        return 0;
    return available < IDE_TERM_H ? available : IDE_TERM_H;
}

static int ide_terminal_top(void)
{
    return ch - STATUS_H - ide_terminal_height();
}
#endif

static int rows(void)
{
    int height = ch - editor_top() - PAD - STATUS_H;
#ifdef GCODE_IDE
    if (terminal_open)
        height -= ide_terminal_height();
#endif
    return height > 0 ? height / ROW_H : 0;
}

static void ecopy(char *d, const char *s, int max)
{
    int i = 0;
    while (s[i] && i < max) {
        d[i] = s[i];
        i++;
    }
    d[i] = '\0';
}

static int text_equal(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return !*a && !*b;
}

static const char *note_for_language(const char *text)
{
    if (!ulocale_language_polish())
        return text;
    if (text_equal(text, "there is nothing to undo"))
        return "Nie ma czego cofnąć";
    if (text_equal(text, "undone"))
        return "Cofnięto";
    if (text_equal(text, "it cannot be written"))
        return "Nie można zapisać pliku";
    if (text_equal(text, "saved"))
        return "Zapisano";
    if (text_equal(text, "there is no such file"))
        return "Nie ma takiego pliku";
    if (text_equal(text, "loaded"))
        return "Wczytano";
    if (text_equal(text, "there is nothing to look for"))
        return "Nie wpisano tekstu do wyszukania";
    if (text_equal(text, "not found"))
        return "Nie znaleziono";
    if (text_equal(text, "the line was copied"))
        return "Skopiowano wiersz";
    if (text_equal(text, "Ctrl+O opens, Ctrl+S saves, Ctrl+F finds, "
                         "Ctrl+Z undoes"))
        return "Ctrl+O otwiera, Ctrl+S zapisuje, Ctrl+F szuka, Ctrl+Z cofa";
    if (text_equal(text, "Unsaved changes! Ctrl+S will save, closing again "
                         "will not"))
        return "Niezapisane zmiany! Ctrl+S zapisuje, zamknięcie odrzuca";
    if (text_equal(text, "Save before changing files"))
        return "Zapisz plik przed przełączeniem";
    if (text_equal(text, "Project folder changed"))
        return "Zmieniono folder projektu";
    if (text_equal(text, "Project folder is not available"))
        return "Folder projektu jest niedostępny";
    if (text_equal(text, "Terminal opened at the project folder"))
        return "Terminal otwarto w folderze projektu";
    if (text_equal(text, "Terminal is already open"))
        return "Terminal jest już otwarty";
    if (text_equal(text, "Could not open the terminal"))
        return "Nie udało się otworzyć terminala";
    if (text_equal(text, "The project terminal ended"))
        return "Terminal projektu został zamknięty";
    if (text_equal(text, "No project file is open"))
        return "Nie otwarto pliku projektu";
    if (text_equal(text, "New file; save to create it"))
        return "Nowy plik — zapisz, aby go utworzyć";
    if (text_equal(text, "That file already exists"))
        return "Taki plik już istnieje";
    if (text_equal(text, "Choose a file from Project"))
        return "Wybierz plik z panelu Projekt";
    if (text_equal(text, "No compiler is bundled; Terminal opens in this project"))
        return "Brak kompilatora; Terminal otworzy się w tym projekcie";
    return text;
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
#ifdef GCODE_IDE
    if (project_path[0] && !text_equal(project_path, "/")) {
        ecopy(names[nfiles], "..", NAME_MAX);
        file_is_dir[nfiles++] = 1;
    }
#endif
    for (int i = 0; i < 128 && nfiles < FILES_MAX; i++) {
#ifdef __x86_64__
        struct dirent64 e;
        if (sys_readdir(i, &e) != 0)
            break;
#ifdef GCODE_IDE
        if (!e.is_dir &&
            (text_equal(e.name, IDE_TERM_OUTFILE) ||
             text_equal(e.name, IDE_TERM_ERRFILE)))
            continue;
#endif
        if (e.is_dir) {
#ifdef GCODE_IDE
            ecopy(names[nfiles], e.name, NAME_MAX);
            file_is_dir[nfiles++] = 1;
#endif
            continue;
        }
        ecopy(names[nfiles], e.name, NAME_MAX);
#else
        char name[FS_NAME_MAX + 1];
        int32_t r = sys_readdir(i, name);
        if (r == -1)
            break;
        if (r == -2) {
#ifdef GCODE_IDE
            ecopy(names[nfiles], name, NAME_MAX);
            file_is_dir[nfiles++] = 1;
#endif
            continue;                    /* a directory, not a file */
        }
#ifdef GCODE_IDE
        if (text_equal(name, IDE_TERM_OUTFILE) ||
            text_equal(name, IDE_TERM_ERRFILE))
            continue;
#endif
        ecopy(names[nfiles], name, NAME_MAX);
#endif
#ifdef GCODE_IDE
        file_is_dir[nfiles] = 0;
#endif
        nfiles++;
    }
}

#ifdef GCODE_IDE
static void project_read_path(void)
{
#ifdef __x86_64__
    sys_getcwd(project_path, sizeof(project_path));
#else
    sys_getcwd(project_path);
#endif
    if (!project_path[0])
        ecopy(project_path, "/", sizeof(project_path));
}

static void ide_tabs_reset(void)
{
    tab_n = 0;
    tab_active = -1;
    project_top = 0;
}

static void ide_register_tab(const char *name)
{
    if (!name || !name[0])
        return;
    for (int i = 0; i < tab_n; i++) {
        if (text_equal(tabs[i], name)) {
            tab_active = i;
            return;
        }
    }
    if (tab_n == IDE_TAB_N) {
        int victim = tab_active == 0 ? 1 : 0;
        for (int i = victim; i < tab_n - 1; i++)
            ecopy(tabs[i], tabs[i + 1], sizeof(tabs[i]));
        if (tab_active > victim)
            tab_active--;
        else if (tab_active == victim)
            tab_active = -1;
        tab_n--;
    }
    ecopy(tabs[tab_n], name, sizeof(tabs[tab_n]));
    tab_active = tab_n++;
}

static void load_from(const char *name);
static void save_to(const char *name);
static void clear_all(void);
static void retitle(void);

static int ide_open_file(const char *name)
{
    if (text_equal(name, fname))
        return 1;
    if (dirty) {
        ecopy(note, "Save before changing files", sizeof(note) - 1);
        return 0;
    }
    load_from(name);
    return text_equal(fname, name);
}

static void ide_open_item(int index)
{
    if (index < 0 || index >= nfiles)
        return;
    if (dirty) {
        ecopy(note, "Save before changing files", sizeof(note) - 1);
        return;
    }
    if (file_is_dir[index]) {
        if (sys_chdir(names[index]) < 0) {
            ecopy(note, "Project folder is not available", sizeof(note) - 1);
            return;
        }
        clear_all();
        fname[0] = '\0';
        ide_tabs_reset();
        project_read_path();
        reread_files();
        retitle();
        ecopy(note, "Project folder changed", sizeof(note) - 1);
        return;
    }
    ide_open_file(names[index]);
}

static void ide_activate_tab(int index)
{
    if (index < 0 || index >= tab_n || index == tab_active)
        return;
    if (dirty) {
        ecopy(note, "Save before changing files", sizeof(note) - 1);
        return;
    }
    ide_open_file(tabs[index]);
}

static int ide_term_join_path(char *dst, int cap, const char *dir,
                              const char *leaf)
{
    int n = 0;
    while (dir[n] && n < cap - 1) {
        dst[n] = dir[n];
        n++;
    }
    if (dir[n])
        return 0;
    if (!n || dst[n - 1] != '/') {
        if (n >= cap - 1)
            return 0;
        dst[n++] = '/';
    }
    for (int i = 0; leaf[i]; i++) {
        if (n >= cap - 1)
            return 0;
        dst[n++] = leaf[i];
    }
    dst[n] = '\0';
    return 1;
}

static void ide_term_newline(void)
{
    if (terminal_line_n < IDE_TERM_LINES) {
        terminal_line_n++;
    } else {
        for (int row = 1; row < IDE_TERM_LINES; row++)
            ecopy(terminal_lines[row - 1], terminal_lines[row],
                  IDE_TERM_COLS);
    }
    terminal_line_len = 0;
    terminal_lines[terminal_line_n - 1][0] = '\0';
    terminal_scroll = 0;
}

static void ide_term_put(char c)
{
    if (c == '\n') {
        ide_term_newline();
        return;
    }
    if (c == '\r') {
        terminal_line_len = 0;
        terminal_lines[terminal_line_n - 1][0] = '\0';
        return;
    }
    if (c == '\b') {
        if (terminal_line_len > 0) {
            terminal_lines[terminal_line_n - 1][--terminal_line_len] = '\0';
        }
        return;
    }
    if (c == '\t') {
        for (int i = 0; i < 4; i++)
            ide_term_put(' ');
        return;
    }
    if ((unsigned char)c < 0x20 || c == 0x7F)
        return;
    if (terminal_line_len == IDE_TERM_COLS)
        ide_term_newline();
    terminal_lines[terminal_line_n - 1][terminal_line_len++] = c;
    terminal_lines[terminal_line_n - 1][terminal_line_len] = '\0';
}

static void ide_term_text(const char *text)
{
    for (int i = 0; text[i]; i++)
        ide_term_put(text[i]);
}

static int ide_term_start(void)
{
    if (terminal_pid >= 0)
        return 1;
    if (!ide_term_join_path(terminal_out_path,
                            (int)sizeof(terminal_out_path), project_path,
                            IDE_TERM_OUTFILE)) {
        ecopy(note, "Project path is too long for Terminal", sizeof(note) - 1);
        return 0;
    }
#ifdef __x86_64__
    if (!ide_term_join_path(terminal_err_path,
                            (int)sizeof(terminal_err_path), project_path,
                            IDE_TERM_ERRFILE)) {
        ecopy(note, "Project path is too long for Terminal", sizeof(note) - 1);
        return 0;
    }
    const char *shell = "ush64";
    const char *errfile = IDE_TERM_ERRFILE;
#else
    terminal_err_path[0] = '\0';
    const char *shell = "sh";
    const char *errfile = (const char *)0;
#endif
    int input_fd = -1;
    int32_t pid = uterm_start_one(shell, IDE_TERM_OUTFILE, errfile, &input_fd);
    if (pid < 0) {
        ide_term_text("Could not start the project shell\n");
        ecopy(note, "Could not start the project shell", sizeof(note) - 1);
        return 0;
    }
    terminal_pid = pid;
    terminal_input_fd = input_fd;
    terminal_out_shown = 0;
    terminal_err_shown = 0;
    return 1;
}

static void ide_terminal_toggle(void)
{
    terminal_open = !terminal_open;
    terminal_focus = terminal_open;
    if (terminal_open && !ide_terminal_height()) {
        terminal_open = 0;
        terminal_focus = 0;
        ecopy(note, "Terminal needs a taller Code window", sizeof(note) - 1);
        return;
    }
    if (terminal_open && terminal_pid < 0)
        ide_term_start();
}

static int ide_term_read_file(const char *path, uint32_t *shown)
{
    if (!path[0])
        return 0;
    char buf[256];
    int changed = 0;
    for (int i = 0; i < 4; i++) {
        int32_t n = uterm_read_new(path, shown, buf, sizeof(buf));
        if (n <= 0)
            break;
        for (int32_t j = 0; j < n; j++)
            ide_term_put(buf[j]);
        changed = 1;
        if (n < (int32_t)sizeof(buf))
            break;
    }
    return changed;
}

static int ide_terminal_poll(void)
{
    if (terminal_pid < 0)
        return 0;

    int changed = ide_term_read_file(terminal_out_path,
                                     &terminal_out_shown);
    changed |= ide_term_read_file(terminal_err_path,
                                  &terminal_err_shown);

    int code = 0;
    int rc = sys_trywait(terminal_pid, &code);
    if (rc == -3)
        return changed;

    changed |= ide_term_read_file(terminal_out_path, &terminal_out_shown);
    changed |= ide_term_read_file(terminal_err_path, &terminal_err_shown);
    if (terminal_input_fd >= 0)
        sys_close(terminal_input_fd);
    terminal_input_fd = -1;
    terminal_pid = -1;
    if (rc == 0 && code != 0)
        ide_term_text("\n[process exited with an error]\n");
    else if (rc != 0)
        ide_term_text("\n[terminal process ended]\n");
    return 1;
}

static void ide_term_remember(void)
{
    if (!terminal_len)
        return;
    if (terminal_history_n > 0 &&
        text_equal(terminal_history[(terminal_history_n - 1) %
                                   IDE_TERM_HISTORY], terminal_input))
        return;
    ecopy(terminal_history[terminal_history_n % IDE_TERM_HISTORY],
          terminal_input, IDE_TERM_INPUT_MAX - 1);
    terminal_history_n++;
}

static void ide_term_submit(void)
{
    if (terminal_pid < 0 && !ide_term_start())
        return;
    if (terminal_input_fd < 0) {
        ide_term_text("Terminal input is closed\n");
        return;
    }

    ide_term_remember();

    char line[IDE_TERM_INPUT_MAX + 1];
    for (int i = 0; i < terminal_len; i++)
        line[i] = terminal_input[i];
    line[terminal_len] = '\n';
    int32_t written = uterm_write_input(terminal_input_fd, line,
                                        (uint32_t)terminal_len + 1);
    if (written != terminal_len + 1)
        ide_term_text("Could not write to the terminal process\n");

    terminal_input[0] = '\0';
    terminal_len = terminal_cursor = 0;
    terminal_history_pos = -1;
}

static void ide_term_history_up(void)
{
    int count = terminal_history_n < IDE_TERM_HISTORY
                    ? terminal_history_n : IDE_TERM_HISTORY;
    if (terminal_history_pos + 1 >= count)
        return;
    terminal_history_pos++;
    int index = (terminal_history_n - 1 - terminal_history_pos) %
                IDE_TERM_HISTORY;
    ecopy(terminal_input, terminal_history[index], IDE_TERM_INPUT_MAX - 1);
    terminal_len = 0;
    while (terminal_input[terminal_len])
        terminal_len++;
    terminal_cursor = terminal_len;
}

static void ide_term_history_down(void)
{
    if (terminal_history_pos <= 0) {
        terminal_history_pos = -1;
        terminal_input[0] = '\0';
        terminal_len = terminal_cursor = 0;
        return;
    }
    terminal_history_pos--;
    int index = (terminal_history_n - 1 - terminal_history_pos) %
                IDE_TERM_HISTORY;
    ecopy(terminal_input, terminal_history[index], IDE_TERM_INPUT_MAX - 1);
    terminal_len = 0;
    while (terminal_input[terminal_len])
        terminal_len++;
    terminal_cursor = terminal_len;
}

static void ide_term_key(char c)
{
    if (c == '\n' || c == '\r') {
        ide_term_submit();
        return;
    }
    if (c == 3 && terminal_pid >= 0) {
        sys_kill(terminal_pid, SIGINT);
        return;
    }
    if (c == 4 && terminal_input_fd >= 0) {
        sys_close(terminal_input_fd);
        terminal_input_fd = -1;
        ide_term_text("\n[terminal input closed]\n");
        return;
    }
    if (c == KEY_UP) {
        ide_term_history_up();
        return;
    }
    if (c == KEY_DOWN) {
        ide_term_history_down();
        return;
    }
    if (c == KEY_LEFT) {
        if (terminal_cursor > 0)
            terminal_cursor--;
        return;
    }
    if (c == KEY_RIGHT) {
        if (terminal_cursor < terminal_len)
            terminal_cursor++;
        return;
    }
    if (c == KEY_HOME) {
        terminal_cursor = 0;
        return;
    }
    if (c == KEY_END) {
        terminal_cursor = terminal_len;
        return;
    }
    if (c == '\b') {
        if (terminal_cursor > 0) {
            for (int i = terminal_cursor; i < terminal_len; i++)
                terminal_input[i - 1] = terminal_input[i];
            terminal_len--;
            terminal_cursor--;
            terminal_input[terminal_len] = '\0';
        }
        terminal_history_pos = -1;
        return;
    }
    if ((unsigned char)c >= 0x20 && c != 0x7F &&
        terminal_len < IDE_TERM_INPUT_MAX - 1) {
        for (int i = terminal_len; i > terminal_cursor; i--)
            terminal_input[i] = terminal_input[i - 1];
        terminal_input[terminal_cursor++] = c;
        terminal_len++;
        terminal_input[terminal_len] = '\0';
        terminal_history_pos = -1;
    }
}

#endif

static int cols(void) { return (cw - editor_x() - PAD) / CELL; }

/* --- letters of more than one byte --------------------------------------
 *
 * The text is UTF-8: a Polish letter is two bytes and one column. The
 * caret and the lines keep byte offsets - that is what the file is -
 * and these turn them into columns and back, so the caret never stands
 * in the middle of a letter. */
static int cont(char c) { return ((unsigned char)c & 0xC0) == 0x80; }

static int col_of(int l, int b)
{
    int n = 0;
    for (int i = 0; i < b && i < llen[l]; i++)
        if (!cont(text[l][i]))
            n++;
    return n;
}

static int byte_of(int l, int c)
{
    for (int i = 0; i < llen[l]; i++)
        if (!cont(text[l][i]) && c-- == 0)
            return i;
    return llen[l];
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

/* How far back one can go; the log itself is described below, where
   the steps are written. */
#define UNDO_N 120
struct step {
    unsigned char kind;
    unsigned char with_prev;  /* one letter of two bytes: undone together */
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

/* Set by whoever records the second byte of a letter */
static int join_next;

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
    log[log_n].with_prev = (unsigned char)join_next;
    join_next = 0;
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
    join_next = cont(c);          /* the rest of a letter goes with it */
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
        /* A whole letter: its last byte back to its first */
        int first = 1;
        for (;;) {
            char gone = text[cy][cx - 1];
            for (int i = cx - 1; i < llen[cy]; i++)
                text[cy][i] = text[cy][i + 1];
            llen[cy]--;
            cx--;
            join_next = !first;
            record(U_DEL, cy, cx, gone);
            first = 0;
            if (!cont(gone) || cx == 0)
                break;
        }
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
static void undo_step(void);

static void undo_one(void)
{
    if (!log_n) {
        ecopy(note, "there is nothing to undo", sizeof(note) - 1);
        return;
    }
    int more;
    do {
        more = log[log_n - 1].with_prev;
        undo_step();
    } while (more && log_n);
}

static void undo_step(void)
{
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
static int  winid = -1;

static void retitle(void)
{
    char t[NAME_MAX + 12];
    int p = 0;
#ifdef GCODE_IDE
    const char *pre = ulocale_language_polish() ? "Kod" : "Code";
#else
    const char *pre = ulocale_language_polish() ? "Edytor" : "Editor";
#endif
    for (int i = 0; pre[i]; i++)
        t[p++] = pre[i];
    if (fname[0]) {
        t[p++] = ':';
        t[p++] = ' ';
        for (int i = 0; fname[i] && p < 62; i++)
            t[p++] = fname[i];
    }
    t[p] = '\0';
    if (winid >= 0 && !text_equal(t, old_title)) {
        win_title(winid, t);
        ecopy(old_title, t, sizeof(old_title));
    }
}

static void save_to(const char *name)
{
    retitle();
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
#ifdef GCODE_IDE
    ide_register_tab(name);
    reread_files();
#endif
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
#ifdef GCODE_IDE
    ide_register_tab(name);
#endif
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
    int col = col_of(cy, cx);           /* hoff is in columns */
    if (col < hoff)
        hoff = col;
    if (col >= hoff + c)
        hoff = col - c + 1;
    if (hoff < 0)
        hoff = 0;
}

#ifdef GCODE_IDE
#define CODE_PLAIN 0
#define CODE_KEYWORD 1
#define CODE_STRING 2
#define CODE_COMMENT 3
#define CODE_NUMBER 4
#define CODE_PREPROC 5

static int code_word_start(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int code_word_char(char c)
{
    return code_word_start(c) || (c >= '0' && c <= '9');
}

static int code_keyword(const char *line, int start, int end)
{
    static const char *const words[] = {
        "auto", "break", "case", "char", "const", "continue", "default",
        "do", "double", "else", "enum", "extern", "float", "for", "goto",
        "if", "inline", "int", "long", "register", "return", "short",
        "signed", "sizeof", "static", "struct", "switch", "typedef",
        "union", "unsigned", "void", "volatile", "while", "asm", "bool",
        "class", "namespace", "public", "private", "uint8_t", "uint16_t",
        "uint32_t", "uint64_t", "int8_t", "int16_t", "int32_t", "int64_t"
    };
    int len = end - start;
    for (int w = 0; w < (int)(sizeof(words) / sizeof(words[0])); w++) {
        int i = 0;
        while (words[w][i] && i < len && words[w][i] == line[start + i])
            i++;
        if (i == len && !words[w][i])
            return 1;
    }
    return 0;
}

static void code_styles(const char *line, int len, unsigned char *style)
{
    for (int i = 0; i < len; i++)
        style[i] = CODE_PLAIN;

    int first = 0;
    while (first < len && (line[first] == ' ' || line[first] == '\t'))
        first++;
    if (first < len && line[first] == '#') {
        for (int i = first; i < len; i++)
            style[i] = CODE_PREPROC;
        return;
    }

    for (int i = 0; i < len;) {
        if (line[i] == '/' && i + 1 < len && line[i + 1] == '/') {
            for (; i < len; i++)
                style[i] = CODE_COMMENT;
            break;
        }
        if (line[i] == '/' && i + 1 < len && line[i + 1] == '*') {
            for (; i < len; i++)
                style[i] = CODE_COMMENT;
            break;
        }
        if (line[i] == '"' || line[i] == '\'') {
            char quote = line[i];
            int end = i++;
            style[end] = CODE_STRING;
            while (i < len) {
                style[i] = CODE_STRING;
                if (line[i] == '\\' && i + 1 < len) {
                    style[++i] = CODE_STRING;
                    i++;
                } else if (line[i++] == quote) {
                    break;
                }
            }
            continue;
        }
        if (code_word_start(line[i])) {
            int start = i++;
            while (i < len && code_word_char(line[i]))
                i++;
            if (code_keyword(line, start, i))
                for (int j = start; j < i; j++)
                    style[j] = CODE_KEYWORD;
            continue;
        }
        if (line[i] >= '0' && line[i] <= '9') {
            int start = i++;
            while (i < len && code_word_char(line[i]))
                i++;
            for (int j = start; j < i; j++)
                style[j] = CODE_NUMBER;
            continue;
        }
        i++;
    }
}

static uint32_t code_colour(const struct ud_look *lk, int style)
{
    switch (style) {
    case CODE_KEYWORD: return lk->accent;
    case CODE_STRING:  return lk->good;
    case CODE_COMMENT: return lk->dim;
    case CODE_NUMBER:
    case CODE_PREPROC: return 0xC586C0;
    default:           return lk->text;
    }
}

static void draw_code_line(int win, const struct ud_look *lk, int line,
                           int first_byte, int end_byte, int baseline)
{
    unsigned char style[MAXC];
    code_styles(text[line], llen[line], style);

    for (int at = first_byte; at < end_byte;) {
        int kind = style[at];
        int end = at + 1;
        while (end < end_byte && style[end] == kind)
            end++;
        char part[MAXC + 1];
        int n = end - at;
        for (int i = 0; i < n; i++)
            part[i] = text[line][at + i];
        part[n] = '\0';
        int xcol = col_of(line, at) - hoff;
        ud_cells(win, editor_x() + xcol * CELL, baseline, CELL, part,
                 code_colour(lk, kind), UD_MONO, TEXT_PX);
        at = end;
    }
}

static void ide_button(int win, const struct ud_look *lk, int x, int w,
                       const char *label, int active)
{
    uint32_t fill = active ? lk->accent : lk->field;
    uint32_t ink = active ? lk->on_accent : lk->text;
    ud_round(win, x, 4, w, IDE_HEADER_H - 8, 4, fill);
    ud_ring(win, x, 4, w, IDE_HEADER_H - 8, 4,
            active ? lk->accent : lk->line);
    ud_text_mid_l(win, x, 4, w, IDE_HEADER_H - 8, label, ink, UD_SANS, 10);
}

static void ide_draw_activity(int win, const struct ud_look *lk)
{
    int explorer_y = IDE_HEADER_H + 18;
    ud_fill(win, 0, explorer_y - 14, 2, 28, lk->accent);
    ud_ring(win, 9, explorer_y - 7, 14, 14, 2, lk->text);
    ud_fill(win, 11, explorer_y - 3, 10, 1, lk->text);
    ud_fill(win, 11, explorer_y, 10, 1, lk->text);

    int search_y = explorer_y + 34;
    ud_ring(win, 9, search_y - 8, 12, 12, 6, lk->dim);
    ud_line(win, 19, search_y + 3, 24, search_y + 8, 16, lk->dim);
}

static void ide_draw_chrome(int win, const struct ud_look *lk)
{
    ud_fill(win, 0, 0, IDE_SIDE_W, ch - STATUS_H, lk->bg);
    ud_fill(win, 0, IDE_HEADER_H, IDE_ACTIVITY_W,
            ch - IDE_HEADER_H - STATUS_H, lk->row);
    ud_fill(win, IDE_ACTIVITY_W, IDE_HEADER_H,
            IDE_SIDE_W - IDE_ACTIVITY_W, ch - IDE_HEADER_H - STATUS_H,
            lk->bg);
    ud_fill(win, IDE_SIDE_W - 1, IDE_HEADER_H, 1,
            ch - IDE_HEADER_H - STATUS_H, lk->line);
    ud_fill(win, 0, IDE_HEADER_H, cw, 1, lk->line);
    ide_draw_activity(win, lk);

    static const char *const menus[] = {
        "File", "Edit", "Selection", "View", "Go", "Run", "Terminal", "Help"
    };
    int menu_x = 10;
    int menu_y = ud_baseline(0, IDE_HEADER_H, 10);
    for (int i = 0; i < (int)(sizeof(menus) / sizeof(menus[0])); i++) {
        int width = ud_width(menus[i], UD_SANS, 9);
        if (menu_x + width > cw - 236)
            break;
        ud_text(win, menu_x, menu_y, menus[i], lk->dim, UD_SANS, 9);
        menu_x += width + 9;
    }
    ide_button(win, lk, cw - 228, 62, "New", 0);
    ide_button(win, lk, cw - 160, 62, "Save", 0);
    ide_button(win, lk, cw - 92, 82,
               terminal_open ? (ulocale_language_polish() ? "Ukryj" : "Hide")
                             : "Terminal", terminal_open);

    ud_text(win, IDE_ACTIVITY_W + 10,
            ud_baseline(IDE_HEADER_H, 22, 9), "EXPLORER",
            lk->dim, UD_BOLD, 9);
    char path[28];
    int length = 0;
    while (project_path[length] && length < 512)
        length++;
    int start = length > 24 ? length - 24 : 0;
    int out = 0;
    if (start) {
        path[out++] = '.';
        path[out++] = '.';
    }
    for (int i = start; i < length && out < (int)sizeof(path) - 1; i++)
        path[out++] = project_path[i];
    path[out] = '\0';
    ud_text(win, IDE_ACTIVITY_W + 10, IDE_EDIT_TOP - 4, path,
            lk->dim, UD_MONO, 9);
    int visible = (ch - IDE_EDIT_TOP - STATUS_H) / IDE_LIST_ROW_H;
    for (int row = 0; row < visible; row++) {
        int item = project_top + row;
        if (item >= nfiles)
            break;
        int y = IDE_EDIT_TOP + row * IDE_LIST_ROW_H;
        if (!file_is_dir[item] && fname[0] &&
            text_equal(fname, names[item]))
            ud_fill(win, IDE_ACTIVITY_W + 4, y,
                    IDE_SIDE_W - IDE_ACTIVITY_W - 8, IDE_LIST_ROW_H, lk->row);
        if (!file_is_dir[item] && fname[0] &&
            text_equal(fname, names[item]))
            ud_fill(win, IDE_ACTIVITY_W + 4, y, 2, IDE_LIST_ROW_H, lk->accent);

        int icon_x = IDE_ACTIVITY_W + 10;
        if (file_is_dir[item]) {
            ud_fill(win, icon_x + 1, y + 6, 11, 7, lk->accent);
            ud_fill(win, icon_x + 2, y + 4, 5, 3, lk->accent);
        } else {
            ud_ring(win, icon_x + 2, y + 3, 9, 12, 1, lk->dim);
            ud_fill(win, icon_x + 4, y + 7, 5, 1, lk->dim);
            ud_fill(win, icon_x + 4, y + 10, 5, 1, lk->dim);
        }
        char label[24];
        int n = 0;
        if (file_is_dir[item])
            label[n++] = '/';
        int name_len = 0;
        while (names[item][name_len])
            name_len++;
        int max_name = (int)sizeof(label) - 1 - n;
        if (name_len <= max_name) {
            for (int i = 0; i < name_len; i++)
                label[n++] = names[item][i];
        } else {
            int copy_len = max_name - 3;
            while (copy_len > 0 && cont(names[item][copy_len]))
                copy_len--;
            for (int i = 0; i < copy_len; i++)
                label[n++] = names[item][i];
            label[n++] = '.';
            label[n++] = '.';
            label[n++] = '.';
        }
        label[n] = '\0';
        ud_text(win, icon_x + 17, ud_baseline(y, IDE_LIST_ROW_H, 10), label,
                file_is_dir[item] ? lk->accent : lk->text, UD_SANS, 10);
    }

    int bread_y = IDE_HEADER_H + IDE_TAB_H;
    ud_fill(win, IDE_SIDE_W, bread_y, cw - IDE_SIDE_W, IDE_BREAD_H, lk->field);
    ud_fill(win, IDE_SIDE_W, bread_y + IDE_BREAD_H - 1,
            cw - IDE_SIDE_W, 1, lk->line);
    int bread_x = IDE_SIDE_W + 8;
    int bread_base = ud_baseline(bread_y, IDE_BREAD_H, 10);
    ud_text(win, bread_x, bread_base, path, lk->dim, UD_MONO, 9);
    bread_x += ud_width(path, UD_MONO, 9) + 8;
    ud_text(win, bread_x, bread_base, ">", lk->dim, UD_SANS, 10);
    bread_x += 12;
    ud_text(win, bread_x, bread_base, fname[0] ? fname : "Untitled",
            lk->text, UD_SANS, 10);

    int tab_width = 112;
    for (int i = 0; i < tab_n; i++) {
        int x = IDE_SIDE_W + i * tab_width;
        if (x + tab_width > cw)
            break;
        ud_fill(win, x, IDE_HEADER_H + 1, tab_width - 1, IDE_TAB_H - 1,
                i == tab_active ? lk->field : lk->bg);
        if (i == tab_active)
            ud_fill(win, x, IDE_HEADER_H + IDE_TAB_H - 2,
                    tab_width - 1, 2, lk->accent);
        char label[NAME_MAX + 2];
        ecopy(label, tabs[i], sizeof(label));
        if (i == tab_active && dirty) {
            int n = 0;
            while (label[n] && n < NAME_MAX)
                n++;
            if (n < NAME_MAX)
                label[n++] = '*';
            label[n] = '\0';
        }
        ud_text(win, x + 7,
                ud_baseline(IDE_HEADER_H, IDE_TAB_H, 10), label,
                i == tab_active ? lk->text : lk->dim, UD_SANS, 10);
    }
}

static void ide_terminal_draw(int win, const struct ud_look *lk)
{
    int height = ide_terminal_height();
    if (!terminal_open || height <= 0 || cw <= IDE_SIDE_W + 24)
        return;
    int y = ide_terminal_top();
    int x = IDE_SIDE_W;
    int width = cw - x;
    int output_y = y + IDE_TERM_HEADER_H;
    int visible = (height - IDE_TERM_HEADER_H) / ROW_H;
    if (visible < 0)
        visible = 0;

    ud_fill(win, x, y, width, height, lk->field);
    ud_fill(win, x, y, width, 1, lk->line);
    static const char *const tabs[] = {
        "PROBLEMS", "OUTPUT", "DEBUG CONSOLE", "TERMINAL"
    };
    int tab_x = x + 10;
    int tab_base = ud_baseline(y, IDE_TERM_HEADER_H, 9);
    for (int i = 0; i < (int)(sizeof(tabs) / sizeof(tabs[0])); i++) {
        uint32_t ink = i == 3 ? lk->text : lk->dim;
        int tab_width = ud_width(tabs[i], UD_SANS, 9);
        ud_text(win, tab_x, tab_base, tabs[i], ink, UD_SANS, 9);
        if (i == 3)
            ud_fill(win, tab_x, y + IDE_TERM_HEADER_H - 2,
                    tab_width, 2, lk->accent);
        tab_x += tab_width + 14;
    }
    ud_fill(win, x, y + IDE_TERM_HEADER_H - 1, width, 1, lk->line);

    int max_scroll = terminal_line_n - visible;
    if (max_scroll < 0)
        max_scroll = 0;
    if (terminal_scroll > max_scroll)
        terminal_scroll = max_scroll;
    int first = terminal_line_n - visible - terminal_scroll;
    if (first < 0)
        first = 0;
    int columns = (width - 20) / CELL;
    if (columns > IDE_TERM_COLS)
        columns = IDE_TERM_COLS;
    if (columns < 1)
        columns = 1;
    for (int row = 0; row < visible; row++) {
        int line = first + row;
        if (line >= terminal_line_n)
            break;
        int current = line == terminal_line_n - 1 && terminal_scroll == 0;
        char shown[IDE_TERM_COLS + IDE_TERM_INPUT_MAX + 1];
        int length = 0;
        for (int i = 0; terminal_lines[line][i] &&
                        length < (int)sizeof(shown) - 1; i++)
            shown[length++] = terminal_lines[line][i];
        if (current) {
            for (int i = 0; i < terminal_len &&
                            length < (int)sizeof(shown) - 1; i++)
                shown[length++] = terminal_input[i];
        }
        shown[length] = '\0';
        int start = 0;
        int cursor_col = -1;
        if (current) {
            int caret = terminal_line_len + terminal_cursor;
            start = caret >= columns ? caret - columns + 1 : 0;
            cursor_col = caret - start;
        }
        char visible_line[IDE_TERM_COLS + 1];
        int out = 0;
        for (int i = start; i < length && out < columns; i++)
            visible_line[out++] = shown[i];
        visible_line[out] = '\0';
        int line_y = output_y + row * ROW_H;
        ud_cells(win, x + 10, line_y + TEXT_BASE, CELL, visible_line,
                 lk->text, UD_MONO, TEXT_PX);
        if (terminal_focus && cursor_col >= 0 && cursor_col < columns)
            ud_fill(win, x + 10 + cursor_col * CELL, line_y + 2, 1,
                    ROW_H - 4, lk->accent);
    }
}
#endif

static void draw(int win)
{
    int r = rows(), c = cols();
    scroll_to_caret();

    struct ud_look lk;
    ud_look(&lk);
    ud_fill(win, 0, 0, cw, ch, lk.field);
#ifdef GCODE_IDE
    ide_draw_chrome(win, &lk);
    ud_fill(win, IDE_SIDE_W, IDE_EDIT_TOP, GUTTER, ch - IDE_EDIT_TOP -
            STATUS_H, lk.bg);
    ud_fill(win, IDE_SIDE_W + GUTTER, IDE_EDIT_TOP, 1, ch - IDE_EDIT_TOP -
            STATUS_H, lk.line);
#else
    ud_fill(win, 0, 0, GUTTER, ch - STATUS_H, lk.bg);
    ud_fill(win, GUTTER, 0, 1, ch - STATUS_H, lk.line);
#endif

    char shown[MAXC + 1];
    for (int i = 0; i < r; i++) {
        int l = top + i;
        if (l >= nlines)
            break;
        int y = editor_top() + i * ROW_H;
        /* The line's number, set against the margin's edge - the one
           the caret is on in the text's own colour, the rest quieter */
        char no[8];
        num(no, l + 1);
#ifdef GCODE_IDE
        int gutter_right = editor_x() - 6;
#else
        int gutter_right = GUTTER - 6;
#endif
        ud_text_right(win, gutter_right, y + TEXT_BASE, no,
                      l == cy ? lk.text : lk.dim, UD_SANS, 11);
        /* From column hoff, c columns of it - whole letters */
        int b = byte_of(l, hoff), n = 0, letters = 0;
        while (b < llen[l]) {
            if (!cont(text[l][b]) && letters++ == c)
                break;
            shown[n++] = text[l][b++];
        }
        shown[n] = '\0';
        if (n) {
#ifdef GCODE_IDE
            draw_code_line(win, &lk, l, byte_of(l, hoff),
                           byte_of(l, hoff) + n, y + TEXT_BASE);
#else
            ud_cells(win, TEXT_X, y + TEXT_BASE, CELL, shown, lk.text,
                     UD_MONO, TEXT_PX);
#endif
        }
    }

    /* The caret: a hair BETWEEN two characters, where the next one
       will go. A block would cover the letter it stands on, and a bar
       inside the cell would eat its first column - both of them hide
       the very thing a person is looking at while they type. */
    int caret_x = editor_x() + (col_of(cy, cx) - hoff) * CELL - 1;
    ud_fill(win, caret_x, editor_top() + (cy - top) * ROW_H + 1, 1, ROW_H - 3,
            lk.accent);

#ifdef GCODE_IDE
    ide_terminal_draw(win, &lk);
#endif

    /* The status line: what the file is, whether it has been changed,
       where the caret is - or the question, when one is being asked. */
#ifdef GCODE_IDE
    ud_fill(win, 0, ch - STATUS_H, cw, STATUS_H, lk.accent);
#else
    ud_fill(win, 0, ch - STATUS_H, cw, STATUS_H, lk.row);
#endif
    ud_fill(win, 0, ch - STATUS_H, cw, 1, lk.line);
    char line[80];
    int p = 0;
    if (asking) {
#ifdef GCODE_IDE
        int polish = ulocale_language_polish();
        const char *q = asking == ASK_SAVE
                            ? (polish ? "Zapisz jako: " : "Save as: ")
                        : asking == ASK_OPEN
                            ? (polish ? "Otwórz: " : "Open: ")
                        : asking == ASK_FIND
                            ? (polish ? "Szukaj: " : "Find: ")
                            : (polish ? "Nowy: " : "New: ");
#else
        const char *q = asking == ASK_SAVE ? "Save as: "
                      : asking == ASK_OPEN ? "Open: "
                                           : "Find: ";
#endif
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
        num(n, col_of(cy, cx) + 1);
        for (int i = 0; n[i]; i++)
            line[p++] = n[i];
        if (note[0]) {
            line[p++] = ' ';
            line[p++] = '-';
            line[p++] = ' ';
            const char *shown_note = note_for_language(note);
            for (int i = 0; shown_note[i] && p < 70; i++)
                line[p++] = shown_note[i];
        }
    }
    line[p] = '\0';
#ifdef GCODE_IDE
    ud_text(win, 10, ud_baseline(ch - STATUS_H, STATUS_H, 11), line,
            lk.on_accent, asking ? UD_BOLD : UD_SANS, 11);
#else
    ud_text(win, 10, ud_baseline(ch - STATUS_H, STATUS_H, 11), line,
            asking ? lk.accent : lk.text, asking ? UD_BOLD : UD_SANS, 11);
#endif

    /* Last, over everything: the panel that opens a file. A list one
       can scroll the text under is not a question. */
    editor_ui.win = win;
    editor_ui.w = cw;
    editor_ui.h = ch;
    ug_pick_draw(&editor_ui, &picker);
}

#ifdef GCODE_IDE
static void edit_key(char c);

static void ide_click(int x, int y)
{
    if (asking)
        return;
    if (y < IDE_HEADER_H) {
        if (x >= cw - 92) {
            ide_terminal_toggle();
        } else if (x >= cw - 160) {
            if (fname[0])
                save_to(fname);
            else {
                asking = ASK_SAVE;
                alen = 0;
            }
        } else if (x >= cw - 228) {
            if (dirty) {
                ecopy(note, "Save before changing files", sizeof(note) - 1);
            } else {
                asking = ASK_NEW;
                alen = 0;
            }
        }
        return;
    }

    if (x < IDE_ACTIVITY_W) {
        terminal_focus = 0;
        if (y >= IDE_HEADER_H + 34 && y < IDE_HEADER_H + 70)
            edit_key(KEY_OPEN);
        return;
    }

    if (terminal_open && x >= IDE_SIDE_W &&
        y >= ide_terminal_top() && y < ch - STATUS_H) {
        terminal_focus = 1;
        return;
    }
    terminal_focus = 0;

    if (y < IDE_EDIT_TOP) {
        if (y >= IDE_HEADER_H && y < IDE_HEADER_H + IDE_TAB_H &&
            x >= IDE_SIDE_W) {
            int index = (x - IDE_SIDE_W) / 112;
            ide_activate_tab(index);
        }
        return;
    }

    if (x < IDE_SIDE_W) {
        int row = project_top + (y - IDE_EDIT_TOP) / IDE_LIST_ROW_H;
        ide_open_item(row);
        return;
    }

    follow_caret = 1;
    int line = top + (y - IDE_EDIT_TOP) / ROW_H;
    if (line < 0)
        line = 0;
    if (line >= nlines)
        line = nlines - 1;
    cy = line;
    int col = hoff + (x - editor_x() + CELL / 2) / CELL;
    if (col < 0)
        col = 0;
    cx = byte_of(cy, col);
}
#endif

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
#ifdef GCODE_IDE
        } else if (which == ASK_NEW && alen) {
            int fd = (int)sys_open(answer, O_READ);
            if (fd >= 0) {
                sys_close(fd);
                ecopy(note, "That file already exists", sizeof(note) - 1);
                return;
            }
            clear_all();
            ecopy(fname, answer, NAME_MAX);
            dirty = 1;
            ide_register_tab(fname);
            ecopy(note, "New file; save to create it", sizeof(note) - 1);
            retitle();
#endif
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
#ifdef GCODE_IDE
        reread_files();
        ug_pick_open(&picker,
                     ulocale_language_polish()
                         ? "Szybkie otwieranie (Ctrl+O)"
                         : "Quick Open (Ctrl+O)",
                     file_row, (void *)0, nfiles, picked, sizeof(picked));
        return;
#else
        /* A list of what is there, instead of a name typed from
           memory. Typing still works - the line under the list is the
           same line the question used to be. */
        reread_files();
        ug_pick_open(&picker, ulocale_language_polish()
                                  ? "Otwórz plik:" : "Open a file:",
                     file_row, (void *)0, nfiles, picked, (int)sizeof(picked));
        return;
#endif
    case KEY_FIND:
        asking = ASK_FIND;
        alen = 0;
        return;
    case KEY_UNDO:
        undo_one();
        return;
    case KEY_UP: {
        int col = col_of(cy, cx);       /* the same column, a line up */
        if (cy > 0)
            cy--;
        cx = byte_of(cy, col);
        return;
    }
    case KEY_DOWN: {
        int col = col_of(cy, cx);
        if (cy < nlines - 1)
            cy++;
        cx = byte_of(cy, col);
        return;
    }
    case KEY_LEFT:
        if (cx > 0) {
            cx--;
            while (cx > 0 && cont(text[cy][cx]))
                cx--;
        } else if (cy > 0) {
            cx = llen[--cy];
        }
        return;
    case KEY_RIGHT:
        if (cx < llen[cy]) {
            cx++;
            while (cx < llen[cy] && cont(text[cy][cx]))
                cx++;
        } else if (cy < nlines - 1) {
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
    case KEY_PGUP: {
        int col = col_of(cy, cx);
        cy -= rows();
        if (cy < 0)
            cy = 0;
        cx = byte_of(cy, col);
        return;
    }
    case KEY_PGDN: {
        int col = col_of(cy, cx);
        cy += rows();
        if (cy > nlines - 1)
            cy = nlines - 1;
        cx = byte_of(cy, col);
        return;
    }
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
#ifdef GCODE_IDE
    {
        int previous = cy;
        int indent = 0;
        while (indent < llen[previous] && text[previous][indent] == ' ')
            indent++;
        int last = llen[previous] - 1;
        while (last >= 0 && text[previous][last] == ' ')
            last--;
        if (last >= 0 && text[previous][last] == '{')
            indent += 4;
        split_line();
        while (indent-- > 0 && llen[cy] < MAXC)
            insert_char(' ');
        return;
    }
#else
        split_line();
        return;
#endif
    case '\t':
#ifdef GCODE_IDE
        for (int i = 0; i < 4 && llen[cy] < MAXC; i++)
            insert_char(' ');
        return;
#else
        return;
#endif
    case '\b':
        backspace();
        return;
    default:
        /* printable, and the bytes of a letter beyond ASCII */
        if ((unsigned char)c >= 0x20 && c != 0x7F)
            insert_char(c);
        return;
    }
}

void _start(int argc, char **argv)
{
#ifdef GCODE_IDE
    int win = (int)win_create(IDE_W0, IDE_H0,
                              ulocale_language_polish() ? "Kod" : "Code");
#else
    int win = (int)win_create(W0, H0,
                              ulocale_language_polish() ? "Edytor" : "Editor");
#endif
    if (win < 0)
        sys_exit(1);
    winid = win;

    clear_all();
#ifdef GCODE_IDE
    ide_tabs_reset();
    project_read_path();
    ecopy(note, "No compiler is bundled; Terminal opens in this project",
          sizeof(note) - 1);
    if (argc > 1 && argv && argv[1] && argv[1][0]) {
        if (sys_chdir(argv[1]) == 0) {
            project_read_path();
        } else {
            load_from(argv[1]);
        }
    }
    if (argc > 2 && argv && argv[2] && argv[2][0])
        load_from(argv[2]);
    project_read_path();
    reread_files();
#else
    ecopy(note, "Ctrl+O opens, Ctrl+S saves, Ctrl+F finds, "
                "Ctrl+Z undoes", sizeof(note) - 1);
    /* Started with a name - 'gedit notatka' - it opens that file at
       once. Making a person type the name a second time, in the
       program they have just told which file they mean, is the sort of
       thing that makes an editor a chore. */
    if (argc > 1 && argv && argv[1] && argv[1][0])
        load_from(argv[1]);
#endif
    draw(win);

    int language = ulocale_language_polish();
    for (;;) {
        struct win_event ev;
        int drew = 0;
#ifdef GCODE_IDE
        if (ide_terminal_poll())
            drew = 1;
#endif
        int current_language = ulocale_language_polish();
        if (current_language != language) {
            language = current_language;
            drew = 1;
        }
        while (win_poll(win, &ev) == 1) {
            if (picker.on) {
                editor_ui.win = win;
                editor_ui.w = cw;
                editor_ui.h = ch;
                int what = ug_pick_event(&editor_ui, &picker, &ev);
                if (what == UG_CLOSED)
                    sys_exit(0);
                if (what == UG_PRESSED && picker.answer && picked[0]) {
#ifdef GCODE_IDE
                    int item = -1;
                    for (int i = 0; i < nfiles; i++) {
                        if (text_equal(names[i], picked)) {
                            item = i;
                            break;
                        }
                    }
                    if (item >= 0)
                        ide_open_item(item);
                    else
                        ide_open_file(picked);
#else
                    load_from(picked);
#endif
                }
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
#ifdef GCODE_IDE
                if (terminal_open && ev.x >= IDE_SIDE_W &&
                    ev.y >= ide_terminal_top() &&
                    ev.y < ch - STATUS_H) {
                    int visible = (ide_terminal_height() -
                                   IDE_TERM_HEADER_H) / ROW_H;
                    int max_scroll = terminal_line_n - visible;
                    if (max_scroll < 0)
                        max_scroll = 0;
                    terminal_scroll += ev.val * 3;
                    if (terminal_scroll > max_scroll)
                        terminal_scroll = max_scroll;
                    if (terminal_scroll < 0)
                        terminal_scroll = 0;
                } else if (ev.x < IDE_SIDE_W) {
                    project_top -= ev.val * 3;
                    int visible = (ch - IDE_EDIT_TOP - STATUS_H) /
                                  IDE_LIST_ROW_H;
                    int max_top = nfiles - visible;
                    if (max_top < 0)
                        max_top = 0;
                    if (project_top > max_top)
                        project_top = max_top;
                    if (project_top < 0)
                        project_top = 0;
                } else {
#endif
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
#ifdef GCODE_IDE
                }
#endif
            } else if (ev.type == WEV_KEY) {
                char c = (char)ev.val;
                if (asking)
                    ask_key(c);
                else {
#ifdef GCODE_IDE
                    if (terminal_open && terminal_focus) {
                        ide_term_key(c);
                    } else {
#endif
                    follow_caret = 1; /* a key means "where I am" again */
                    edit_key(c);
#ifdef GCODE_IDE
                    }
#endif
                }
            } else if (ev.type == WEV_CLICK) {
#ifdef GCODE_IDE
                ide_click(ev.x, ev.y);
#else
                follow_caret = 1;     /* putting the caret somewhere */
                /* The caret goes where the pointer is - the nearest
                   place there is text, not the empty page past it. */
                int l = top + (ev.y - PAD) / ROW_H;
                if (l < 0)
                    l = 0;
                if (l > nlines - 1)
                    l = nlines - 1;
                cy = l;
                int col = hoff + (ev.x - TEXT_X + CELL / 2) / CELL;
                if (col < 0)
                    col = 0;
                cx = byte_of(cy, col);
#endif
            }
            drew = 1;
        }
        if (drew)
            draw(win);
        sys_sleep(40);
    }
}
