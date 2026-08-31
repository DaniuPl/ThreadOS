/* gterm64 - a terminal in a graphical window (ring 3, the 64-bit
 * kernel)
 *
 * It takes keys (when the window has focus) into a command line. On
 * Enter it runs a program with its output redirected to a file, waits
 * for it to end, reads the file and draws the result in the window.
 *
 * There is not a single thing here the kernel could not do before -
 * what there is instead is all of it at once: the keyboard through
 * window events, fork and exec, redirection through dup2, waiting for a
 * child, the file system and drawing. The terminal is an ordinary
 * program in ring 3 and knows nothing about the compositor beyond four
 * window calls.
 */

#include "usys64.h"

#define C_GREEN 2
#define C_WHITE 4
#define C_BG    8

#define WINDOW_W 240
#define WINDOW_H 150

#define ROWS_MAX 60
#define FILE_MARK "termout"

static char row_text[ROWS_MAX];
static int length;
static char result[1024];

/* Runs the command that was typed with its output to a file and reads
   the result */
static void run(void)
{
    row_text[length] = '\0';
    length = 0;
    if (!row_text[0])
        return;

    /* The file from scratch: otherwise a shorter result would leave a
       tail of the previous one. We delete it and create it again - the
       same thing 'rm' would do before a redirection. */
    sys_unlink(FILE_MARK);
    int fd = (int)sys_open(FILE_MARK, 1);
    if (fd < 0)
        return;

    int64_t pid = sys_fork();
    if (pid == 0) {
        /* From this moment the child's prints go to the file, not to the
           console. The child does not know that and does not have to -
           for it this is still descriptor number one. */
        sys_dup2(fd, FD_STDOUT);
        sys_close(fd);
        char *argv[2] = {row_text, 0};
        sys_exec(row_text, argv);
        sys_exit(127);               /* exec returns only on an error */
    }
    sys_close(fd);
    if (pid > 0) {
        int code = 0;
        sys_wait(&code);
    }

    int rd = (int)sys_open(FILE_MARK, 0);
    if (rd < 0) {
        result[0] = '\0';
        return;
    }
    int64_t n = sys_read(rd, result, sizeof(result) - 1);
    sys_close(rd);
    result[n < 0 ? 0 : n] = '\0';
}

/* Draws the output buffer split into lines on the newline character */
static void print_result(int w)
{
    char buf[40];
    int kol = 0, row = 0, y = 4;

    for (int i = 0; result[i] && row < 12; i++) {
        if (result[i] == '\n' || kol >= (int)sizeof(buf) - 1) {
            buf[kol] = '\0';
            win_text(w, 4, y, buf, C_WHITE);
            y += 10;
            row++;
            kol = 0;
            if (result[i] != '\n')
                buf[kol++] = result[i];
        } else if (result[i] != '\r') {
            buf[kol++] = result[i];
        }
    }
    if (kol) {
        buf[kol] = '\0';
        win_text(w, 4, y, buf, C_WHITE);
    }
}

void _start(void)
{
    int w = (int)win_create(WINDOW_W, WINDOW_H, "Terminal");
    if (w < 0) {
        print("gterm64: pulpit nie dal okna\n");
        sys_exit(1);
    }

    for (;;) {
        struct win_event ev;
        while (win_poll(w, &ev) == 1) {
            if (ev.type != WEV_KEY)
                continue;
            char c = (char)ev.val;
            if (c == '\n')
                run();
            else if (c == '\b') {
                if (length > 0)
                    length--;
            } else if (c >= 0x20 && c < 0x7F && length < ROWS_MAX - 1) {
                row_text[length++] = c;
            }
        }

        win_rect(w, 0, 0, WINDOW_W, WINDOW_H, C_BG);
        print_result(w);
        row_text[length] = '\0';
        win_text(w, 4, WINDOW_H - 12, ">", C_GREEN);
        win_text(w, 16, WINDOW_H - 12, row_text, C_WHITE);

        sys_sleep(60);
    }
}
