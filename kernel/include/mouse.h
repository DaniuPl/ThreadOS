#ifndef PROTONOS_MOUSE_H
#define PROTONOS_MOUSE_H

/* PS/2 mouse driver (IRQ12). The position is accumulated from relative
   movements and clamped to the screen range (320x200 by default -
   ready for graphics mode). The state is read through mouse_get. */
void mouse_init(void);
void mouse_get(int *x, int *y, int *buttons);

#endif
