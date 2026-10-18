#ifndef THREADOS_DESKTOP_H
#define THREADOS_DESKTOP_H

#include <stdint.h>

/* Starts the graphical desktop (the compositor) as a kernel thread.
   The move into graphics mode is one-way. */
void desktop_start(void);

/* The same compositor with everything else taken away: one program,
   the whole screen, no bar, no icons, no menu and no other window.
   This is what installing looks like - while a machine is being
   written to, a taskbar within reach is not a convenience but a way to
   click something else by mistake. The only thing the environment
   itself offers is restarting the machine.

   Has to be called BEFORE the compositor starts; a desktop already on
   the screen cannot become a kiosk. */
int desktop_start_kiosk(const char *program);

/* API for ring 3 applications (called from syscalls). owner = process pid. */
int desktop_win_create(int owner, int cw, int ch, const char *title);
int desktop_win_rect(int winid, int owner, int x, int y, int w, int h,
                     uint8_t color);
int desktop_win_text(int winid, int owner, int x, int y, const char *s,
                     uint8_t color);
int desktop_win_poll(int winid, int owner, void *out);
/* The answer to a close request: "not yet". Returns 1 when there was a
   request to decline, 0 when there was none, -1 for a window that is
   not the caller's. */
int desktop_win_keep(int winid, int owner);
/* The window's name, as the program that owns it wants it read. */
int desktop_win_title(int winid, int owner, const char *title);
/* A block of palette indices copied into the window's content - the
   only way to draw a picture without a syscall per pixel. */
int desktop_win_blit(int winid, int owner, int x, int y, int w, int h,
                     const uint8_t *pixels);
/* Asks for the window's pixels to become colours (0x00RRGGBB) rather
   than palette indices. Granted whatever the screen can show - on an
   eight-bit one the compositor trades each colour for the nearest
   index while composing. */
int desktop_win_truecolour(int winid, int owner);
/* A block of colours. Only for a window that asked for them. */
int desktop_win_blit32(int winid, int owner, int x, int y, int w, int h,
                       const uint32_t *pixels);

/* Drawing as the desktop itself draws: soft-edged shapes and TrueType
 * text, in colours. Positions in whole pixels of the window's content;
 * 'a' is how much of the colour goes on (255 = all of it). A window
 * that still holds palette indices is made a colour window first.
 *
 *   WD_FILL    a rectangle
 *   WD_RRECT   a rectangle with corners of radius r
 *   WD_RING    only its edge, a pixel thick
 *   WD_GRAD    a rounded rectangle sloping from rgb (top) to rgb2
 *   WD_DISC    a circle in the square x, y, w
 *   WD_LINE    from (x, y) to (w, h), r sixteenths of a pixel thick
 *   WD_TEXT    text with its baseline at y, in a face (UI_SANS, UI_BOLD,
 *              UI_MONO) and a size in pixels; returns where the pen ended
 *   WD_MEASURE how wide that text would be - no window needed
 *   WD_CELLS   text on a grid: every character's pen at x + i*w, the
 *              baseline at y - a terminal's columns, which no font's
 *              own advances keep
 */
#define WD_FILL    1
#define WD_RRECT   2
#define WD_RING    3
#define WD_GRAD    4
#define WD_DISC    5
#define WD_LINE    6
#define WD_TEXT    7
#define WD_MEASURE 8
#define WD_CELLS   9

struct win_draw {
    int op;
    int x, y, w, h, r;
    uint32_t rgb, rgb2;
    int a;
    int font, px;
};
int desktop_win_draw(int winid, int owner, const struct win_draw *d,
                     const char *s);

/* The colours a program draws its window in, so that it follows the
   theme as the desktop does - eleven words, in this order: */
#define UI_LOOK_N 11   /* dark?, background, field, text, dim text, line,
                          row, accent, text on accent, danger, good */
/* which: 0 the theme in use, 1 the dark one, 2 the light one */
void desktop_ui_look(int which, uint32_t *out);

/* A background as a w x h picture of 0x00RRGGBB, for a program to show
   before it is chosen: 'bg' as the 'background' setting counts them,
   in the dark theme or the light one. 0, or -1 for no such
   background. At most BG_PREVIEW_MAX pixels. */
#define BG_PREVIEW_MAX (200 * 150)
int desktop_bg_preview(int bg, int dark, int w, int h, uint32_t *out);

/* The compositor's last second: frames drawn, and the share of the
   second spent drawing them (percent). 0 while it is not running. */
int desktop_frame_stats(int *fps, int *busy);

/* A card at the top right of the screen for a few seconds: something
   worth a glance that nobody asked for. Callable from anywhere. */
void desktop_notify(const char *title, const char *body);

/* Locks the screen: nothing but the time and a field for the password
   until it is given. 0, or -1 when there is no desktop to lock. */
int desktop_lock(void);

#endif
