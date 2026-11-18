#ifndef THREADOS_ARM64_TOUCH_H
#define THREADOS_ARM64_TOUCH_H

#include <stdint.h>
#include "virtio.h"

/* The touchscreen: a virtio-input device with absolute axes (touch.c) */

enum { TOUCH_DOWN = 1, TOUCH_MOVE, TOUCH_UP };

struct touch_event {
    int kind;                   /* TOUCH_DOWN, TOUCH_MOVE, TOUCH_UP */
    int x, y;                   /* logical pixels of the screen */
};

/* Takes the first virtio-input device that has absolute X and Y - a
   tablet or a multitouch panel - and maps its range onto a screen of
   w x h logical pixels. 0 = there is one; its name is touch_name(). */
int touch_init(struct virtio_dev *devs, int n, int w, int h);
const char *touch_name(void);
/* The screen's logical size changed (the phone's display size) */
void touch_screen(int w, int h);
/* The next touch, or 0 when there is none waiting. Reads what the device
   has sent first, so it can be called from a loop that sleeps between. */
int touch_next(struct touch_event *e);

#endif
