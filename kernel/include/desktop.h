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
/* Is pid the kiosk's one program - the installer the kernel started */
int desktop_kiosk_is(int pid);

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
/* The window's theme bar (SYS64_WIN_THEMEBAR): light 0 or 1 the theme
   chosen, -1 none; a click comes back as WEV_THEME */
int desktop_win_theme_bar(int winid, int owner, int light);
/* A block of palette indices copied into the window's content - the
   only way to draw a picture without a syscall per pixel. */
int desktop_win_blit(int winid, int owner, int x, int y, int w, int h,
                     const uint8_t *pixels);
/* Asks for the window's pixels to become colours (0x00RRGGBB) rather
   than palette indices. Granted whatever the screen can show - on an
   eight-bit one the compositor trades each colour for the nearest
   index while composing. */
int desktop_win_truecolour(int winid, int owner);
/* A program's window to the whole screen (on) and back */
int desktop_win_full(int winid, int owner, int on);
/* What a program's window plays (state: 0 nothing, 1 playing, 2 paused),
   for the island's play and pause (WEV_MEDIA back to the window) */
/* state: 0 nothing, 1 playing, 2 paused; or'ed with NP_NAMED, the
   island names it by 'title' rather than by the window (usys64.h's
   NOW_NAMED) */
#define NP_NAMED 0x100
int desktop_now_playing(int winid, int owner, int state, const char *title);
/* A block of colours. Only for a window that asked for them. */
int desktop_win_blit32(int winid, int owner, int x, int y, int w, int h,
                       const uint32_t *pixels);
/* The same in the panel's pixels, at whatever scale the desktop draws */
int desktop_win_blit_hi(int winid, int owner, int x, int y, int w, int h,
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
 *   WD_LTEXT   the same as WD_TEXT, but translate a static UI label
 *   WD_LMEASURE the same as WD_MEASURE, but translate a static UI label
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
#define WD_LTEXT   10
#define WD_LMEASURE 11
/* From now on, draw only inside x, y, w, h (in the content's
   coordinates); w or h 0 draws everywhere again. For a web page's boxes
   that hide what overflows them. */
#define WD_CLIP    12
/* A panel in the system's own look over what is drawn there - the
   phone's Liquid Glass, matte or solid as its Settings say; a tint on
   the PC - answering how light it came out (0..255), for what is put
   on it to be dark or light; and the window's pixels held back from
   the screen (w = 1) until they are let go (w = 0), so that a picture
   and what is drawn over it reach the screen together */
#define WD_GLASS   13
#define WD_HOLD    14
/* Weave's marks (ProtonUI v2; kernel/gfx.c says exactly what each is):
 *   WD_GRAIN   the grain's 3x3 tile over x, y, w, h in rgb at r of 255,
 *              counted from the window's corner
 *   WD_STITCH  a dashed line filling x, y, w, h along its longer side,
 *              dashes r long (0: 4)
 *   WD_KNOT    a knot w across at x, y: r 0 hollow, 1 ringed, 2 filled
 *   WD_THREAD  a curve from (x, y) to (w, h), pulled towards the points
 *              packed in font and px (x in the high half, y in the low,
 *              each a signed 16-bit number), r sixteenths of a pixel wide
 */
#define WD_GRAIN   15
#define WD_STITCH  16
#define WD_KNOT    17
#define WD_THREAD  18
#define WD_PACK(x, y)   ((int)((uint32_t)(uint16_t)(x) << 16 | (uint16_t)(y)))
#define WD_PACKED_X(v)  ((int)(int16_t)((uint32_t)(v) >> 16))
#define WD_PACKED_Y(v)  ((int)(int16_t)((uint32_t)(v) & 0xFFFF))

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
/* ProtonUI v2 (user/udraw.h) asks for more after those eleven: the
   desktop's style, Weave's six dyes in the theme, the person's thread
   colour, the grain's ink, a control's corners, the density */
#define UI_LOOK_MAX 22
#define UI_STYLE_BAR_DOCK 0
#define UI_STYLE_TASKBAR  1
#define UI_STYLE_WEAVE    2
#define UI_STYLE_PHONE    3    /* the phone's shell, not a desktop style */
/* SYS64_UI_LOOK's second argument: the theme in its low byte (0 the
   one in use, 1 the dark one, 2 the light one) and in the byte above
   it how many words the program knows - 0 for every program from
   before v2, which gets exactly UI_LOOK_N and nothing past its array */
#define UI_LOOK_THEME(a)  ((int)((a) & 0xFF))
#define UI_LOOK_WORDS(a)  ((int)(((a) >> 8) & 0xFF))
/* Fills UI_LOOK_MAX words */
void desktop_ui_look(int which, uint32_t *out);

/* A background as a w x h picture of 0x00RRGGBB, for a program to show
   before it is chosen: 'bg' as the 'background' setting counts them,
   in the dark theme or the light one. 0, or -1 for no such
   background. At most BG_PREVIEW_MAX pixels. */
#define BG_PREVIEW_MAX (200 * 150)
#define BG_PREVIEW_SIDE 1024      /* each side, checked before w * h */
int desktop_bg_preview(int bg, int dark, int w, int h, uint32_t *out);

/* The compositor's last second: its frames, the share of the second
   spent drawing them (percent), and how many of them were drawn - a
   frame with nothing new in it is not. 0 while it is not running. */
int desktop_frame_stats(int *fps, int *busy, int *drawn);

/* A notification in the island at the top of the screen: something
   worth a glance that nobody asked for. Callable from anywhere. */
void desktop_notify(const char *title, const char *body);
/* A notification with a weight (NOTE_*) and up to two actions, which
   come back to the owner's window as WEV_NOTE (owner -1: the kernel's,
   no window). -> its number (> 0), or -1. Callable from anywhere. */
#define NOTE_QUIET   0           /* straight to Threads: no island */
#define NOTE_NORMAL  1           /* the island for a moment, then Threads */
#define NOTE_URGENT  2           /* the island until it is answered */
#define NOTE_ACT_LEN 16          /* an action's label, with its NUL */
int desktop_note(int owner, int winid, int weight, const char *title,
                 const char *body, const char *act1, const char *act2);
/* What a program has going on, on the shuttle (SYS64_SHUTTLE): kind
   SHUTTLE_*, value a timer's seconds left, a download's per cent (-1
   unknown) or how long a call has run; dye 0x00RRGGBB, 0 = the
   program's own. SHUTTLE_END takes it away. -> 0, or -1 */
#define SHUTTLE_END      0
#define SHUTTLE_TIMER    1
#define SHUTTLE_PROGRESS 2
#define SHUTTLE_CALL     3
int desktop_shuttle(int owner, int winid, int kind, int value, uint32_t dye,
                    const char *title, const char *sub);
/* A package was taken while the desktop runs: its tile, if it asks for
   one, comes with the next frame */
void desktop_packages_changed(void);

/* Locks the screen: nothing but the time and a field for the password
   until it is given. 0, or -1 when there is no desktop to lock. */
int desktop_lock(void);

/* The power button was pressed: ask whether to shut down. -1 = there is
   no desktop to ask on. */
int desktop_power_button(void);

#endif
