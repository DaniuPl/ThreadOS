#ifndef PROTONOS_DESKTOP_H
#define PROTONOS_DESKTOP_H

#include <stdint.h>

/* Uruchamia graficzny pulpit (kompozytor) jako watek jadra.
   Przejscie w tryb graficzny jest jednokierunkowe. */
void desktop_start(void);

/* API dla aplikacji ring 3 (wolane z syscalli). owner = pid procesu. */
int desktop_win_create(int owner, int cw, int ch, const char *title);
int desktop_win_rect(int winid, int owner, int x, int y, int w, int h,
                     uint8_t color);
int desktop_win_text(int winid, int owner, int x, int y, const char *s,
                     uint8_t color);

#endif
