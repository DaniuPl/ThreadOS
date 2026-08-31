/* gterm - a terminal in a graphical window (ring 3)
 *
 * It takes keys (when the window has focus) into a command line. On
 * Enter it runs a program with its output redirected to the file
 * 'termout', waits, reads the file and draws the result in the window.
 * It ties the keyboard, processes, redirection and the file system
 * together in one GUI.
 */

#include "usys.h"

#define W 240
#define H 150
#define C_BG    8
#define C_WHITE 4
#define C_GREEN 2

#define LINE_MAX 60
static char line[LINE_MAX];
static int len;
static char out[1024];

static void run_line(void)
{
    line[len] = '\0';
    if (len == 0)
        return;
    int32_t pid = sys_spawn(line, "termout", 0);   /* stdout -> a file */
    if (pid >= 0)
        sys_wait(pid);

    int fd = sys_open("termout", O_READ);
    if (fd >= 0) {
        int n = sys_read(fd, out, sizeof(out) - 1);
        sys_close(fd);
        out[n < 0 ? 0 : n] = '\0';
    }
    len = 0;
}

/* Draws the output buffer split into lines on '\n' */
static void draw_output(int w)
{
    char buf[40];
    int col = 0, row = 0, y = 4;
    for (int i = 0; out[i] && row < 12; i++) {
        if (out[i] == '\n' || col >= (int)sizeof(buf) - 1) {
            buf[col] = '\0';
            win_text(w, 4, y, buf, C_WHITE);
            y += 10;
            row++;
            col = 0;
            if (out[i] != '\n')
                buf[col++] = out[i];
        } else {
            buf[col++] = out[i];
        }
    }
    if (col) {
        buf[col] = '\0';
        win_text(w, 4, y, buf, C_WHITE);
    }
}

void _start(void)
{
    int w = win_create(W, H, "Terminal");
    if (w < 0)
        sys_exit(1);

    for (;;) {
        struct win_event ev;
        while (win_poll(w, &ev) == 1) {
            /* Asked to end by the desktop - so we end */
            if (ev.type == WEV_CLOSE)
                sys_exit(0);
            if (ev.type != WEV_KEY)
                continue;
            char c = (char)ev.val;
            if (c == '\n')
                run_line();
            else if (c == '\b') {
                if (len > 0)
                    len--;
            } else if (c >= 0x20 && c < 0x7F && len < LINE_MAX - 1)
                line[len++] = c;
        }

        win_rect(w, 0, 0, W, H, C_BG);
        draw_output(w);
        line[len] = '\0';
        win_text(w, 4, H - 12, ">", C_GREEN);
        win_text(w, 16, H - 12, line, C_WHITE);

        sys_sleep(60);
    }
}
