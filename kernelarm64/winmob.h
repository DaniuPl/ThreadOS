#ifndef THREADOS_ARM64_WINMOB_H
#define THREADOS_ARM64_WINMOB_H

#include <stdint.h>

/* Programs' windows on the phone (winmob.c): the PC's window calls, a
   window being the whole screen below the status bar */

/* The PC's struct win_event (user64/usys64.h) and its kinds */
struct win_event {
    int32_t type;
    int32_t x, y;
    int32_t val;
};
#define WEV_CLICK   1
#define WEV_SIZE    4
#define WEV_PRESS   5
#define WEV_DRAG    6
#define WEV_RELEASE 7

#define WINMOB_MAX 8

/* The shell, once it has the screen: where a window goes on the panel
   (its pixels) - the panel's width, from row 'top' to its bottom */
void winmob_init(int width, int top, int bottom);

/* The calls, for the program numbered 'owner' (proc.c) */
int64_t winmob_create(int owner, const char *title);
int64_t winmob_blit32(int win, int owner, int x, int y, int w, int h,
                      const uint32_t *px);
int64_t winmob_poll(int win, int owner, struct win_event *ev);
int64_t winmob_title(int win, int owner, const char *title);
/* Is the window this program's */
int winmob_mine(int win, int owner);
/* The program has ended: its windows go */
void winmob_drop(int owner);

/* The shell's side. Each change - a window made, drawn into, gone -
   moves the count on, so the shell knows to draw again. */
uint32_t winmob_changes(void);
/* A window made since the last ask: its number, or -1 */
int winmob_take_new(void);
int winmob_exists(int win);
int winmob_count(void);
/* The k-th window there is, in the order they were made: its number */
int winmob_nth(int k);
void winmob_name(int win, char *out, int max);
/* Its pixels onto the panel's rows from 'top' down */
void winmob_draw(int win);
/* A finger on it, in the window's own pixels */
void winmob_touch(int win, int kind, int x, int y);

#endif
