#ifndef THREADOS_MOUSE_H
#define THREADOS_MOUSE_H

/* PS/2 mouse driver (IRQ12). The position is accumulated from relative
   movements and clamped to the screen range (320x200 by default -
   ready for graphics mode). The state is read through mouse_get. */
/* Sets the device on the auxiliary port up. Returns 1 when there
   really is one - a laptop whose touchpad hangs off I2C rather than
   PS/2 has nothing here, and saying so is the difference between a
   pointer that does not move and a pointer nobody promised. */
int mouse_init(void);
int mouse_present(void);
/* Whether the 8042 controller answers at all. When it does not, there
   is no PS/2 keyboard either - whatever is typing came over USB. */
int ps2_controller_present(void);
void mouse_get(int *x, int *y, int *buttons);
/* Wheel notches since the last call, and then zero until it turns
   again. On a laptop this is two fingers sliding on the touchpad. */
int mouse_take_scroll(void);
int mouse_has_wheel(void);
/* Movement from a pointer that is not on the PS/2 port - a USB mouse,
   or a laptop touchpad on a machine that has no 8042 at all. There is
   one pointer on the screen, so both roads end in the same place. */
void mouse_inject(int dx, int dy, int buttons, int wheel_notches);

#endif
