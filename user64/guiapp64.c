/* guiapp64 - a graphical application in ring 3, the 64-bit kernel
 *
 * The desktop has so far worked with itself alone: the compositor was
 * in the kernel, and the icons pointed at programs that did not exist
 * in long mode. This program is on the other side of that line - it
 * draws through system calls, in a window of its own, and gets events
 * back: clicks in the window's area and keys, when the window has
 * focus.
 *
 * The colours are indices into the desktop's palette, not RGB - the
 * kernel keeps the palette, the program does not have to know anything
 * about it.
 */

#include "usys64.h"

#define C_RED   1
#define C_GREEN 2
#define C_BLUE  3
#define C_WHITE 4
#define C_BG    8    /* the window's grey background */
/* Dark text on a light surface, light text on a dark one: the window
   is the system's light grey, so its text is black. */
#define C_TEXT  0
#define C_DIM  15
#define C_HEAD  3

#define WINDOW_W 120
#define WINDOW_H 60

#define MAXPT 64
static int px[MAXPT], py[MAXPT];
static int npt;

static char written[16];
static int length;

static void draw_number(int w, int x, int y, int v, int color)
{
    char tmp[12], s[12];
    int n = 0, j = 0;
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        s[j++] = tmp[--n];
    s[j] = '\0';
    win_text(w, x, y, s, color);
}

void _start(void)
{
    int w = (int)win_create(WINDOW_W, WINDOW_H, "App64");
    if (w < 0) {
        print("guiapp64: pulpit nie dal okna\n");
        sys_exit(1);
    }

    for (;;) {
        /* Events: clicks and keys. The queue is short, so we drain it
           to the end on every frame. */
        struct win_event ev;
        while (win_poll(w, &ev) == 1) {
            if (ev.type == WEV_CLICK && npt < MAXPT) {
                px[npt] = ev.x;
                py[npt] = ev.y;
                npt++;
            } else if (ev.type == WEV_KEY) {
                char c = (char)ev.val;
                if (c == '\b') {
                    if (length > 0)
                        length--;
                } else if (c >= 0x20 && c < 0x7F &&
                           length < (int)sizeof(written) - 1) {
                    written[length++] = c;
                }
            }
        }
        written[length] = '\0';

        /* The whole window's content from scratch: the background, the
   palette, the captions, the counter */
        win_rect(w, 0, 0, WINDOW_W, WINDOW_H, C_BG);
        win_rect(w, 6, 6, 24, 12, C_RED);
        win_rect(w, 34, 6, 24, 12, C_GREEN);
        win_rect(w, 62, 6, 24, 12, C_BLUE);
        win_text(w, 6, 24, "klikniec:", C_TEXT);
        draw_number(w, 80, 24, npt, C_TEXT);
        win_text(w, 6, 38, "txt:", C_HEAD);
        win_text(w, 40, 38, written, C_TEXT);

        /* Markers at the places of the clicks */
        for (int i = 0; i < npt; i++)
            win_rect(w, px[i] - 2, py[i] - 2, 5, 5, C_HEAD);

        sys_sleep(80);
    }
}
