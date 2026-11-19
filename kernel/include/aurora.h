#ifndef THREADOS_AURORA_H
#define THREADOS_AURORA_H

#include <stdint.h>

/* The desktop's own background (see kernel/aurora.c): drawn once into
   'buf', a picture w x h of 0x00RRGGBB words, 'stride' to a row. */
void aurora_draw(uint32_t *buf, int w, int h, int stride, int dark);

/* Weave's background, the warp (kernel/include/look.h says what it is):
   'thread' is the person's own thread colour, which one of its threads
   is dyed */
void warp_draw(uint32_t *buf, int w, int h, int stride, int dark,
               uint32_t thread);

#endif
