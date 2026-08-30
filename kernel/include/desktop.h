#ifndef PROTONOS_DESKTOP_H
#define PROTONOS_DESKTOP_H

#include <stdint.h>

/* Starts the graphical desktop (the compositor) as a kernel thread.
   The move into graphics mode is one-way. */
void desktop_start(void);

/* API for ring 3 applications (called from syscalls). owner = process pid. */
int desktop_win_create(int owner, int cw, int ch, const char *title);
int desktop_win_rect(int winid, int owner, int x, int y, int w, int h,
                     uint8_t color);
int desktop_win_text(int winid, int owner, int x, int y, const char *s,
                     uint8_t color);
int desktop_win_poll(int winid, int owner, void *out);

#endif
