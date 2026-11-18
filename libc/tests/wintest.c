/* wintest - a program's window, seen from the program
 *
 * ThreadOS's own window calls (user64/usys64.h), the ones Weft's WPE
 * platform draws and hears through. On the phone a window is the
 * screen below the status bar (kernelarm64/winmob.c), and
 * scripts/tests/t_arm64win.py reads that screen and touches it:
 *
 *   the window's size comes first (WEV_SIZE: its logical size, and in
 *   its val the scale the screen draws it at), and the program draws at
 *   it: the top half red, the bottom half blue, a green square in the
 *   middle, a black one in the top left corner - and a line of text
 *   beside the corner, through WIN_DRAW, which the screen draws at its
 *   own resolution
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
#define WHITE  0xFFFFFF
#define TEXT_X 140               /* the text's start and baseline */
#define TEXT_Y 60
#define TEXT_PX 24

/* user64/compat64.h's struct win_draw_cmd: one text, in bold */
struct draw {
    int32_t op, x, y, w, h, r;
    uint32_t rgb, rgb2;
    int32_t a, font, px;
    const char *s;
};

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
    int k = (ev.val >> 8) & 0xFF;
    row = malloc((size_t)w * 4);
    fill(0, 0, w, h / 2, RED);
    fill(0, h / 2, w, h - h / 2, BLUE);
    fill((w - SQUARE) / 2, (h - SQUARE) / 2, SQUARE, SQUARE, GREEN);
    fill(0, 0, CORNER, CORNER, BLACK);
    struct draw text = { 7, TEXT_X, TEXT_Y, 0, 0, 0, WHITE, 0, 255, 1,
                         TEXT_PX, "Hello phone" };
    long end = syscall64(SYS64_WIN_DRAW, win, (int64_t)(uintptr_t)&text);
    printf("[wintest] window %d, %dx%d at %d/16, text to %ld, drawn\n", win,
           w, h, k ? k : 16, end);
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
