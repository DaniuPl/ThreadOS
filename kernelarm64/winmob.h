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
#define WEV_KEY     2
#define WEV_SIZE    4
#define WEV_SIZE_TOUCH 1         /* WEV_SIZE's val: the pointer is a finger;
                                    bits 8-15 the scale, in sixteenths */
#define WEV_PRESS   5
#define WEV_DRAG    6
#define WEV_RELEASE 7
#define WEV_LOOK    10          /* the look changed: draw again in it */

#define WINMOB_MAX 8

/* The shell, once it has the screen: where a window goes on the panel
   (its pixels) - the panel's width, from row 'top' to its bottom */
void winmob_init(int width, int top, int bottom);
/* Where windows end now, above the home indicator's strip or the
   bar of Android's buttons: each is told its new size */
void winmob_set_bottom(int bottom);
/* The screen turned: its new width, and where windows begin and end */
void winmob_reshape(int width, int top, int bottom);

/* The calls, for the program numbered 'owner' (proc.c) */
int64_t winmob_create(int owner, const char *title);
int64_t winmob_blit32(int win, int owner, int x, int y, int w, int h,
                      const uint32_t *px);
int64_t winmob_blit_hi(int win, int owner, int x, int y, int w, int h,
                       const uint32_t *px);
/* WIN_DRAW: a shape or text (the PC's struct win_draw, desktop.h) */
struct win_draw;
int64_t winmob_draw_cmd(int win, int owner, const struct win_draw *d,
                        const char *s);
/* The display size changed: each window is told its new size */
void winmob_rescale(void);
void winmob_look_changed(void);
uint32_t winmob_edge_colour(int win, int bottom);
/* SYS64_WIN_KEYBOARD: the program waits for typing - the shell shows
   its keyboard over the window - or no longer does */
int64_t winmob_set_keyboard(int win, int owner, int on);
int winmob_keyboard(int win);
/* The keyboard's keys, to the window as WEV_KEY bytes */
void winmob_type(int win, const char *s);
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
/* A finger on it, in the panel's pixels from the window's corner */
void winmob_touch(int win, int kind, int x, int y);

#endif
