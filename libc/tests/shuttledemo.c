/* shuttledemo: notes in their three weights, and what is going on
 *
 * docs/protonui-plan.md stage 6: a window of buttons, each of which
 * sends something to the shell through ProtonUI v2 - a note of each
 * weight (pu_notify), a timer, a download and a call (pu_shuttle), and
 * the end of them. A key does what its button does, so that a test
 * need not find the buttons first:
 *
 *   q  a Quiet note      - straight to Threads
 *   n  a Normal note     - the shuttle a moment, then Threads; "Reply"
 *   u  an Urgent note    - the shuttle until answered; "Stop", "Snooze"
 *   t  a timer           - "Tea", 3:12
 *   d  a download        - at 41 %, and on with every 'd' after
 *   c  a call            - counting up from 0:00
 *   e  the end of it
 *
 * What happens is said on the serial port:
 *
 *   [shuttledemo] drawn
 *   [shuttledemo] sent WEIGHT NUMBER       (the note's number, or -1)
 *   [shuttledemo] answered NUMBER ANSWER   (0 opened, 1 or 2 an action)
 *   [shuttledemo] shuttle KIND RESULT
 *
 * scripts/tests/t_threads.py drives it and reads the screen.
 */

#include <stdio.h>
#include "ugui.h"

#define W 360
#define H 150

static struct ug u;
static struct ug_item room[16];
static int download = 41;

static const char *const weights[3] = { "quiet", "normal", "urgent" };

static void note(int weight)
{
    int id;
    if (weight == PU_QUIET)
        id = pu_notify(&u, PU_QUIET, "Updates", "Darkroom 3.1 is ready", 0, 0);
    else if (weight == PU_NORMAL)
        id = pu_notify(&u, PU_NORMAL, "Ania",
                       "Is the new build on the stick yet?", "Reply", 0);
    else
        id = pu_notify(&u, PU_URGENT, "Alarm", "The tea is ready", "Stop",
                       "Snooze");
    printf("[shuttledemo] sent %s %d\n", weights[weight], id);
    fflush(stdout);
}

static void activity(int kind)
{
    struct pu_activity a = { kind, 0, 0, "", "" };
    const char *what = "end";
    if (kind == PU_TIMER) {
        a.value = 192;
        a.title = "Tea";
        what = "timer";
    } else if (kind == PU_DOWNLOAD) {
        a.value = download;
        a.title = "gstreamer-1.26.tar.xz";
        download = download + 20 > 100 ? 100 : download + 20;
        what = "download";
    } else if (kind == PU_CALL) {
        a.title = "Ania";
        a.sub = "Call";
        a.dye = 0x6A4FD6;
        what = "call";
    }
    printf("[shuttledemo] shuttle %s %d\n", what, pu_shuttle(&u, &a));
    fflush(stdout);
}

static int by_key(int c)
{
    switch (c) {
    case 'q': note(PU_QUIET); return 1;
    case 'n': note(PU_NORMAL); return 1;
    case 'u': note(PU_URGENT); return 1;
    case 't': activity(PU_TIMER); return 1;
    case 'd': activity(PU_DOWNLOAD); return 1;
    case 'c': activity(PU_CALL); return 1;
    case 'e': activity(PU_END); return 1;
    }
    return 0;
}

static const char keys[] = "qnutdce";

int main(void)
{
    u.win = (int)win_create(W, H, "Shuttle demo");
    if (u.win < 0)
        return 1;
    pu_start(&u, u.win, W, H, room, 16);
    static const char *const words[7] = { "Quiet", "Normal", "Urgent",
                                          "Timer", "Download", "Call", "End" };
    for (int i = 0; i < 7; i++)
        pu_button(&u, 12 + (i % 4) * 86, 16 + (i / 4) * 48, 78, 34, words[i]);
    ug_draw(&u);
    printf("[shuttledemo] drawn\n");
    fflush(stdout);
    for (;;) {
        struct win_event ev;
        int redraw = 0;
        while (win_poll(u.win, &ev) == 1) {
            if (ev.type == WEV_LOOK) {
                redraw = 1;
                continue;
            }
            if (ev.type == WEV_KEY && by_key(ev.val & 0xFF))
                continue;
            int which;
            int what = ug_event(&u, &ev, &which);
            if (what == UG_CLOSED)
                return 0;
            if (what == UG_PRESSED && which >= 0 && which < 7)
                by_key(keys[which]);
            if (what == UG_NOTE) {
                printf("[shuttledemo] answered %d %d\n", pu_note_id(&u),
                       pu_note_answer(&u));
                fflush(stdout);
            }
            redraw = 1;
        }
        if (redraw)
            ug_draw(&u);
        sys_sleep(50);
    }
}
