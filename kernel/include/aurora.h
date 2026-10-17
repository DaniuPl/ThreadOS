#ifndef THREADOS_AURORA_H
#define THREADOS_AURORA_H

#include <stdint.h>

/* The desktop's own background (see kernel/aurora.c): drawn once into
   'buf', a picture w x h of 0x00RRGGBB words, 'stride' to a row. */
void aurora_draw(uint32_t *buf, int w, int h, int stride, int dark);

#endif
