/* pumarks - Weave's four marks, drawn where t_protonui can find them
 *
 * A window filled with one colour no other window has (BG), and on it,
 * at the places below (in the window's content, logical pixels):
 *
 *   the grain at GRAIN_X..+GRAIN_W, GRAIN_Y..+GRAIN_H, white at INK
 *   the same grain again through a clip that starts at CLIP_X, so the
 *   test can see the tile is counted from the window's corner and not
 *   from the clip's
 *   a stitch across at STITCH_X, STITCH_Y, STITCH_LEN long, and one
 *   down at VSTITCH_X, VSTITCH_Y
 *   three knots KNOT_D across at KNOT_X + i * KNOT_STEP, KNOT_Y: hollow,
 *   ringed, filled
 *   a thread from (T0X, T0Y) pulled towards (T1X, T1Y) and (T2X, T2Y)
 *   to (T3X, T3Y), THREAD_W sixteenths wide
 *
 * Every number is a plain #define the test reads. It says
 * "[pumarks] drawn" and stays until its window is closed.
 */

#include "uapi.h"
#include "udraw.h"

#define W 320
#define H 220
#define BG 0x102030
#define INK_RGB 0xFFFFFF
#define INK 120
#define GRAIN_X 10
#define GRAIN_Y 10
#define GRAIN_W 60
#define GRAIN_H 30
#define CLIP_X 202
#define CLIP_Y 11
#define CLIP_W 40
#define CLIP_H 29
#define STITCH_RGB 0xF0D020
#define STITCH_X 10
#define STITCH_Y 60
#define STITCH_LEN 60
#define VSTITCH_X 90
#define VSTITCH_Y 10
#define KNOT_RGB 0x46C27A
#define KNOT_X 110
#define KNOT_Y 10
#define KNOT_D 24
#define KNOT_STEP 30
#define THREAD_RGB 0xD152A6
#define THREAD_W 48
#define T0X 10
#define T0Y 150
#define T1X 70
#define T1Y 90
#define T2X 170
#define T2Y 210
#define T3X 300
#define T3Y 150

static void draw(int w)
{
    ud_fill(w, 0, 0, W, H, BG);
    ud_grain(w, GRAIN_X, GRAIN_Y, GRAIN_W, GRAIN_H, INK_RGB, INK);
    ud_clip(w, CLIP_X, CLIP_Y, CLIP_W, CLIP_H);
    ud_grain(w, CLIP_X, CLIP_Y, CLIP_W, CLIP_H, INK_RGB, INK);
    ud_unclip(w);
    ud_stitch(w, STITCH_X, STITCH_Y, STITCH_LEN, 0, STITCH_RGB);
    ud_stitch(w, VSTITCH_X, VSTITCH_Y, STITCH_LEN, 1, STITCH_RGB);
    for (int i = 0; i < 3; i++)
        ud_knot(w, KNOT_X + i * KNOT_STEP, KNOT_Y, KNOT_D, i, KNOT_RGB);
    ud_thread(w, T0X, T0Y, T1X, T1Y, T2X, T2Y, T3X, T3Y, THREAD_W,
              THREAD_RGB, 255);
}

void _start(void)
{
    int w = (int)win_create(W, H, "Marks");
    if (w < 0)
        sys_exit(1);
    draw(w);
    print("[pumarks] drawn\n");
    for (;;) {
        struct win_event ev;
        while (win_poll(w, &ev) == 1) {
            if (ev.type == WEV_CLOSE)
                sys_exit(0);
            if (ev.type == WEV_LOOK || ev.type == WEV_SIZE)
                draw(w);
        }
        sys_sleep(100);
    }
}
