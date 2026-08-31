/* gdeaf - an application that does not listen
 *
 * It opens a window and never asks the desktop for anything. Closing
 * such a window is a request that goes to nobody: there is nothing on
 * the other side to hear it.
 *
 * A desktop that only asked would be stuck with the window forever, so
 * after a while it stops asking and kills the process. This program is
 * here to be killed - it is the only way to check that a window nobody
 * can close does not exist.
 */

#include "usys.h"

#define C_BG   8
#define C_RED  1

void _start(void)
{
    int w = win_create(100, 40, "Gluchy");
    if (w < 0)
        sys_exit(1);

    for (;;) {
        win_rect(w, 0, 0, 100, 40, C_BG);
        win_text(w, 4, 4, "nie slucham", C_RED);
        win_text(w, 4, 18, "zamknij mnie", C_RED);
        sys_sleep(200);
    }
}
