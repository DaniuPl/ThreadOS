#ifndef THREADOS_MARK_H
#define THREADOS_MARK_H

#include <stdint.h>

/* ThreadOS's mark (art/threados-mark.svg, kernel/mark.c), in the middle
   of a d x d box whose top left is (x, y) - logical pixels of the
   surface t (NULL is the screen), drawn at its own size - in colour c,
   a of 255 strong */
struct gfx_surface;
void mark_draw(const struct gfx_surface *t, int x, int y, int d, uint32_t c,
               int a);

#endif
