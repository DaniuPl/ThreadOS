/* gterm - terminal w oknie graficznym (ring 3)
 *
 * Przyjmuje klawisze (gdy okno ma fokus) do linii polecen. Po Enter
 * uruchamia program z wyjsciem przekierowanym do pliku 'termout',
 * czeka, czyta plik i rysuje wynik w oknie. Spina klawiature, procesy,
 * przekierowania i system plikow w jednym GUI.
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
    int32_t pid = sys_spawn(line, "termout", 0);   /* stdout -> plik */
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

/* Rysuje bufor wyjscia rozbity na wiersze po '\n' */
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
