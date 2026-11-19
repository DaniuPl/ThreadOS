#ifndef THREADOS_WEAVE_H
#define THREADOS_WEAVE_H

#include <stdint.h>
#include "gfx.h"
#include "look.h"

/* Weave's icons (kernel/weave.c): a tile of the program's dye with the
   cloth's grain and the program's mark in white strokes, from the
   design's own table (docs/weave/canvas/Icon.dc.html). */

/* The icon of that name ("files", "term", ...), or -1 */
int weave_icon_find(const char *name);

/* An icon 's' logical pixels across at x, y on 't' (NULL: the screen),
   square - the caller rounds its corners, by WEAVE_ICON_R percent. An
   icon of -1 is a tile of a dye picked by the letter, with the letter
   on it. */
void weave_icon_draw(const struct gfx_surface *t, int icon, int x, int y,
                     int s, char letter);

/* Only an icon's strokes, in one colour, no tile: the loom's small
   pictures ("g-network", "g-sound", "g-find"). 'stroke' in tenths of a
   unit of the 48 grid. */
void weave_glyph_draw(const struct gfx_surface *t, int icon, int x, int y,
                      int s, uint32_t c, int stroke);

uint32_t weave_icon_dye(int icon);
uint32_t weave_letter_dye(char letter);

#endif
