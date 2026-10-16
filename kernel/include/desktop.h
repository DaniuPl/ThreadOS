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

#endif
