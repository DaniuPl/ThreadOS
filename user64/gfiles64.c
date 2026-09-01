/* gfiles64 - a file manager in a window (ring 3, the 64-bit kernel)
 *
 * It lists files from the disk in a window; clicking a name opens the
 * file and shows its beginning in the preview panel. It ties four
 * things together at once: the file system, mouse events, the window
 * calls and the process's own address space - and all of it from
 * ring 3.
 */

#include "usys64.h"

#define C_BG    8
#define C_BLUE  3
#define C_GREEN 2
#define C_WHITE 4

#define WINDOW_W 180
#define WINDOW_H 120
#define ROW_Y0 16
#define ROW_H_PX  10
#define MAXP      6

static char names[MAXP][NAME_MAX64 + 1];
static int ile;
static char selected[NAME_MAX64 + 1];
static char preview[96];

static void copy_out(char *d, const char *s, int max)
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

/* "Edytuj": what a file manager is FOR is getting to a file, and
   getting to a text file means opening it in the editor. In long mode
   a program is started the way everything else is here - fork and
   exec - and the child hands the file name straight to the editor. */
#define EDIT_X 122
#define EDIT_H 12

static void edit_selected(void)
{
    if (!selected[0])
        return;
    int64_t pid = sys_fork();
    if (pid == 0) {
        char *args[3];
        args[0] = "gedit";
        args[1] = selected;
        args[2] = 0;
        sys_exec("gedit", args);
        sys_exit(127);            /* exec comes back only on an error */
    }
}

/* Opens the selected file and reads its beginning into the preview */
static void load(const char *name)
{
    copy_out(selected, name, sizeof(selected));

    int fd = (int)sys_open(name, 0);
    if (fd < 0) {
        copy_out(preview, "(nie da sie otworzyc)", sizeof(preview));
        return;
    }
    int64_t n = sys_read(fd, preview, sizeof(preview) - 1);
    sys_close(fd);
    if (n < 0)
        n = 0;
    preview[n] = '\0';
    /* The preview is drawn on one line - it ends at the first newline */
    for (int i = 0; preview[i]; i++)
        if (preview[i] == '\n') {
            preview[i] = '\0';
            break;
        }
}

void _start(void)
{
    int w = (int)win_create(WINDOW_W, WINDOW_H, "Pliki");
    if (w < 0) {
        print("gfiles64: pulpit nie dal okna\n");
        sys_exit(1);
    }

    for (;;) {
        struct win_event ev;
        while (win_poll(w, &ev) == 1) {
            if (ev.type == WEV_CLICK) {
                if (ev.y < EDIT_H && ev.x >= EDIT_X) {
                    edit_selected();
                    continue;
                }
                int row_text = (ev.y - ROW_Y0) / ROW_H_PX;
                if (ev.y >= ROW_Y0 && row_text >= 0 && row_text < ile)
                    load(names[row_text]);
            }
        }

        win_rect(w, 0, 0, WINDOW_W, WINDOW_H, C_BG);
        win_text(w, 4, 2, "Pliki na dysku:", C_GREEN);
        win_rect(w, EDIT_X, 0, WINDOW_W - EDIT_X, EDIT_H, C_BLUE);
        win_text(w, EDIT_X + 5, 2, "Edytuj", C_WHITE);

        ile = 0;
        struct dirent64 entry;
        for (int i = 0; i < 32 && ile < MAXP; i++) {
            /* sys_readdir returns ZERO on a hit and -1 at the end of the
               directory - unlike fs_readdir in the kernel, which
               returns one. Confusing these two conventions ends in an
               empty window, because the first entry looks like the
               end. */
            if (sys_readdir(i, &entry) != 0)
                break;
            copy_out(names[ile], entry.name, sizeof(names[0]));
            int color = same(entry.name, selected) ? C_BLUE : C_WHITE;
            win_text(w, 8, ROW_Y0 + ile * ROW_H_PX, names[ile], color);
            ile++;
        }
        if (ile == 0)
            win_text(w, 8, ROW_Y0, "(pusto)", C_WHITE);

        win_rect(w, 0, 86, WINDOW_W, 1, C_WHITE);
        if (selected[0]) {
            win_text(w, 4, 90, "tresc:", C_GREEN);
            win_text(w, 4, 102, preview, C_WHITE);
        }

        sys_sleep(120);
    }
}
