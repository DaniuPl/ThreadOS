/* wintest - a program's window, seen from the program
 *
 * ThreadOS's own window calls (user64/usys64.h), the ones Weft's WPE
 * platform draws and hears through. On the phone a window is the
 * screen below the status bar (kernelarm64/winmob.c), and
 * scripts/tests/t_arm64win.py reads that screen and touches it:
 *
 *   the window's size comes first (WEV_SIZE), and the program draws at
 *   it: the top half red, the bottom half blue, a green square in the
 *   middle, a black one in the top left corner
 *
 *   every event is said on the console, in the window's own pixels
 *
 *   a tap in the middle square turns it yellow, and green again; a tap
 *   in the corner ends the program
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "usys64.h"

#define SQUARE 160               /* the middle square's side */
#define CORNER 120               /* the corner's */
#define RED    0xE02020
#define BLUE   0x2040E0
#define GREEN  0x20B040
#define YELLOW 0xF0D020
#define BLACK  0x000000

static int win, w, h;
static uint32_t *row;

static void fill(int x, int y, int fw, int fh, uint32_t c)
{
    for (int i = 0; i < fw; i++)
        row[i] = c;
    for (int yy = y; yy < y + fh; yy++)
        win_blit32(win, x, yy, fw, 1, row);
}

static int in_square(int x, int y)
{
    return x >= (w - SQUARE) / 2 && x < (w + SQUARE) / 2 &&
           y >= (h - SQUARE) / 2 && y < (h + SQUARE) / 2;
}

static const char *kind(int type)
{
    switch (type) {
    case WEV_CLICK:   return "click";
    case WEV_PRESS:   return "press";
    case WEV_DRAG:    return "drag";
    case WEV_RELEASE: return "release";
    case WEV_SIZE:    return "size";
    }
    return "other";
}

int main(void)
{
    win = (int)win_create(320, 240, "Win test");
    if (win < 0) {
        printf("[wintest] no window\n");
        return 1;
    }
    win_truecolour(win);
    struct win_event ev;
    while (win_poll(win, &ev) != 1 || ev.type != WEV_SIZE)
        usleep(10000);
    w = ev.x;
    h = ev.y;
    row = malloc((size_t)w * 4);
    fill(0, 0, w, h / 2, RED);
    fill(0, h / 2, w, h - h / 2, BLUE);
    fill((w - SQUARE) / 2, (h - SQUARE) / 2, SQUARE, SQUARE, GREEN);
    fill(0, 0, CORNER, CORNER, BLACK);
    printf("[wintest] window %d, %dx%d, drawn\n", win, w, h);
    fflush(stdout);

    int yellow = 0;
    for (;;) {
        if (win_poll(win, &ev) != 1) {
            usleep(10000);
            continue;
        }
        printf("[wintest] %s %d,%d\n", kind(ev.type), ev.x, ev.y);
        if (ev.type == WEV_RELEASE && in_square(ev.x, ev.y)) {
            yellow = !yellow;
            fill((w - SQUARE) / 2, (h - SQUARE) / 2, SQUARE, SQUARE,
                 yellow ? YELLOW : GREEN);
            printf("[wintest] the square is %s\n", yellow ? "yellow"
                                                         : "green");
        } else if (ev.type == WEV_RELEASE && ev.x < CORNER &&
                   ev.y < CORNER) {
            printf("[wintest] bye\n");
            fflush(stdout);
            return 0;
        }
        fflush(stdout);
    }
}
